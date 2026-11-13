#pragma once

#include "runtime_fwd.hpp"
#include "threading.hpp"

#include <chrono>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "plugin_def.hpp"
#include "handover_state.hpp"
#include "component_constructor.hpp"
#include "component_slots.hpp"

namespace rpp
{

/// A temporary, lifecycle-guarded handle to an active component.
///
/// The handle is move-only and keeps the slot call guard for its lifetime.
/// Do not retain references or pointers obtained through it beyond the handle.
template<typename T>
class ActiveComponent
{
  template<class>
  friend class BasicSlotManager;

  // Member order is deliberate: destroy the instance reference before releasing
  // the slot call guard.
  std::shared_ptr<void> guard_;
  std::shared_ptr<T> instance_;

  ActiveComponent(std::shared_ptr<void> guard, std::shared_ptr<T> instance)
  : guard_(std::move(guard)), instance_(std::move(instance)) {}

public:
  ActiveComponent() = default;
  ActiveComponent(const ActiveComponent &) = delete;
  ActiveComponent & operator=(const ActiveComponent &) = delete;
  ActiveComponent(ActiveComponent &&) noexcept = default;
  ActiveComponent & operator=(ActiveComponent &&) noexcept = default;

  [[nodiscard]] explicit operator bool() const noexcept
  {
    return static_cast<bool>(instance_);
  }

  T & operator*() const {return *instance_;}
  T * operator->() const {return instance_.get();}
};

/**
 * Owns the component entries in one single, option or list slot.
 * Enforces registration, exclusive/multiple activation, lifecycle state,
 * instance identity, and guarded-call draining. Lifecycle/membership operations
 * execute on one owner thread; concurrent mode synchronizes only access to the slot gate.
 * Applications normally use the owning ComponentRuntime; this class creates no executor.
 */
template<class Threading>
class BasicSlotManager : public std::enable_shared_from_this<BasicSlotManager<Threading>>
{
  using ComponentRuntime = rpp::ComponentRuntime<Threading::concurrent>;
  using SlotManager = BasicSlotManager<Threading>;
  using Constructor = ComponentConstructor<Threading>;
  using ComponentData = detail::ComponentData<Threading>;

  friend class ComponentRuntimeAccess;
  friend class BasicComponentRuntimeBuilder<Threading>;
  template<bool, bool>
  friend class BasicRuntime;
  friend class ComponentConstructor<Threading>;
  friend class ComponentContext;
  friend class SubcomponentContext;

public:
  /// In-process construction seam. Production managers retain a ComponentConstructor instead.
  using RuntimeFactory = typename ComponentConstructor<Threading>::RuntimeFactory;

  /// Maximum duration of guarded-call draining; max() waits indefinitely.
  using Timeout = std::chrono::milliseconds;

  /// Inspect a candidate and loaded peers before admission; return failure to reject it.
  /// Validators run on the owner thread and must not perform lifecycle/membership operations.
  using Validator = typename ComponentConstructor<Threading>::Validator;

  /// Admission validators selected by the candidate's slot-local registration name.
  using NamedValidators = typename ComponentConstructor<Threading>::NamedValidators;
  using RegistrationValidators = typename ComponentConstructor<Threading>::RegistrationValidators;
  using DynamicComponentPolicy = typename ComponentConstructor<Threading>::DynamicComponentPolicy;

private:
  struct Entry;
  ComponentData & component_data(const std::string & name, std::string_view instance_id) const;
  static ComponentRuntime create_root(
    std::unique_ptr<ComponentData> data,
    std::shared_ptr<SlotManager> manager = {});
  std::unique_ptr<ComponentData> take_component(const ComponentRuntime & runtime);
  std::shared_ptr<ComponentRuntime> runtime_view(const Entry & item) const;
  std::shared_ptr<Plugin> subcomponent_instance(
    const std::optional<std::string> & component_name) const;
  std::vector<std::shared_ptr<Plugin>> subcomponent_instances() const;
  using PreparedComponents = typename Constructor::PreparedComponents;
  std::shared_ptr<Constructor> constructor_;
  static CallbackResult release_resources(ComponentData & component);
  static CallbackResult configure_component(ComponentData & component);
  static CallbackResult configure_subcomponents(ComponentData & component);
  static CallbackResult cleanup_component(ComponentData & component);
  static CallbackResult cleanup_subcomponents(ComponentData & component);
  static CallbackResult activate_component(ComponentData & component, HandoverState state);
  static DeactivationResult<HandoverState> deactivate_component(ComponentData & component);
  static CallbackResult activate_subcomponents(ComponentData & component);
  static CallbackResult deactivate_subcomponents(ComponentData & component, Timeout timeout);
  static CallbackResult deactivate_descendants_for_shutdown(
    ComponentData & component,
    Timeout timeout);
  static bool requires_deactivation(ComponentData & component);
  CallbackResult process_deactivations(
    const std::string & name, std::string_view instance_id,
    Timeout timeout);
  static CallbackResult process_subcomponent_deactivations(
    ComponentData & component,
    Timeout timeout);
  static CallbackResult configure_external_component(ComponentData & component);
  static CallbackResult cleanup_external_component(ComponentData & component);
  static CallbackResult activate_external_component(ComponentData & component, HandoverState state);
  static DeactivationResult<HandoverState> deactivate_external_component(ComponentData & component);
  static CallbackResult release_external_component_process(ComponentData & component);
  static CallbackResult request_deactivation(ComponentData & component);

  RuntimeFactory factory_;
  // One short-held gate for registry snapshots, call admission, and draining.
  mutable typename Threading::Mutex slot_mutex_;
  typename Threading::Condition guarded_calls_drained_;
  typename Threading::LocalCalls guarded_call_nesting_;
  std::map<std::string, std::shared_ptr<Entry>> component_entries_;
  std::vector<std::string> registration_order_;
  bool shutdown_requested_ = false;
  ActivationPolicy activation_policy_;
  bool allow_dynamic_components_;
  bool registration_validation_in_progress_ = false;
  std::string plugin_type_;
  std::string component_name_;
  Validator registration_validator_;
  NamedValidators registration_validators_by_name_;
  CallbackResult validate(const ComponentRuntime &, const std::string & name);

  // Single-threaded reentrancy check only; concurrent mode identifies no threads.
  bool has_local_call() const {return guarded_call_nesting_.active();}

  // Caller holds the gate when registry access can overlap guarded callers.
  std::shared_ptr<Entry> entry_unlocked(const std::string & name) const;
  std::shared_ptr<Entry> selected_entry_unlocked() const;
  std::shared_ptr<Entry> entry(const std::string & name) const;
  std::optional<CallbackResult> begin(
    Entry &, const std::string & operation,
    std::initializer_list<LifecycleState> allowed, bool during_close = false);
  CallbackResult finish(Entry &, LifecycleState, CallbackResult);
  template<typename Callback>
  static CallbackResult capture(const std::string & operation, Callback callback);
  CallbackResult drain(Entry &, Timeout);
  DeactivationResult<HandoverState> stop(Entry &);
  CallbackResult initialize(
    const detail::ComponentBuildRequest & request, const PreparedComponents & prepared,
    std::unique_ptr<ComponentData> prepared_component = {});
  CallbackResult load_impl(
    const std::string &, const std::string &, bool initial,
    const detail::ComponentBuildRequest * request = nullptr,
    const PreparedComponents * prepared = nullptr,
    std::unique_ptr<ComponentData> prepared_component = {});

  void release_call(Entry &);

  struct ActiveCall
  {
    std::shared_ptr<Plugin> instance;
    std::shared_ptr<Entry> entry;
    BasicSlotManager * owner;
    ActiveCall(
      std::shared_ptr<Plugin> plugin, std::shared_ptr<Entry> item,
      BasicSlotManager & manager)
    : instance(std::move(plugin)), entry(std::move(item)), owner(&manager) {}

    ActiveCall(const ActiveCall &) = delete;

    ActiveCall(ActiveCall &&) = default;

    ~ActiveCall()
    {
      instance.reset();
      if (entry) {owner->release_call(*entry);}
    }
  };
  ActiveCall borrow(const std::string &, std::optional<std::string_view> expected_instance_id);
  std::optional<ActiveCall> try_borrow(
    const std::optional<std::string> & component_name,
    std::optional<std::string_view> expected_instance_id);

public:
  /// Create an empty slot with its factory, activation policy, dynamic permission and validators.
  /// Construction does not load, configure, or activate plugins.
  explicit BasicSlotManager(
    RuntimeFactory factory = {},
    ActivationPolicy policy = ActivationPolicy::MULTIPLE, bool allow_dynamic_components = false,
    std::string plugin_type = "", Validator validator = {}, NamedValidators named_validators = {});

  /// Attempt immediate shutdown; use close() explicitly to inspect failures and retry.
  /// The manager must outlive all guarded callbacks and have shared ownership before loading.
  ~BasicSlotManager();

  /// Slot ownership and admission state cannot be copied.
  BasicSlotManager(const SlotManager &) = delete;

  /// Slot ownership and admission state cannot be copy-assigned.
  SlotManager & operator=(const SlotManager &) = delete;

  /// Return the fixed permission for runtime replacement/selection in this slot.
  bool allows_dynamic_components() const;

  /**
   * Activate the selected entry in an exclusive slot, or all loaded entries in a list slot.
   * The owning lifecycle thread performs selection, state checks, and activation.
   * Append only newly activated contexts to activated for the caller's rollback;
   * the shared pointers keep those context objects alive. Already-active entries
   * are preserved. Stop and return on the first activation failure.
   */
  CallbackResult activate_selected(std::vector<std::shared_ptr<ComponentRuntime>> & activated);

  /// Replace the slot-wide admission rule on the owner thread.
  /// Existing entries are not revalidated; replacement during validation fails.
  CallbackResult set_registration_validator(Validator validator);

  /// Snapshot loaded runtimes in registration order, including inactive entries.
  /// Each returned handle retains this manager; released/replaced instances reject access.
  std::vector<std::shared_ptr<ComponentRuntime>> runtimes() const;

  /// Return a named loaded runtime, or null when unloaded; unknown names throw.
  std::shared_ptr<ComponentRuntime> runtime(const std::string & name) const;

  /// Return the selected loaded runtime, falling back to the first loaded entry, or null.
  std::shared_ptr<ComponentRuntime> selected_runtime() const;

  /// Share a named or selected parent-facing subcomponent view; list slots require a name.
  /// A retained view stays bound to the old instance and rejects access after release.
  std::shared_ptr<const SubcomponentContext> subcomponent_context(
    const std::optional<std::string> & component_name) const;

  /// Snapshot loaded parent-facing views in registration order, including inactive subcomponents.
  /// Views keep only their detached-access check alive, not the subcomponent runtimes/plugins.
  std::vector<std::shared_ptr<const SubcomponentContext>> subcomponent_contexts() const;

  /// Snapshot the selected entry for an exclusive slot, or all loaded entries for a list.
  std::vector<std::shared_ptr<ComponentRuntime>> activation_runtimes() const;

  /// Snapshot loaded entries that may retain active work, in registration order.
  std::vector<std::shared_ptr<ComponentRuntime>> deactivation_runtimes() const;

  /// Construct and register an unconfigured runtime in a dynamic slot on the owner thread.
  /// An empty path reuses the entry's previous path; admission rejection preserves peers.
  CallbackResult load(const std::string & name, const std::string & component = "");

  /// Dynamic load preserving the explicit plugin identity needed for linked component records.
  CallbackResult load(
    const std::string & name, const std::string & component,
    const std::string & plugin_name);

  /// Synchronously configure an unconfigured entry and its subcomponents.
  /// Optional instance UUID rejects stale handles; failure leaves ERROR.
  CallbackResult configure(
    const std::string & name,
    std::optional<std::string_view> expected_instance_id = {});

  /// Synchronously activate a configured entry, optionally using handover state.
  /// Exclusive slots reject a second active entry; expected UUID rejects stale handles.
  CallbackResult activate(
    const std::string & name, HandoverState state = {},
    std::optional<std::string_view> expected_instance_id = {});

  /// Close guarded admission, drain participating calls, and synchronously deactivate an entry.
  /// Return optional handover state; timeout/failure retains the runtime for recovery.
  /// Direct plugin calls remain governed by the plugin, not by this gate.
  DeactivationResult<HandoverState> deactivate(
    const std::string & name,
    Timeout timeout = Timeout::max(), std::optional<std::string_view> expected_instance_id = {});

  /// Synchronously clean up an entry after active work stops; success leaves UNCONFIGURED.
  /// Optional instance UUID rejects stale handles.
  CallbackResult cleanup(
    const std::string & name,
    std::optional<std::string_view> expected_instance_id = {});

  /// Drain guards, stop remaining work, clean up and release an entry in a dynamic slot.
  /// Failure retains resources for retry; the registration record remains available for loading.
  CallbackResult unload(
    const std::string & name, Timeout timeout = Timeout::max(),
    std::optional<std::string_view> expected_instance_id = {});

  /// Synchronously deactivate the source, then activate/select the target in a dynamic slot.
  /// Optionally transfer compatible handover state; source failure prevents target activation.
  CallbackResult switch_components(
    const std::string & source, const std::string & target,
    bool transfer_state = true, Timeout timeout = Timeout::max());

  /// Synchronously unload, rebuild, configure, and optionally reactivate a dynamic entry.
  /// Preserve handover state when requested; refresh runs after unload and before rebuilding.
  CallbackResult reload(
    const std::string & name, bool transfer_state = false,
    Timeout timeout = Timeout::max(), std::function<void(const std::string &)> refresh = {});

  /// Synchronously shut down all entries in reverse registration order, including static slots.
  /// Continue after independent failures and return the first; timeout bounds guard draining.
  CallbackResult close(Timeout timeout = Timeout::max());

  /// Snapshot an entry's state, transition, identity and guarded-call count; unknown names throw.
  ComponentStatus get_status(const std::string & name) const;

  /// Observe a named entry's current lifecycle state; unknown names throw.
  LifecycleState get_state(const std::string & name) const;

  /// Snapshot metadata for all registrations in order, including unloaded and error entries.
  std::vector<ComponentStatus> list() const;

  /// Acquire a move-only guard for a named list entry or the selected exclusive entry.
  /// An inactive or unavailable entry returns an empty handle. Type mismatches throw.
  template<typename T>
  [[nodiscard]] ActiveComponent<T> get_active(
    std::optional<std::string> component_name = {},
    std::optional<std::string_view> expected_instance_id = {})
  {
    auto call = try_borrow(component_name, expected_instance_id);
    if (!call) {return {};}
    auto instance = std::dynamic_pointer_cast<T>(call->instance);
    if (!instance) {throw LifecycleError("Managed plugin type mismatch");}
    auto guard = std::make_shared<ActiveCall>(std::move(*call));
    return ActiveComponent<T>(std::move(guard), std::move(instance));
  }

  /**
   * Guard only the duration of callback(T&) against deactivation/unloading of this entry.
   * Used by ComponentContext::with_active_component(); ordinary plugin calls are not tracked.
   * Concurrent mode counts calls without tracking thread IDs; single-threaded mode checks nesting.
   * All lifecycle/membership operations stay on the owning thread, outside affected guards.
   * The manager must outlive this call. Direct pointers bypass the gate in every lifecycle state.
   * Return and exception paths both release the guard.
   * It does not lock the plugin for the callback's duration: concurrent calls remain possible.
   */
  template<typename T, typename Callback>
  auto with_active(
    const std::string & name, Callback && callback,
    std::optional<std::string_view> expected_instance_id = {}) -> std::invoke_result_t<Callback,
    T &>
  {
    using Result = std::invoke_result_t<Callback, T &>;
    static_assert(
      !std::is_pointer_v<Result>&& !std::is_reference_v<Result>,
      "Return owned values, not borrowed plugin pointers/references");
    auto guard = borrow(name, expected_instance_id);
    auto * plugin = dynamic_cast<T *>(guard.instance.get());
    if (!plugin) {throw LifecycleError("Managed plugin type mismatch");}
    return std::invoke(std::forward<Callback>(callback), *plugin);
  }
};
}  // namespace rpp
