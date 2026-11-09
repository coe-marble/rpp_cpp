#include "rpp_cpp/component_runtime_builder.hpp"

#include <stdexcept>
#include <utility>

namespace rpp
{

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
rpp::ComponentRuntime<Threading::concurrent> BasicComponentRuntimeBuilder<Threading>::build_component_from_path(
  const std::string & component_path)
{
  auto runtimes = build_from_component_roots({{component_path, ""}});
  return std::move(runtimes.front());
}

template<class Threading>
std::vector<rpp::ComponentRuntime<Threading::concurrent>> BasicComponentRuntimeBuilder<Threading>::build_from_component_roots(
  const std::vector<ComponentRoot> & roots)
{
  std::vector<typename ComponentConstructor<Threading>::ComponentBuildRequest> requests;
  requests.reserve(roots.size());
  for (const auto & root : roots) {requests.push_back({"", root.path, root.path, root.plugin_name});}
  auto constructor = create_constructor();
  auto components = constructor->construct_components(requests);
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
rpp::ScriptRuntime<Threading::concurrent> BasicComponentRuntimeBuilder<Threading>::build_script_from_path(
  const std::string & script_path, std::optional<std::string> configuration)
{
  return build_script_from_description_path(
    data_manager_.get_default_script_description_path(script_path), std::move(configuration));
}

template<class Threading>
rpp::ScriptRuntime<Threading::concurrent> BasicComponentRuntimeBuilder<Threading>::build_script_from_library(
  const std::string & library_name, const std::string & script_name,
  std::optional<std::string> configuration)
{
  return build_script_from_description_path(
    data_manager_.get_script_description_path_from_library(library_name, script_name),
    std::move(configuration));
}

template<class Threading>
rpp::ScriptRuntime<Threading::concurrent> BasicComponentRuntimeBuilder<Threading>::build_script_from_description_path(
  const std::string & script_description_path, std::optional<std::string> configuration,
  std::optional<std::string> parts_folder)
{
  return ScriptRuntime(create_constructor()->build_script(
    script_description_path, std::move(configuration), std::move(parts_folder)));
}

template class BasicComponentRuntimeBuilder<SingleThreaded>;
template class BasicComponentRuntimeBuilder<Concurrent>;
}  // namespace rpp
