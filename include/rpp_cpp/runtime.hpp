#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <type_traits>
#include <utility>

#include "threading.hpp"

#include "runtime_fwd.hpp"
#include "handover_state.hpp"
#include "slot_manager.hpp"
#include "context.hpp"

namespace rpp
{
namespace detail
{
template<class Runtime, bool HasRootInstance>
class RootInstanceAccess
{};

template<class Runtime>
class RootInstanceAccess<Runtime, true>
{
public:
  template<typename T>
  std::shared_ptr<T> get_instance() const
  {
    auto instance = std::static_pointer_cast<T>(
      static_cast<const Runtime *>(this)->component_data().instance_);
    if (!instance) {throw std::runtime_error("This runtime has no plugin instance");}
    return instance;
  }
};
}  // namespace detail

/**
 * Application-facing handle for lifecycle and component administration.
 * Slot entries own component data; scripts directly own their services and slot collection.
 * All plugin lifecycle operations execute through the corresponding slot manager.
 * Services and subcomponent access are exposed through get_context().
 */
template<bool Concurrent, bool IsScript>
class BasicRuntime : public detail::RootInstanceAccess<BasicRuntime<Concurrent, IsScript>, !IsScript>
{
  using Runtime = BasicRuntime<Concurrent, IsScript>;
  using Threading = std::conditional_t<Concurrent, rpp::Concurrent, SingleThreaded>;
  using SlotManager = BasicSlotManager<Threading>;
  using ComponentData = detail::ComponentData<Threading>;

  template<class, bool> friend class detail::RootInstanceAccess;
  friend class ComponentRuntimeAccess;
  friend class BasicSlotManager<Threading>;
  friend class BasicComponentRuntimeBuilder<Threading>;

  struct ComponentHandle
  {
    std::shared_ptr<SlotManager> slot;
    std::string name;
    std::string instance_id;
    bool owns_tree = false;
  };
  using Storage = std::conditional_t<IsScript, std::unique_ptr<ComponentData>, ComponentHandle>;
  Storage storage_;

  ComponentData & component_data() const;
  SlotManager & slot(const std::string & name) const;
  CallbackResult close_owned_slots(typename SlotManager::Timeout timeout);
  void release_owned_tree() noexcept;
  explicit BasicRuntime(Storage storage);

public:
  /// Inspect a candidate and registered peers before admission. Validators must not manage lifecycle.
  using RegistrationValidator = typename SlotManager::Validator;

  /// Maximum guarded-call drain time; max() waits indefinitely. Does not time plugin callbacks.
  using Timeout = typename SlotManager::Timeout;

  /// Attempt shutdown for a root handle. Call close() explicitly to inspect failures.
  ~BasicRuntime();

  /// Transfer this handle. Manager-bound plugin contexts require no rebinding.
  /// Serialize moves with access; moved-from handles may be destroyed or assigned.
  BasicRuntime(Runtime && other) noexcept;

  /// Replace this handle. Close a live destination first to inspect shutdown failures.
  Runtime & operator=(Runtime && other) noexcept;

  BasicRuntime(const Runtime &) = delete;
  Runtime & operator=(const Runtime &) = delete;

  /// Borrow services and subcomponent access from the manager-owned context.
  /// The reference survives handle moves; keep its owning component view alive.
  /// Release detaches the view; its operations then throw LifecycleError.
  const ComponentContext & get_context() const;

  /// Configure subcomponents before this plugin; failure leaves recovery to the caller.
  CallbackResult configure() const;

  /// Activate this plugin with optional handover state; subcomponents are explicit opt-in.
  CallbackResult activate(HandoverState state = {}) const;

  /// Drain guarded calls and deactivate this plugin, returning optional handover state.
  DeactivationResult<HandoverState> deactivate(Timeout timeout = Timeout::max()) const;

  /// Clean up this plugin, then subcomponents in reverse order, after active work stops.
  CallbackResult cleanup() const;

  /// Shut down a root tree. On a subcomponent handle, request unload from its slot.
  /// Failures retain resources needed for recovery; static slots reject direct unload.
  CallbackResult close();

  /// Process deferred self-deactivation on the application's lifecycle thread at a safe point.
  /// Failed deactivations stay pending for retry. Call after application callbacks return.
  CallbackResult process_deactivations(Timeout timeout = Timeout::max()) const;

  /// Observe owning-slot permission; fixed roots and script collections return false.
  bool allows_dynamic_components() const;

  /// Observe the fixed permission for a subcomponent slot; missing slots throw.
  bool allows_dynamic_components(const std::string & slot_name) const;

  /// Snapshot registrations in a slot, including unloaded/error entries and instance UUIDs.
  std::vector<ComponentStatus> get_subcomponent_statuses(const std::string & slot_name) const;

  /// Construct and register an unconfigured candidate in a dynamic slot.
  /// component is the workspace component folder; name is the slot-local registration name.
  CallbackResult load_component(
    const std::string & slot_name, const std::string & name,
    const std::string & component) const;

  /// Configure a named entry and its subcomponents; the slot manager validates the transition.
  CallbackResult configure_component(const std::string & slot_name, const std::string & name) const;

  /// Activate a named entry; exclusive slots reject a second active entry.
  CallbackResult activate_component(
    const std::string & slot_name, const std::string & name,
    HandoverState state = {}) const;

  /// Drain guarded calls and deactivate a named entry, returning optional handover state.
  DeactivationResult<HandoverState> deactivate_component(
    const std::string & slot_name, const std::string & name,
    Timeout timeout = Timeout::max()) const;

  /// Clean up a named entry after active work stops; success leaves UNCONFIGURED.
  CallbackResult cleanup_component(const std::string & slot_name, const std::string & name) const;

  /// Shut down and release a named entry in a dynamic slot. Failures retain it for retry.
  CallbackResult unload_component(
    const std::string & slot_name, const std::string & name,
    Timeout timeout = Timeout::max()) const;

  /// Stop source, then activate target, optionally transferring compatible handover state.
  /// Target failure leaves source stopped; dynamic permission is required.
  CallbackResult switch_component(
    const std::string & slot_name, const std::string & source,
    const std::string & target, bool transfer_state = true, Timeout timeout = Timeout::max()) const;

  /// Unload, rebuild, configure and optionally reactivate a dynamic entry.
  /// refresh runs after unload and before construction. Failures stop the sequence.
  CallbackResult reload_component(
    const std::string & slot_name, const std::string & name,
    bool transfer_state = false, Timeout timeout = Timeout::max(),
    std::function<void(const std::string &)> refresh = {}) const;

  /// Replace a slot's admission validator for future registrations; do not reenter lifecycle.
  CallbackResult set_registration_validator(
    const std::string & slot_name, RegistrationValidator validator);
};

}  // namespace rpp
