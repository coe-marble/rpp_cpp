#pragma once

#include <string>
#include <memory>
#include <map>
#include <vector>
#include "plugin_def.hpp"
#include "parameters.hpp"
#include <functional>
#include "logger.hpp"
#include "clock.hpp"
#include "component_manager.hpp"

namespace rpp
{

class ComponentContextBuilder;
class ExternalComponentProcess;

/**
 * Public access to one component, or a script root and its managed slots.
 * Builders construct instances; contexts expose parameters, components, and
 * lifecycle/slot operations. Child lifecycle calls route through their owner.
 * The builder assembles managers first; this context takes ownership of their map.
 */
class ComponentContext
{
  friend class ComponentContextBuilder;
  friend class ComponentContextAccess;
  friend class ComponentManager;

private:
  using PluginPtrShared = std::shared_ptr<Plugin>;
  using SubcomponentSlots = std::map<std::string, std::shared_ptr<ComponentManager>>;
  using ScriptComponentSlots = SubcomponentSlots;

  params::Parameters parameters_;
  SubcomponentSlots subcomponent_slots_;
  // Route calls on a borrowed child context through its slot without an ownership cycle.
  std::weak_ptr<ComponentManager> owner_;
  // Slot-local entry identity, assigned during adoption/loading, not a plugin class name.
  std::string owner_name_;
  std::string component_name_;
  std::string component_reference_;
  std::string plugin_type_;
  std::shared_ptr<ExternalComponentProcess> external_component_process_;
  PluginPtrShared instance_;
  std::shared_ptr<rpp::RppLogger> logger_;
  std::shared_ptr<rpp::RppClock> clock_;
  mutable bool activation_attempted_ = false;
  mutable bool is_active_ = false;
  mutable bool configuration_attempted_ = false;

  CallbackResult close_runtime();

  CallbackResult configure_runtime() const;

  CallbackResult cleanup_runtime() const;

  ComponentManager & slot(const std::string & name) const;

  std::shared_ptr<ComponentManager> owning_manager() const;

  bool contains_guarded_call() const;

  bool has_active_work() const;

  // Explicit teardown must stop all descendants, even if callbacks do not opt into their lifecycle.
  CallbackResult deactivate_components_for_shutdown(ComponentManager::Timeout timeout) const;

  // Take ownership of assembled managers; slot creation belongs to the builder.
  ComponentContext(
    PluginPtrShared instance,
    params::Parameters && parameters,
    std::shared_ptr<ExternalComponentProcess> external_component_process,
    SubcomponentSlots && subcomponent_slots,
    const ClockOptions & clock_options);

  // Script roots own their assigned components through managers, without their own plugin instance.
  ComponentContext(
    ScriptComponentSlots && script_components, const ClockOptions & clock_options);

public:
  /**
   * Admission rule receiving a candidate and existing contexts, including inactive entries.
   * Return a failure to reject the candidate. Validators must not perform lifecycle operations.
   */
  using RegistrationValidator = ComponentManager::Validator;
  /**
   * Maximum time to wait for guarded calls to finish; Timeout::max() waits indefinitely.
   * This does not impose a time limit on plugin callbacks or external process startup.
   */
  using Timeout = ComponentManager::Timeout;
  /**
   * Release owned resources. Call close() explicitly to run lifecycle shutdown and inspect failures.
   */
  virtual ~ComponentContext();
  /**
   * Transfer this context and its resources; the moved-from object may be destroyed or assigned.
   * Applications should not move contexts borrowed from a slot manager.
   */
  ComponentContext(ComponentContext && other) noexcept;
  /**
   * Replace this context by moving another. Call close() on the destination first
   * if it still owns a live tree, so lifecycle failures can be handled.
   */
  ComponentContext & operator=(ComponentContext && other) noexcept;

  /**
   * Contexts own runtime resources and cannot be copied.
   */
  ComponentContext(const ComponentContext &) = delete;
  /**
   * Contexts own runtime resources and cannot be copy-assigned.
   */
  ComponentContext & operator=(const ComponentContext &) = delete;


  /**
   * Return shared, const access to this context's logger. Logging remains available.
   */
  std::shared_ptr<const rpp::RppLogger> get_logger() const;

  /**
   * Return shared, const access to this context's clock for reading timestamps.
   */
  std::shared_ptr<const rpp::RppClock> get_clock() const;

  /**
   * Deactivate, clean up, and release a root tree. On a managed child, request unload
   * through its owning slot; static slots reject that unload. Check the returned result:
   * lifecycle failure retains resources needed for recovery. May block while guarded calls drain.
   */
  CallbackResult close();
  /**
   * Configure registered children before this plugin. A managed child delegates to its slot.
   * Adapted plugins configure through their runtime host. Returns the first failure;
   * previously configured children are not automatically rolled back.
   */
  CallbackResult configure() const;
  /**
   * Activate only this plugin; child components are unaffected by default.
   * @param state Optional handover state for this plugin, or no value for cold activation.
   * The plugin's on_activate() runs before its manager marks the entry ACTIVE.
   * The callback may call activate_components() to opt into child activation.
   * Adapted activation is currently unsupported and returns a failure.
   */
  CallbackResult activate(const std::any & state = {}) const;
  /**
   * Deactivate only this plugin and return its optional handover state on success.
   * A managed entry drains guarded calls, then runs on_deactivate() before becoming INACTIVE.
   * The callback may call deactivate_components() to opt into child deactivation.
   * Child failures affect this result only when the plugin propagates them from its callback.
   * Adapted deactivation is currently unsupported and returns a failure.
   */
  DeactivationResult<std::any> deactivate() const;
  /**
   * Explicitly activate the selected entry in each exclusive child slot, or all entries in a list.
   * Script applications may call this directly; plugins may call it from on_activate().
   * Each child controls its own descendants through its callbacks. No handover state is passed.
   * On failure, roll back only entries newly activated by this helper; preserve prior active entries.
   * Success does not tie child lifetime to the parent's later activation result.
   */
  CallbackResult activate_components() const;
  /**
   * Explicitly deactivate active child entries in reverse slot/registration order.
   * Scripts may call this directly; plugins may call it from on_deactivate().
   * Continue after errors and return the first failure. Child handover states are discarded.
   * Each child controls its own descendants; this does not force recursive deactivation.
   * @param timeout Maximum wait for guarded calls on each child; max() waits indefinitely.
   */
  CallbackResult deactivate_components(Timeout timeout = Timeout::max()) const;
  /**
   * Release configured resources on this plugin, then children in reverse order.
   * Deactivate active work first. Cleanup continues through child failures and reports
   * the first error; a managed child delegates lifecycle validation to its slot.
   */
  CallbackResult cleanup() const;

private:
  CallbackResult close_impl();


  CallbackResult configure_impl() const;

  CallbackResult cleanup_impl() const;

  // Transition validation and call gates belong to the manager. These callbacks
  // affect only this plugin; child lifecycle is an explicit choice inside the callbacks.
  CallbackResult activate_impl(const std::any & state) const;

  DeactivationResult<std::any> deactivate_impl() const;

public:
  /**
   * Read a resolved parameter by value using its declared C++ representation T.
   * @throws std::runtime_error If the parameter is missing or cannot be converted to T.
   */
  template<typename T>
  T get_parameter(const std::string & name) const;

  /**
   * Return the selected plugin instance in a slot, including when inactive.
   * T must be the correct plugin interface type; this accessor uses a static cast.
   * @throws std::runtime_error If the slot is missing, empty, or has no plugin instance.
   * Use with_active_component() when calls can overlap runtime transitions.
   */
  template<typename T>
  std::shared_ptr<T> get_component(const std::string & name) const;

  /**
   * Return all loaded plugin instances in registration order, including inactive candidates.
   * T must be the correct plugin interface type. Missing slots or instances throw.
   * The returned pointers do not guard calls against lifecycle transitions.
   */
  template<typename T>
  std::vector<std::shared_ptr<T>> get_components(const std::string & name) const;

  /**
   * Return slot names in sorted order, including declared empty slots.
   */
  std::vector<std::string> list_subcomponents() const;

  /**
   * Return this context's own plugin as T. T must be the correct interface type;
   * the static cast does not check type compatibility. A script context has no instance
   * and throws std::runtime_error. The pointer does not guard lifecycle transitions.
   */
  template<typename T>
  std::shared_ptr<T> get_instance() const;

  /**
   * Return a reference to the selected child context, including when inactive.
   * Missing or empty slots throw. The reference is borrowed; do not retain it across
   * unloading or replacement. Use get_subcomponent_contexts() for shared ownership.
   */
  const ComponentContext & get_subcomponent_context(const std::string & name) const;

  /**
   * Return a snapshot of shared const contexts for loaded entries in registration order.
   * Includes inactive candidates. Shared ownership keeps the context object alive,
   * but does not keep its plugin loaded or bypass generation checks. Missing slots throw.
   */
  std::vector<std::shared_ptr<const ComponentContext>> get_subcomponent_contexts(
    const std::string & name) const;

  /**
   * Return a borrowed child context by slot name and registration name.
   * Missing slots or unloaded entries throw LifecycleError. Do not retain the reference
   * across unloading or replacement; child lifecycle calls still use its owning manager.
   */
  const ComponentContext & get_subcomponent_context(
    const std::string & slot_name,
    const std::string & component_name) const;

  /**
   * Return whether this instance's owning slot permits replacement or selection changes.
   * An unmanaged root returns false. Child-slot permissions are independent and are not inspected.
   * A released owning slot throws LifecycleError, as with other borrowed-context operations.
   */
  bool allows_dynamic_components() const;
  /**
   * Return the fixed build-time permission for a named child slot. Missing slots throw LifecycleError.
   * A parent's permission does not grant permission to switch this child independently.
   */
  bool allows_dynamic_components(const std::string & slot_name) const;
  /**
   * Return entry metadata in registration order, including lifecycle state, generation,
   * and unloaded/error entries. Missing slots throw LifecycleError.
   */
  std::vector<ComponentInfo> list_components(const std::string & slot_name) const;
  /**
   * Build and register an unconfigured candidate in a dynamic slot.
   * @param slot_name Slot receiving the candidate.
   * @param name Nonempty registration name, also used to select its admission rule.
   * @param component Workspace component folder passed to the builder-supplied factory.
   * Does not configure or activate the plugin. Admission rejection preserves existing entries.
   */
  CallbackResult load_component(
    const std::string & slot_name, const std::string & name,
    const std::string & component) const;
  /**
   * Configure a named unconfigured entry and its children, making it inactive on success.
   * The slot manager validates the transition and reports callback failures.
   */
  CallbackResult configure_component(const std::string & slot_name, const std::string & name) const;
  /**
   * Activate a configured entry, optionally supplying its typed handover state.
   * Exclusive slots allow at most one active entry. Static exclusive slots reject
   * selection changes. Adapted activation currently returns a failure.
   */
  CallbackResult activate_component(
    const std::string & slot_name, const std::string & name,
    const std::any & state = {}) const;
  /**
   * Drain guarded calls and deactivate an entry, returning its optional handover state.
   * @param timeout Maximum wait for guarded calls; Timeout::max() waits indefinitely.
   * On timeout or lifecycle failure, retain the entry for recovery.
   */
  DeactivationResult<std::any> deactivate_component(
    const std::string & slot_name,
    const std::string & name, Timeout timeout = Timeout::max()) const;
  /**
   * Clean up an inactive/unconfigured/error entry after active work has stopped.
   * Successful cleanup returns it to UNCONFIGURED without unloading it.
   */
  CallbackResult cleanup_component(const std::string & slot_name, const std::string & name) const;
  /**
   * Drain guarded calls, deactivate and clean up as needed, then release a named entry.
   * Requires a dynamic slot. The registration record remains available for a later load.
   * @param timeout Maximum wait for guarded calls; timeout/failure retains the affected entry.
   */
  CallbackResult unload_component(
    const std::string & slot_name, const std::string & name,
    Timeout timeout = Timeout::max()) const;
  /**
   * Deactivate the active source and activate/select the target within one dynamic slot.
   * Load/configure the target first if its registration needs it.
   * @param transfer_state Pass the source snapshot to the target when true (the default).
   * State types must be compatible. Target activation failure leaves the source stopped.
   * @param timeout Maximum wait for source guarded calls to finish.
   */
  CallbackResult switch_component(
    const std::string & slot_name, const std::string & source,
    const std::string & target, bool transfer_state = true, Timeout timeout = Timeout::max()) const;
  /**
   * Unload, rebuild, and configure a named entry; reactivate it if previously active.
   * Requires a dynamic slot.
   * @param transfer_state Preserve handover state across replacement when true.
   * @param timeout Maximum wait for guarded calls to drain.
   * @param refresh Optional hook called with the component path after unload and before rebuild,
   * for example to update registration or source artifacts. Failures stop the sequence.
   */
  CallbackResult reload_component(
    const std::string & slot_name, const std::string & name,
    bool transfer_state = false, Timeout timeout = Timeout::max(),
    std::function<void(const std::string &)> refresh = {}) const;
  /**
   * Install a slot-wide admission rule for future registrations/replacements.
   * Existing entries are not evicted or revalidated. Candidate-specific builder rules
   * still apply. Return failure if called reentrantly from guarded or validator code.
   */
  CallbackResult set_registration_validator(
    const std::string & slot_name,
    RegistrationValidator validator);

  /**
   * Call callback(T&) while holding an active-call guard for a named entry.
   * T is checked dynamically. The callback may return void or an owned value;
   * pointer/reference return types are rejected. Do not retain the supplied reference
   * or perform lifecycle operations from the callback. Callback exceptions propagate.
   * @param generation Optional expected generation, obtained from list_components().
   * @throws LifecycleError If inactive, draining, stale, missing, or of the wrong type.
   */
  template<typename T, typename Callback>
  auto with_active_component(
    const std::string & slot_name, const std::string & name,
    Callback && callback, std::optional<uint64_t> generation = {}) const
  -> std::invoke_result_t<Callback, T &>;

};

} /// namespace rpp

#include "context.tpp"
