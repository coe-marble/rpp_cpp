#include "rpp_cpp/context_builder.hpp"

#include <set>
#include <stdexcept>
#include <utility>

#include "rpp_cpp/parameter_handler.hpp"
#include "rpp_cpp/plugin_loader.hpp"
#include "rpp_cpp/network_utils.hpp"
#include "rpp_cpp/rpp_paths.hpp"

namespace rpp
{

ComponentContextBuilder::~ComponentContextBuilder() = default;

ComponentContextBuilder::ComponentContextBuilder(
  rpp::RppDataManager & data_manager,
  const ClockOptions & clock_options, bool allow_dynamic_components,
  RegistrationValidators validators)
: data_manager_own_(nullptr),
  data_manager_(data_manager),
  clock_options_(clock_options), dynamic_components_(allow_dynamic_components),
  registration_validators_(std::move(validators)) {}

ComponentContextBuilder::ComponentContextBuilder(
  rpp::RppDataManager & data_manager, DynamicComponentPolicy policy,
  const ClockOptions & clock_options, RegistrationValidators validators)
: data_manager_own_(nullptr), data_manager_(data_manager), clock_options_(clock_options),
  dynamic_components_(std::move(policy)),
  registration_validators_(std::move(validators))
{
  if (!std::get<DynamicComponentPolicy>(dynamic_components_)) {
    throw std::invalid_argument("Dynamic component policy must not be empty");
  }
}

ComponentContextBuilder::ComponentContextBuilder(
  const ClockOptions & clock_options,
  bool allow_dynamic_components, RegistrationValidators validators)
: data_manager_own_(std::make_shared<rpp::RppDataManager>()),
  data_manager_(*data_manager_own_.get()),
  clock_options_(clock_options), dynamic_components_(allow_dynamic_components),
  registration_validators_(std::move(validators)) {}

void ComponentContextBuilder::register_external_component_server(
  const std::string & language, const std::string & command)
{
  external_component_process_registry_.register_server(language, command);
}

ComponentContext ComponentContextBuilder::build_component_from_path(
  const std::string & component_path)
{
  const auto prepared_components = prepare_component_processes(
    {
      make_component_node(component_path, component_path, "")
    });
  return build_for_component(component_path, prepared_components);
}

std::vector<ComponentContext> ComponentContextBuilder::build_from_component_roots(
  const std::vector<ComponentRoot> & roots)
{
  std::vector<ComponentNode> root_nodes;
  root_nodes.reserve(roots.size());
  for (const auto & root : roots) {
    root_nodes.push_back(
      make_component_node(
        root.path, root.path, root.plugin_name));
  }
  const auto prepared_components = prepare_component_processes(root_nodes);

  std::vector<ComponentContext> contexts;
  contexts.reserve(root_nodes.size());
  for (const auto & root : root_nodes) {
    contexts.push_back(
      build_for_component(
        root.path, prepared_components, root.parent_path, root.plugin_name));
  }
  return contexts;
}

ComponentContext ComponentContextBuilder::build_script_from_path(
  const std::string & script_path,
  std::optional<std::string> configuration)
{
  return build_script_from_description_path(
    data_manager_.get_default_script_description_path(script_path),
    std::move(configuration));
}

ComponentContext ComponentContextBuilder::build_script_from_library(
  const std::string & library_name,
  const std::string & script_name,
  std::optional<std::string> configuration)
{
  return build_script_from_description_path(
    data_manager_.get_script_description_path_from_library(
      library_name, script_name),
    std::move(configuration));
}

ComponentContext ComponentContextBuilder::build_script_from_description_path(
  const std::string & script_description_path,
  std::optional<std::string> configuration,
  std::optional<std::string> parts_folder)
{
  const auto script_description = data_manager_.load_script_description(
    script_description_path);
  const auto & components = script_description.get_components(configuration);
  const auto resolved_parts_folder = parts_folder.has_value() ?
    *parts_folder :
    data_manager_.get_default_script_parts_folder_path_from_description(
    script_description_path);

  std::vector<ComponentNode> roots;
  for (const auto & [slot_name, assigned_components] : components) {
    (void)slot_name;
    for (const auto & component : assigned_components) {
      const auto component_path =
        data_manager_.get_component_path_in_parts_folder(
        resolved_parts_folder, component.plugin_name, component.id);
      roots.push_back(
        make_component_node(
          component_path, component_path, component.plugin_name));
    }
  }
  const auto prepared_components = prepare_component_processes(roots);

  std::map<std::string, std::vector<ComponentContext>> subcomponents;
  for (const auto & [slot_name, assigned_components] : components) {
    auto & slot_subcomponents = subcomponents[slot_name];
    for (const auto & component : assigned_components) {
      const auto component_path =
        data_manager_.get_component_path_in_parts_folder(
        resolved_parts_folder, component.plugin_name, component.id);
      slot_subcomponents.push_back(
        build_for_component(
          component_path, prepared_components, component_path, component.plugin_name));
    }
  }
  auto managers = make_slot_managers(
    std::move(subcomponents), script_description.spec, make_factory(), dynamic_policy(),
    registration_validators_);
  return ComponentContext(std::move(managers), clock_options_);
}

ComponentContextBuilder::DynamicComponentPolicy ComponentContextBuilder::dynamic_policy() const
{
  if (const auto * flag = std::get_if<bool>(&dynamic_components_)) {
    return [enabled = *flag](const std::string &, const ComponentSlot &) {return enabled;};
  }
  return std::get<DynamicComponentPolicy>(dynamic_components_);
}

ComponentContext::SubcomponentSlots ComponentContextBuilder::make_slot_managers(
  SubcomponentMap subcomponents,
  const std::map<std::string, std::string> & spec,
  const ComponentManager::ContextFactory & factory,
  const DynamicComponentPolicy & dynamic_policy,
  const RegistrationValidators & named_validators,
  const std::map<std::string, ComponentManager::Validator> & validators)
{
  ComponentContext::SubcomponentSlots managers;
  for (const auto & [name, declaration] : spec) {
    (void)declaration;
    subcomponents.try_emplace(name);
  }
  for (auto & [name, children] : subcomponents) {
    auto declaration = spec.find(name);
    ComponentSlot description{"", children.size() > 1 ? SlotKind::LIST : SlotKind::SINGLE};
    if (declaration != spec.end()) {description = parse_component_slot_type(declaration->second);}
    if (description.kind == SlotKind::SINGLE && children.size() > 1) {
      throw LifecycleError("Single slot '" + name + "' must be constructed with one component");
    }
    const bool slot_dynamic = dynamic_policy && dynamic_policy(name, description);
    auto validator = validators.find(name);
    auto named = named_validators.find(description.plugin_type);
    auto manager = std::make_shared<ComponentManager>(
      factory,
      description.activation_policy(),
      slot_dynamic, description.plugin_type, validator == validators.end() ?
        ComponentManager::Validator{} : validator->second,
      named == named_validators.end() ?
        ComponentManager::NamedValidators{} : named->second
    );

    for (size_t i = 0; i < children.size(); ++i) {
      auto child = std::make_shared<ComponentContext>(std::move(children[i]));
      auto entry_name = child->component_name_;
      auto reference = child->component_reference_;
      manager->adopt(
        entry_name, std::move(child), reference,
        true).require_success("register " + name);
    }
    managers.emplace(name, std::move(manager));
  }
  return managers;
}

ComponentManager::ContextFactory ComponentContextBuilder::make_factory() const
{
  if (const auto * flag = std::get_if<bool>(&dynamic_components_); flag && !*flag) {return {};}
  auto data = std::make_shared<RppDataManager>(data_manager_);
  auto clock = clock_options_;
  auto registry = external_component_process_registry_;
  auto validators = registration_validators_;
  auto dynamic = dynamic_components_;
  // Contexts may outlive this builder. Capture owned build settings rather than
  // this, and create a builder when a manager later requests a load or reload.
  return [data, clock, registry, validators, dynamic](const std::string & path) {
           ComponentContextBuilder builder(*data, clock, false, validators);
           builder.dynamic_components_ = dynamic;
           builder.external_component_process_registry_ = registry;
           return builder.build_component_from_path(path);
         };
}

ComponentRecord ComponentContextBuilder::resolve_component(
  const std::string & component_path,
  const std::string & parent_component_path,
  const std::string & plugin_name) const
{
  auto component_record = data_manager_.load_component_info(component_path);
  if (std::holds_alternative<LinkedComponentRecord>(component_record)) {
    auto linked_record = std::get<LinkedComponentRecord>(component_record);
    if (plugin_name.empty()) {
      throw std::runtime_error(
              "Plugin name must be provided for linked components.");
    }
    auto linked_component_path = data_manager_.get_linked_component_folder_path(
      parent_component_path, plugin_name, linked_record.linked_component_id);
    auto linked_component_record =
      data_manager_.load_component_info(linked_component_path);
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

ComponentContext ComponentContextBuilder::build_native_component(
  const rpp::ComponentRecord & record,
  const rpp::PluginInfo & plugin_info,
  const PreparedComponents & prepared_components) const
{
  auto instance =
    rpp::load_cpp_plugin_from_shared_library(plugin_info);
  std::unique_ptr<rpp::params::Parameters> params;
  {
    // In scope to ensure ParameterHandler is destroyed before returning
    params::ParameterHandler parameter_handler(record.folder);
    auto params_module = parameter_handler.load_parameters_from_python_module();
    params::ParameterHandler::resolve_params(
      plugin_info.plugin_metadata.parameters, params_module, params);
  }

  std::map<std::string, std::vector<rpp::ComponentContext>> subcomponents;
  for (const auto & [slot_name, subcomponent_infos] : record.subcomponents) {
    auto & slot_subcomponents = subcomponents[slot_name];
    for (const auto & subcomponent_info : subcomponent_infos) {
      auto subcomponent_path = data_manager_
        .get_subcomponent_folder_path(
        record.folder, subcomponent_info.id);
      slot_subcomponents.push_back(
        build_for_component(
          subcomponent_path, prepared_components, record.folder,
          subcomponent_info.plugin_name));
    }
  }
  auto managers = make_slot_managers(
    std::move(subcomponents), record.subcomponent_spec, make_factory(), dynamic_policy(),
    registration_validators_);
  auto context = ComponentContext(
    std::shared_ptr<Plugin>(std::move(instance)), std::move(*params), nullptr,
    std::move(managers), clock_options_);
  context.component_name_ = record.name;
  context.component_reference_ = record.folder;
  context.plugin_type_ = record.plugin_type;
  return context;

}

ComponentContextBuilder::ComponentNode ComponentContextBuilder::make_component_node(
  const std::string & component_path,
  const std::string & parent_component_path,
  const std::string & plugin_name) const
{
  auto record = resolve_component(
    component_path, parent_component_path, plugin_name);
  auto plugin_info = data_manager_.get_plugin_info_from_lib(
    record.plugin_name);
  return {
    component_path, parent_component_path, plugin_name,
    record, plugin_info,
  };
}

void ComponentContextBuilder::collect_component_nodes(
  const ComponentNode & node,
  std::vector<ComponentNode> & nodes) const
{
  if (!is_cpp_source_language(node.plugin_info)) {
    nodes.push_back(node);
    return;
  }
  nodes.push_back(node);
  for (const auto & [slot_name, subcomponents] : node.record.subcomponents) {
    (void)slot_name;
    for (const auto & subcomponent : subcomponents) {
      collect_component_nodes(
        make_component_node(
          data_manager_.get_subcomponent_folder_path(
            node.record.folder, subcomponent.id),
          node.path,
          subcomponent.plugin_name),
        nodes);
    }
  }
}

ComponentContextBuilder::PreparedComponents ComponentContextBuilder::prepare_component_processes(
  const std::vector<ComponentNode> & roots) const
{
  // Collect the root components and their subcomponents recursively, preparing
  // external processes only for non-native components. Each external host builds
  // its own subtree. The returned instances are passed to build_for_component
  // for constructing the context tree.
  PreparedComponents prepared_components;
  std::vector<ComponentNode> nodes;
  for (const auto & root : roots) {
    collect_component_nodes(root, nodes);
  }

  std::map<std::string, std::vector<ComponentNode>> groups;
  for (auto & node : nodes) {
    if (!is_cpp_source_language(node.plugin_info)) {
      groups[to_lower_copy(node.plugin_info.source_language)].push_back(
        std::move(node));
    }
  }

  std::set<std::string> connection_names;
  for (auto & [language, group] : groups) {
    std::vector<ExternalComponentSpec> component_specs;
    component_specs.reserve(group.size());
    for (const auto & node : group) {
      const auto connection_name = node.record.id + "_connection";
      if (!connection_names.insert(connection_name).second) {
        throw std::runtime_error(
                "Duplicate adapted component connection name '" +
                connection_name + "'.");
      }
      component_specs.push_back(
        {
          node.record.folder,
          node.plugin_info,
          connection_name,
        });
    }
    auto prepared = ExternalComponentProcess::create(
      language, RPP_HOME, component_specs,
      external_component_process_registry_);
    for (size_t index = 0; index < group.size(); ++index) {
      prepared_components[group[index].record.folder] = {
        std::move(prepared.instances[index]), prepared.process,
      };
    }
  }
  return prepared_components;
}

ComponentContext ComponentContextBuilder::build_for_component(
  std::string component_path,
  const PreparedComponents & prepared_components,
  std::string parent_component_path,
  std::string plugin_name) const
{
  if (parent_component_path.empty()) {
    parent_component_path = component_path;
  }
  auto record = resolve_component(
    component_path,
    parent_component_path, plugin_name);

  rpp::PluginInfo plugin_info =
    data_manager_.get_plugin_info_from_lib(record.plugin_name);
  if (is_cpp_source_language(plugin_info)) {
    return build_native_component(
      record, plugin_info, prepared_components);
  }
  auto prepared = prepared_components.find(record.folder);
  if (prepared == prepared_components.end()) {
    throw std::runtime_error(
            "Adapted component was not prepared before context construction.");
  }

  std::unique_ptr<params::Parameters> params;
  params::ParameterHandler parameter_handler(record.folder);
  auto params_module = parameter_handler.load_parameters_from_python_module();
  params::ParameterHandler::resolve_params(
    plugin_info.plugin_metadata.parameters, params_module, params);
  std::map<std::string, std::vector<ComponentContext>> subcomponents;
  auto managers = make_slot_managers(
    std::move(subcomponents), record.subcomponent_spec, make_factory(), dynamic_policy(),
    registration_validators_);
  auto context = ComponentContext(
    prepared->second.instance, std::move(*params),
    prepared->second.process, std::move(managers), clock_options_);
  context.component_name_ = record.name;
  context.component_reference_ = record.folder;
  context.plugin_type_ = record.plugin_type;
  return context;
}

}  // namespace rpp
