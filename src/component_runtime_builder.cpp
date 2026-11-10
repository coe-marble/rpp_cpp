#include "rpp_cpp/component_runtime_builder.hpp"
#include "rpp_cpp/rpp_server_host.hpp"
#include "rpp_cpp/plugin_loader.hpp"

#include <set>
#include <stdexcept>
#include <utility>

namespace rpp
{
using detail::ComponentBuildRequest;

template<class Threading>
BasicComponentRuntimeBuilder<Threading>::~BasicComponentRuntimeBuilder() = default;

template<class Threading>
BasicComponentRuntimeBuilder<Threading>::BasicComponentRuntimeBuilder(
  rpp::RppDataManager & data_manager,
  const ClockOptions & clock_options, bool allow_dynamic_components,
  RegistrationValidators validators)
: data_manager_own_(nullptr),
  data_manager_(data_manager),
  clock_options_(clock_options), dynamic_components_(allow_dynamic_components),
  registration_validators_(std::move(validators)) {}

template<class Threading>
BasicComponentRuntimeBuilder<Threading>::BasicComponentRuntimeBuilder(
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

template<class Threading>
BasicComponentRuntimeBuilder<Threading>::BasicComponentRuntimeBuilder(
  const ClockOptions & clock_options,
  bool allow_dynamic_components, RegistrationValidators validators)
: data_manager_own_(std::make_shared<rpp::RppDataManager>()),
  data_manager_(*data_manager_own_.get()),
  clock_options_(clock_options), dynamic_components_(allow_dynamic_components),
  registration_validators_(std::move(validators)) {}

template<class Threading>
void BasicComponentRuntimeBuilder<Threading>::register_external_component_server(
  const std::string & language, const std::string & command)
{
  external_component_process_registry_.register_server(language, command);
}

template<class Threading>
std::shared_ptr<ComponentConstructor<Threading>>
BasicComponentRuntimeBuilder<Threading>::create_constructor() const
{
  auto options = std::make_shared<typename ComponentConstructor<Threading>::BuildOptions>();
  options->data = std::make_shared<RppDataManager>(data_manager_);
  options->clock = clock_options_;
  if (const auto * enabled = std::get_if<bool>(&dynamic_components_)) {
    options->dynamic_policy = [flag = *enabled](const auto &, const auto &) {return flag;};
  } else {
    options->dynamic_policy = std::get<DynamicComponentPolicy>(dynamic_components_);
  }
  options->validators = registration_validators_;
  options->external_servers =
    std::make_shared<ExternalComponentProcessRegistry>(external_component_process_registry_);
  return std::make_shared<ComponentConstructor<Threading>>(std::move(options));
}

template<class Threading>
rpp::ComponentRuntime<Threading::concurrent> BasicComponentRuntimeBuilder<Threading>::build_component(
  const ComponentRequest & request)
{
  auto runtimes = build_components({request});
  return std::move(runtimes.front());
}

template<class Threading>
std::vector<rpp::ComponentRuntime<Threading::concurrent>> BasicComponentRuntimeBuilder<Threading>::build_components(
  const std::vector<ComponentRequest> & requests)
{
  std::vector<ComponentBuildRequest> build_requests;
  build_requests.reserve(requests.size());
  for (const auto & request : requests) {
    build_requests.push_back({"", request.path, request.path, request.plugin_name, {}});
  }
  auto constructor = create_constructor();
  auto components = constructor->construct_components(build_requests);
  std::vector<ComponentRuntime> runtimes;
  runtimes.reserve(components.size());
  for (auto & component : components) {
    auto manager = std::make_shared<SlotManager>(
      typename SlotManager::RuntimeFactory{}, ActivationPolicy::EXCLUSIVE, false);
    manager->constructor_ = constructor;
    runtimes.push_back(SlotManager::create_root(std::move(component), std::move(manager)));
  }
  return runtimes;
}

template<class Threading>
rpp::ScriptRuntime<Threading::concurrent> BasicComponentRuntimeBuilder<Threading>::build_script(
  const ScriptRequest & request)
{
  std::string description_path;
  if (const auto * path = std::get_if<ScriptPath>(&request.source)) {
    description_path = data_manager_.get_default_script_description_path(path->path);
  } else if (const auto * library = std::get_if<LibraryScript>(&request.source)) {
    description_path = data_manager_.get_script_description_path_from_library(
      library->library_name, library->script_name);
  } else {
    description_path = std::get<ScriptDescriptionPath>(request.source).path;
  }
  return ScriptRuntime(create_constructor()->build_script(
    description_path, request.configuration, request.parts_folder));
}

template<class Threading>
std::unique_ptr<BasicRppServerHost<Threading>>
BasicComponentRuntimeBuilder<Threading>::build_component_server(
  const ComponentServerRequest & request)
{
  if (request.slots.empty()) {
    throw std::invalid_argument("A component server requires at least one hosted slot");
  }
  auto server = std::make_unique<BasicRppServerHost<Threading>>(
    request.host, request.port, request.logger);
  std::set<std::string> connection_names;
  for (const auto & slot_request : request.slots) {
    if (slot_request.connection_name.empty() || !connection_names.insert(
        slot_request.connection_name).second)
    {
      throw std::invalid_argument("Hosted slot connection names must be nonempty and unique");
    }
    if (slot_request.components.empty()) {
      throw std::invalid_argument("Each hosted slot requires an initial component");
    }
    const auto description = parse_component_slot_type(slot_request.declaration);
    const bool dynamic = std::holds_alternative<bool>(dynamic_components_) ?
      std::get<bool>(dynamic_components_) :
      std::get<DynamicComponentPolicy>(dynamic_components_)(slot_request.connection_name, description);
    auto manager = std::make_shared<SlotManager>(
      typename SlotManager::RuntimeFactory{}, description.activation_policy(), dynamic,
      description.plugin_type);
    auto constructor = create_constructor();
    manager->constructor_ = constructor;
    std::optional<PluginInfo> adapter_plugin_info;
    for (const auto & component : slot_request.components) {
      ComponentBuildRequest build_request{
        "", component.path, component.path, component.plugin_name, {}};
      auto record = constructor->get_component_record(build_request);
      if (record.parent_component_info) {
        throw LifecycleError("A subcomponent cannot be registered in a component server slot");
      }
      auto plugin_info = constructor->get_plugin_info(record);
      if (!is_cpp_source_language(plugin_info))
      {
        throw std::invalid_argument("The C++ component server can only host C++ components");
      }
      if (!adapter_plugin_info) {adapter_plugin_info = std::move(plugin_info);}
      build_request.name = record.name;
      manager->initialize(build_request, {}).require_success(
        "register " + slot_request.connection_name);
    }
    auto runtime = manager->selected_runtime();
    if (!runtime) {
      throw LifecycleError("Hosted slot has no selected initial component");
    }
    auto adapter = load_plugin_adapter_server(
      *adapter_plugin_info, runtime->template get_instance<Plugin>(),
      slot_request.connection_name + "_server", slot_request.connection_name,
      "create_plugin_server", request.logger);
    server->add_slot(
      std::move(adapter), manager,
      [manager, constructor](const std::string & path, const std::string & plugin_name) {
        ComponentBuildRequest build_request{
          "", path, path, plugin_name, {}};
        auto record = constructor->get_component_record(build_request);
        if (record.parent_component_info) {
          return CallbackResult::failure(
            "A subcomponent cannot be loaded into a component server slot");
        }
        if (!is_cpp_source_language(constructor->get_plugin_info(record)))
        {
          return CallbackResult::failure(
            "The C++ component server can only host C++ components");
        }
        build_request.name = record.name;
        return manager->load(build_request.name, build_request.path, build_request.plugin_name);
      });
  }
  return server;
}

template class BasicComponentRuntimeBuilder<SingleThreaded>;
template class BasicComponentRuntimeBuilder<Concurrent>;
}  // namespace rpp
