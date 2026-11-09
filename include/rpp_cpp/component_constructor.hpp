#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "runtime_fwd.hpp"
#include "plugin_def.hpp"
#include "clock.hpp"
#include "data_model.hpp"
#include "component_slots.hpp"
#include "component_data.hpp"

namespace rpp
{
class RppDataManager;
class ExternalComponentProcess;
class ExternalComponentProcessRegistry;

// Internal construction helper retained by managers. It resolves descriptions,
// prepares external hosts, and assembles instances, parameters and subcomponent-slot managers.
// Registration, lifecycle transitions and guarded calls remain manager responsibilities.
template<class Threading>
class ComponentConstructor
{
  using ComponentRuntime = rpp::ComponentRuntime<Threading::concurrent>;
  using SlotManager = BasicSlotManager<Threading>;
  using ComponentData = detail::ComponentData<Threading>;

public:
  using RuntimeFactory = std::function<ComponentRuntime(const std::string &)>;
  using Validator = std::function<CallbackResult(
      const ComponentRuntime &, const std::vector<std::shared_ptr<const ComponentRuntime>> &)>;
  using NamedValidators = std::map<std::string, Validator>;
  using RegistrationValidators = std::map<std::string, NamedValidators>;
  using DynamicComponentPolicy = std::function<bool(const std::string &, const ComponentSlot &)>;

  // Owned construction configuration survives the public builder facade.
  struct BuildOptions
  {
    std::shared_ptr<RppDataManager> data;
    ClockOptions clock;
    DynamicComponentPolicy dynamic_policy;
    RegistrationValidators validators;
    std::shared_ptr<ExternalComponentProcessRegistry> external_servers;
  };

  struct ComponentBuildRequest
  {
    std::string name;
    // Keep the referring path and resolution context across reloads of linked components.
    std::string path;
    std::string parent_path;
    std::string plugin_name;
  };
  using SlotBuildRequests = std::map<std::string, std::vector<ComponentBuildRequest>>;
  struct PreparedAdaptedComponent
  {
    std::shared_ptr<Plugin> instance;
    std::shared_ptr<ExternalComponentProcess> process;
  };
  using PreparedComponents = std::map<std::string, PreparedAdaptedComponent>;
  using SlotManagers = std::map<std::string, std::shared_ptr<SlotManager>>;

  explicit ComponentConstructor(std::shared_ptr<BuildOptions> options)
  : build_options_(std::move(options)) {}

  static SlotManagers create_slots(
    const SlotBuildRequests & slot_build_requests, const std::map<std::string, std::string> & spec,
    const std::shared_ptr<BuildOptions> & options, const PreparedComponents & prepared,
    RuntimeFactory factory = {}, const std::map<std::string, Validator> & validators = {});
  std::vector<std::unique_ptr<ComponentData>> construct_components(
    const std::vector<ComponentBuildRequest> & requests,
    const PreparedComponents * prepared = nullptr) const;
  std::unique_ptr<ComponentData> build_script(
    const std::string & description_path, std::optional<std::string> configuration,
    std::optional<std::string> parts_folder = std::nullopt) const;

private:
  std::shared_ptr<BuildOptions> build_options_;
  std::unique_ptr<ComponentData> construct_component(
    const ComponentRecord & record, const PluginInfo & plugin_info,
    const PreparedComponents & prepared) const;
  ComponentRecord get_component_record(const ComponentBuildRequest & request) const;
  void discover_external_components(
    const ComponentBuildRequest & request,
    std::vector<ComponentBuildRequest> & external_components) const;
  PreparedComponents prepare_processes_for_non_native_components(
    const std::vector<ComponentBuildRequest> & requests) const;
};
}  // namespace rpp
