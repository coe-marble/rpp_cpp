#pragma once
#include <memory>
#include <vector>

#include "runtime_fwd.hpp"
#include <string>

#include "runtime.hpp"
#include "build_requests.hpp"
#include "data_manager.hpp"
#include "external_component_process.hpp"

namespace rpp
{

/**
 * Public construction facade. Components enter fixed root slots; scripts own slot collections.
 * Supplies construction options retained by each slot manager for later loads.
 * Building prepares transports but never calls configure or activate callbacks.
 * Build calls on the same builder must not overlap or reenter the builder.
 * See docs/context-flow.md for the complete construction and ownership sequence.
 */
template<class Threading>
class BasicComponentRuntimeBuilder
{
  using ComponentRuntime = rpp::ComponentRuntime<Threading::concurrent>;
  using ScriptRuntime = rpp::ScriptRuntime<Threading::concurrent>;
  using SlotManager = BasicSlotManager<Threading>;
  using ComponentRuntimeBuilder = BasicComponentRuntimeBuilder<Threading>;

public:
  /**
   * Initial admission validators indexed by slot plugin type, then candidate component name.
   * The type is the base plugin identity (without List/Option wrappers).
   * Initial names use the workspace component name; empty names are rejected on registration.
   * dynamic candidates use the name supplied to load_component().
   * Rules apply to matching candidates throughout the constructed tree.
   * Validators receive prepared contexts before those contexts enter their slot.
   */
  using RegistrationValidators = typename SlotManager::RegistrationValidators;

  /**
   * Copyable callback: bool(slot_name, slot_type).
   * ComponentSlot exposes plugin_type, kind (SINGLE/OPTION/LIST), and activation_policy().
   * Invoked once per slot when its context is assembled, including empty declared slots.
   * The result is fixed for that slot's lifetime. Nested slots use the same callback;
   * identical names in different contexts are evaluated independently.
   * This sets permissions on managers created locally; it is not sent to external hosts.
   * Keep captured references alive for later dynamic builds as well as initial construction.
   */
  using DynamicComponentPolicy = typename SlotManager::DynamicComponentPolicy;

private:
  std::shared_ptr<rpp::RppDataManager> data_manager_own_;
  rpp::RppDataManager & data_manager_;
  ClockOptions clock_options_;
  std::variant<bool, DynamicComponentPolicy> dynamic_components_;
  RegistrationValidators registration_validators_;

  ExternalComponentProcessRegistry external_component_process_registry_;

public:
  /**
   * Borrow a data manager and apply one dynamic-modification flag to every slot.
   * @param data_manager Must outlive this builder and its build calls.
   * @param clock_options Clock settings passed to every constructed context.
   * @param allow_dynamic_components Enable runtime load/unload, replacement, and selection changes.
   * False still permits initial construction, normal lifecycle, and root shutdown.
   * @param validators Initial type/name admission rules; copied/moved into the builder.
   * Does not read component descriptions, construct plugins, or invoke lifecycle callbacks.
   */
  BasicComponentRuntimeBuilder(
    rpp::RppDataManager & data_manager,
    const ClockOptions & clock_options = ClockOptions(), bool allow_dynamic_components = false,
    RegistrationValidators validators = {});

  /**
   * Borrow a data manager and decide dynamic permission separately for each slot.
   * @param data_manager Must outlive this builder and its build calls.
   * @param clock_options Clock settings passed to constructed contexts.
   * @param policy Copyable callable bool(const std::string &, const ComponentSlot &).
   * Receives the slot name and parsed type; true enables mutation for that slot.
   * The callback is copied into managers used for later dynamic loading, preserving
   * the policy for newly constructed descendants after this builder is destroyed.
   * @param validators Initial admission rules, independent of dynamic permission.
   * @throws std::invalid_argument If the supplied callable wrapper is empty.
   * Policy exceptions propagate from build calls, not from this constructor.
   * The callback only sets each local slot manager's permission, regardless of the
   * source language of the components in that slot. External hosts are started normally.
   */
  BasicComponentRuntimeBuilder(
    rpp::RppDataManager & data_manager, DynamicComponentPolicy policy,
    const ClockOptions & clock_options = ClockOptions(), RegistrationValidators validators = {});

  /**
   * Own a default RppDataManager using the configured RPP_HOME.
   * @param clock_options Clock settings passed to every constructed context.
   * @param allow_dynamic_components Uniform slot permission; false by default.
   * @param validators Initial admission rules keyed by slot plugin type and candidate name.
   * Otherwise has the same construction/lifecycle behavior as the borrowed-manager overload.
   */
  explicit BasicComponentRuntimeBuilder(
    const ClockOptions & clock_options = ClockOptions(),
    bool allow_dynamic_components = false, RegistrationValidators validators = {});

  /// Destroy the builder; returned runtimes and managers retain their construction resources.
  ~BasicComponentRuntimeBuilder();

  /**
   * Select the executable used to host an adapted source language in future builds.
   * @param language Source-language name, normalized case-insensitively.
   * @param command Executable name/path; registration does not launch it.
   * @throws std::invalid_argument If language or command is empty.
   * Replaces that language's previous registration. Existing contexts are unaffected.
   */
  void register_external_component_server(
    const std::string & language, const std::string & command);

  /// Build one prepared, unconfigured top-level component tree.
  ComponentRuntime build_component(const ComponentRequest & request);

  /// Batch construction preserves shared adapted-host preparation.
  std::vector<ComponentRuntime> build_components(
    const std::vector<ComponentRequest> & requests);

  /// Build a script from a path, registered library identity, or explicit description.
  ScriptRuntime build_script(const ScriptRequest & request);

  /// Build a complete component server. The returned host owns all slot managers,
  /// adapters, and deferred root loaders; callers never handle managers directly.
  std::unique_ptr<BasicRppServerHost<Threading>> build_component_server(
    const ComponentServerRequest & request);

private:
  // Slot managers retain construction settings; none captures this builder.
  std::shared_ptr<ComponentConstructor<Threading>> create_constructor() const;

};
}  // namespace rpp
