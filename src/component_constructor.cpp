#include "rpp_cpp/component_constructor.hpp"
#include "rpp_cpp/slot_manager.hpp"

#include <set>
#include <array>
#include <random>
#include <stdexcept>
#include <utility>
#include <variant>

#include "rpp_cpp/parameter_handler.hpp"
#include "rpp_cpp/plugin_loader.hpp"
#include "rpp_cpp/data_manager.hpp"
#include "rpp_cpp/external_component_process.hpp"
#include "rpp_cpp/rpp_paths.hpp"

namespace rpp
{
// One slot-construction operation: declare the slot, then let its manager
// construct and admit each initial entry through the ordinary load path.
template<class Threading>
typename ComponentConstructor<Threading>::SlotManagers ComponentConstructor<Threading>::create_slots(
  const SlotBuildRequests & slot_build_requests, const std::map<std::string, std::string> & spec,
  const std::shared_ptr<BuildOptions> & options, const PreparedComponents & prepared,
  RuntimeFactory factory, const std::map<std::string, Validator> & validators)
{
  std::map<std::string, size_t> counts;
  for (const auto & [slot, entries] : slot_build_requests) {counts.emplace(slot, entries.size());}
  for (const auto & [slot, declaration] : spec) {
    (void)declaration;
    counts.try_emplace(slot, 0);
  }
  SlotManagers slot_managers;
  for (const auto & [name, count] : counts) {
    auto declaration = spec.find(name);
    ComponentSlot description{"", count > 1 ? SlotKind::LIST : SlotKind::SINGLE};
    if (declaration != spec.end()) {description = parse_component_slot_type(declaration->second);}
    if (description.kind == SlotKind::SINGLE && count > 1) {
      throw LifecycleError("Single slot '" + name + "' must be constructed with one component");
    }
    const bool dynamic = options->dynamic_policy && options->dynamic_policy(name, description);
    auto validator = validators.find(name);
    auto named = options->validators.find(description.plugin_type);
    auto manager = std::make_shared<SlotManager>(
      factory, description.activation_policy(), dynamic, description.plugin_type,
      validator == validators.end() ? Validator{} : validator->second,
      named == options->validators.end() ? NamedValidators{} : named->second);
    manager->constructor_ = std::make_shared<ComponentConstructor>(options);
    auto entries = slot_build_requests.find(name);
    if (entries != slot_build_requests.end()) {
      for (auto request : entries->second) {
        if (request.name.empty() && !factory) {
          request.name = manager->constructor_->get_component_record(request).name;
        }
        manager->initialize(request, prepared).require_success("register " + name);
      }
    }
    slot_managers.emplace(name, std::move(manager));
  }
  return slot_managers;
}

template<class Threading>
ComponentRecord ComponentConstructor<Threading>::get_component_record(
  const ComponentBuildRequest & request) const
{
  auto component_record = build_options_->data->load_component_info(request.path);
  if (std::holds_alternative<LinkedComponentRecord>(component_record)) {
    auto linked_record = std::get<LinkedComponentRecord>(component_record);
    if (request.plugin_name.empty()) {
      throw std::runtime_error(
              "Plugin name must be provided for linked components.");
    }
    auto linked_component_path = build_options_->data->get_linked_component_folder_path(
      request.parent_path, request.plugin_name, linked_record.linked_component_id);
    auto linked_component_record =
      build_options_->data->load_component_info(linked_component_path);
    if (std::holds_alternative<ComponentRecord>(linked_component_record)) {
      return std::get<ComponentRecord>(linked_component_record);
    } else {
      throw std::runtime_error("Doubly linked components are not supported.");
    }
  } else if (std::holds_alternative<ComponentRecord>(component_record)) {
    return std::get<ComponentRecord>(component_record);
  } else {
    throw std::runtime_error("Invalid component record type.");
  }
}

template<class Threading>
void ComponentConstructor<Threading>::discover_external_components(
  const ComponentBuildRequest & request,
  std::vector<ComponentBuildRequest> & external_components) const
{
  const auto record = get_component_record(request);
  const auto plugin_info = build_options_->data->get_plugin_info_from_lib(record.plugin_name);
  if (!is_cpp_source_language(plugin_info)) {
    external_components.push_back(request);
    return;
  }
  for (const auto & [slot_name, subcomponents] : record.subcomponents) {
    (void)slot_name;
    for (const auto & subcomponent : subcomponents) {
      discover_external_components(
        {"", build_options_->data->get_subcomponent_folder_path(record.folder, subcomponent.id),
          record.folder, subcomponent.plugin_name}, external_components);
    }
  }
}

template<class Threading>
typename ComponentConstructor<Threading>::PreparedComponents
ComponentConstructor<Threading>::prepare_processes_for_non_native_components(
  const std::vector<ComponentBuildRequest> & requests) const
{
  // Each external host owns discovery and construction of its own descendants.
  std::vector<ComponentBuildRequest> external_components;
  for (const auto & request : requests) {discover_external_components(request, external_components);}

  std::map<std::string, std::vector<ExternalComponentSpec>> groups;
  std::set<std::string> connection_names;
  for (const auto & request : external_components) {
    const auto record = get_component_record(request);
    const auto plugin_info = build_options_->data->get_plugin_info_from_lib(record.plugin_name);
    const auto connection_name = record.id + "_connection";
    if (!connection_names.insert(connection_name).second) {
      throw std::runtime_error("Duplicate adapted component connection name '" + connection_name + "'.");
    }
    groups[to_lower_copy(plugin_info.source_language)].push_back(
      {record.folder, plugin_info, connection_name});
  }

  PreparedComponents prepared_components;
  for (const auto & [language, component_specs] : groups) {
    auto prepared = ExternalComponentProcess::create(
      language, RPP_HOME, component_specs, *build_options_->external_servers);
    for (size_t index = 0; index < component_specs.size(); ++index) {
      prepared_components[component_specs[index].component_path] = {
        std::move(prepared.instances[index]), prepared.process};
    }
  }
  return prepared_components;
}

template<class Threading>
std::unique_ptr<detail::ComponentData<Threading>> ComponentConstructor<Threading>::construct_component(
  const ComponentRecord & record, const PluginInfo & plugin_info,
  const PreparedComponents & prepared) const
{
  const bool native = is_cpp_source_language(plugin_info);
  std::shared_ptr<Plugin> instance;
  std::shared_ptr<ExternalComponentProcess> process;
  if (native) {
    instance = load_cpp_plugin_from_shared_library(plugin_info);
  } else {
    auto transport = prepared.find(record.folder);
    if (transport == prepared.end()) {
      throw LifecycleError("External component transport was not prepared before construction");
    }
    instance = transport->second.instance;
    process = transport->second.process;
  }
  std::unique_ptr<params::Parameters> parameters;
  {
    params::ParameterHandler handler(record.folder);
    auto module = handler.load_parameters_from_python_module();
    params::ParameterHandler::resolve_params(plugin_info.plugin_metadata.parameters, module, parameters);
  }
  SlotManagers slot_managers;
  if (native) {
    SlotBuildRequests slot_build_requests;
    for (const auto & [slot, assignments] : record.subcomponents) {
      auto & entries = slot_build_requests[slot];
      for (const auto & assignment : assignments) {
        const auto path = build_options_->data->get_subcomponent_folder_path(record.folder, assignment.id);
        entries.push_back({"", path, record.folder, assignment.plugin_name});
      }
    }
    slot_managers = create_slots(slot_build_requests, record.subcomponent_spec, build_options_, prepared);
  }
  // An external host owns its subcomponent tree. Its local proxy has no subcomponent managers.
  auto component = std::make_unique<ComponentData>(std::move(instance), std::move(*parameters),
    std::move(process), std::move(slot_managers), build_options_->clock);
  component->component_name_ = record.name;
  component->component_reference_ = record.folder;
  component->plugin_type_ = record.plugin_type;
  return component;
}

template<class Threading>
std::vector<std::unique_ptr<detail::ComponentData<Threading>>> ComponentConstructor<Threading>::construct_components(
  const std::vector<ComponentBuildRequest> & requests, const PreparedComponents * prepared) const
{
  PreparedComponents owned_processes;
  if (!prepared) {
    owned_processes = prepare_processes_for_non_native_components(requests);
    prepared = &owned_processes;
  }
  std::vector<std::unique_ptr<ComponentData>> components;
  components.reserve(requests.size());
  for (const auto & request : requests) {
    const auto record = get_component_record(request);
    const auto plugin_info = build_options_->data->get_plugin_info_from_lib(record.plugin_name);
    components.push_back(construct_component(record, plugin_info, *prepared));
  }
  return components;
}

template<class Threading>
std::unique_ptr<detail::ComponentData<Threading>> ComponentConstructor<Threading>::build_script(
  const std::string & description_path, std::optional<std::string> configuration,
  std::optional<std::string> parts_folder) const
{
  auto & data = *build_options_->data;
  const auto description = data.load_script_description(description_path);
  const auto & assignments = description.get_components(configuration);
  const auto folder = parts_folder ? *parts_folder :
    data.get_default_script_parts_folder_path_from_description(description_path);
  SlotBuildRequests slot_build_requests;
  std::vector<ComponentBuildRequest> requests;
  for (const auto & [slot, components] : assignments) {
    auto & entries = slot_build_requests[slot];
    for (const auto & component : components) {
      const auto path = data.get_component_path_in_parts_folder(
        folder, component.plugin_name, component.id);
      ComponentBuildRequest request{"", path, path, component.plugin_name};
      request.name = get_component_record(request).name;
      entries.push_back(request);
      requests.push_back(std::move(request));
    }
  }
  auto prepared = prepare_processes_for_non_native_components(requests);
  return std::make_unique<ComponentData>(nullptr, params::Parameters{}, nullptr,
    create_slots(slot_build_requests, description.spec, build_options_, prepared), build_options_->clock);
}

template<class Threading>
detail::ComponentData<Threading>::ComponentData(std::shared_ptr<Plugin> instance,
  params::Parameters parameters, std::shared_ptr<ExternalComponentProcess> process,
  SlotManagers slots, const ClockOptions & clock_options)
: parameters_(std::move(parameters)), subcomponent_slots_(std::move(slots)),
  instance_id_(instance ? create_instance_id() : std::string{}), external_component_process_(std::move(process)),
  instance_(std::move(instance)), logger_(std::make_shared<RppLogger>()),
  clock_(ClockFactory::create_clock(clock_options))
{}

template<class Threading>
std::string detail::ComponentData<Threading>::create_instance_id()
{
  // Generate a UUIDv4 only when component data is constructed, never on plugin method calls.
  std::array<unsigned char, 16> bytes;
  std::random_device entropy;
  std::uniform_int_distribution<unsigned int> byte(0, 255);
  for (auto & value : bytes) {
    value = static_cast<unsigned char>(byte(entropy));
  }
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
  constexpr char hex[] = "0123456789abcdef";
  std::string id;
  id.reserve(36);
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {id.push_back('-');}
    id.push_back(hex[bytes[i] >> 4]);
    id.push_back(hex[bytes[i] & 0x0f]);
  }
  return id;
}

template struct detail::ComponentData<SingleThreaded>;
template struct detail::ComponentData<Concurrent>;

template class ComponentConstructor<SingleThreaded>;
template class ComponentConstructor<Concurrent>;
}  // namespace rpp
