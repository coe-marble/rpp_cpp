#pragma once
#include <memory>
#include <vector>

#include "runtime_fwd.hpp"
#include <optional>
#include <string>
#include <variant>

#include "runtime.hpp"
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

  /// One workspace root requested by build_from_component_roots().
  struct ComponentRoot
  {
    /// Component folder, containing its description or a linked-component record.
    std::string path;
    /// Registered library::Plugin identity; required for a linked root, otherwise optional.
    std::string plugin_name;
  };

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

  /**
   * Build one prepared, unconfigured component tree.
   * @param component_path Folder containing the component description.
   * @return An owning root runtime; no configure/activate callback has run.
   * Resolves records, creates plugin instances, resolves parameters, recursively builds
   * children, and prepares adapted transports when needed.
   * For a linked root requiring an explicit plugin identity, use build_from_component_roots().
   * @throws std::exception On invalid/missing data, loading, admission, policy, or transport failure.
   * Policy/validator failures abort construction; preparation references are released.
   */
  ComponentRuntime build_component_from_path(const std::string & component_path);

  /**
   * Build several roots after preparing their adapted transports as a batch.
   * @param roots Component folders and optional/linked-root plugin identities.
   * @return Runtimes in the same order as roots; empty input produces an empty vector.
   * Each context owns its tree/handles; roots may share a prepared external host.
   * Does not configure or activate plugins. Failure releases this batch's preparation state.
   * @throws std::exception For the same construction failures as build_component_from_path().
   */
  std::vector<ComponentRuntime> build_from_component_roots(
    const std::vector<ComponentRoot> & roots);

  /**
   * Build a script context from the workspace description associated with its source path.
   * @param script_path Script source path, used to find .rppws/script_descriptions/<stem>.json.
   * @param configuration Configuration name; omitted means the description's active configuration.
   * @return A script context with managed slots and no plugin instance of its own.
   * Does not execute the script or invoke configure/activate callbacks.
   * @throws std::exception If the description/configuration cannot be resolved or construction fails.
   */
  ScriptRuntime build_script_from_path(
    const std::string & script_path,
    std::optional<std::string> configuration = std::nullopt);

  /**
   * Find a script in the data manager's selected workspace and build its assignments.
   * @param library_name Library owning the registered script identity.
   * @param script_name Unqualified name or matching library::name.
   * @param configuration Configuration name; omitted selects the active configuration.
   * @return An owning, unconfigured script runtime; the script is not executed.
   * @throws std::exception On missing/ambiguous script identity, configuration, or build failure.
   */
  ScriptRuntime build_script_from_library(
    const std::string & library_name,
    const std::string & script_name,
    std::optional<std::string> configuration = std::nullopt);

  /**
   * Build assignments from an explicit script-description JSON file.
   * @param script_description_path Description containing Spec, Configurations, and ActiveConfiguration.
   * @param configuration Configuration name, or the description's active configuration.
   * @param parts_folder Override the component parts directory; omitted uses the workspace parts folder.
   * @return A script root whose slot managers own the built component contexts.
   * Empty declared slots are retained. Configure/activate remain explicit application operations.
   * @throws std::exception On description/configuration errors or any component construction failure.
   */
  ScriptRuntime build_script_from_description_path(
    const std::string & script_description_path,
    std::optional<std::string> configuration = std::nullopt,
    std::optional<std::string> parts_folder = std::nullopt);

private:
  // Slot managers retain construction settings; none captures this builder.
  std::shared_ptr<ComponentConstructor<Threading>> create_constructor() const;

};
}  // namespace rpp
