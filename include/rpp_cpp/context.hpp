#pragma once
#include <memory>
#include <vector>
#include <stdexcept>

#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <type_traits>
#include <variant>

#include "slot_manager.hpp"
#include "handover_state.hpp"

namespace rpp
{

class SubcomponentContextRange;

/**
 * Plugin-facing services and subcomponent control. Refers non-owningly to component data.
 * Each subcomponent operation delegates to that subcomponent's slot manager.
 * Retained views reject access after release. Release must not overlap context access.
 * Lifecycle control stays on one owning thread. Concurrent policy permits guarded
 * worker calls, slot snapshots, and deferred self-deactivation. Direct plugin pointers
 * remain callable in inactive states; the plugin owns their state and synchronization.
 */
class ComponentContext
{
  template<bool, bool>
  friend class BasicRuntime;
  template<class>
  friend class BasicSlotManager;
  friend class SubcomponentContext;

  using DataRef = std::variant<
    std::reference_wrapper<detail::ComponentData<SingleThreaded>>,
    std::reference_wrapper<detail::ComponentData<Concurrent>>>;
  std::optional<DataRef> component_;

  template<class Threading>
  explicit ComponentContext(detail::ComponentData<Threading> & component)
  : component_(std::ref(component)) {}

  void detach() {component_.reset();}

  template<class Callback>
  decltype(auto) visit(Callback && callback) const
  {
    if (!component_) {throw LifecycleError("The component has been released");}
    return std::visit([&](auto component) -> decltype(auto) {
               if (component.get().resources_released_) {
                 throw LifecycleError("The component has been released");
               }
               return callback(component.get());
    }, *component_);
  }

public:
  /// Maximum wait for guarded calls; max() waits indefinitely, without timing plugin callbacks.
  using Timeout = SlotManager::Timeout;

  /// Self views belong to their component data and cannot be copied.
  ComponentContext(const ComponentContext &) = delete;

  /// Self views cannot be reassigned.
  ComponentContext & operator=(const ComponentContext &) = delete;

  /// Borrow the loaded instance UUID; copy it before retaining it beyond runtime release.
  const std::string & get_instance_id() const;

  /// Read a resolved parameter by value; missing or incompatible values throw.
  template<typename T>
  T get_parameter(const std::string & name) const;

  /// Share read-only access to the component logger.
  std::shared_ptr<const RppLogger> get_logger() const;

  /// Share read-only access to the component clock.
  std::shared_ptr<const RppClock> get_clock() const;

  /// List subcomponent slot names in sorted order, including declared empty slots.
  std::vector<std::string> list_subcomponents() const;

  /// Return whether the selected subcomponent is a direct native C++ instance.
  /// List slots always require component_name; unloaded entries throw.
  bool is_native_subcomponent(
    const std::string & slot_name,
    std::optional<std::string> component_name = std::nullopt) const;

  /// Borrow lifecycle control of a named or selected subcomponent in a slot.
  /// Adapted children forward lifecycle transitions to their remote host.
  /// List slots always require component_name.
  /// Keep the reference within its view lifetime; retain a shared snapshot when needed.
  const SubcomponentContext & get_subcomponent_context(
    const std::string & slot_name,
    std::optional<std::string> component_name = std::nullopt) const;

  /// Lazily enumerate loaded subcomponent views in registration order, including inactive entries.
  /// Iteration snapshots views but does not keep runtimes/plugins alive.
  SubcomponentContextRange enumerate_subcomponent_contexts(
    const std::string & slot_name) const;

  /// Observe this component's current active state; this does not reserve future access.
  bool is_active() const;

  /// Record self-deactivation without running callbacks. The application processes
  /// it at a safe point with runtime.process_deactivations(), after this call returns.
  /// Success means recorded; repeated submissions coalesce and failures stay pending for retry.
  /// Worker submission is supported only in concurrent mode while the view remains attached.
  CallbackResult request_deactivation() const;

  /// Synchronously activate selected exclusive subcomponents or all loaded list subcomponents.
  /// On failure, roll back only subcomponents newly activated by this call; use the owner thread.
  CallbackResult activate_subcomponents() const;

  /// Synchronously deactivate active subcomponents in reverse order and return the first failure.
  /// Concurrent mode drains their guarded calls; timeout bounds only that wait.
  CallbackResult deactivate_subcomponents(Timeout timeout = Timeout::max()) const;

  /// Acquire a move-only guard for an active child component.
  ///
  /// Omitting component_name selects the current entry of an exclusive slot.
  /// List slots require component_name. An inactive or unavailable component
  /// returns an empty handle; type mismatches and unknown slots throw.
  /// Destroy the returned handle before deactivating, switching, unloading, or
  /// cleaning up its slot; those lifecycle operations cannot proceed while it
  /// is in use.
  template<typename T>
  [[nodiscard]] ActiveComponent<T> get_active_component(
    const std::string & slot_name,
    std::optional<std::string> component_name = {},
    std::optional<std::string_view> expected_instance_id = {}) const;

  /// Execute callback(T&) through a named active subcomponent's slot guard.
  /// Callback return values must be owned values or void; exceptions propagate.
  /// An expected UUID rejects replacement under the same registration name.
  /// Keep lifecycle operations on the owner thread outside affected guards.
  /// Concurrent mode drains guarded calls before deactivation/unload;
  /// single-threaded mode only checks state and rejects reentrant lifecycle changes.
  template<typename T, typename Callback>
  auto with_active_component(
    const std::string & slot_name, const std::string & name, Callback && callback,
    std::optional<std::string_view> expected_instance_id = {}) const
  -> std::invoke_result_t<Callback, T &>;
};

/**
 * Parent-facing access to one subcomponent. Lifecycle calls execute synchronously and
 * return the actual result; concurrent deactivation drains existing guarded calls.
 * Exposes lifecycle control, instance identity and typed instance access for one subcomponent.
 * Owned by its slot manager entry. Retains only the self view, never manager or plugin.
 * Component release detaches the self view and invalidates parent access.
 */
class SubcomponentContext
{
  template<class>
  friend class BasicSlotManager;
  friend class ComponentContext;
  std::shared_ptr<const ComponentContext> context_;

  explicit SubcomponentContext(std::shared_ptr<const ComponentContext> context)
  : context_(std::move(context)) {}

public:
  /// Maximum wait for concurrent guarded calls; max() waits indefinitely.
  using Timeout = ComponentContext::Timeout;

  /// Manager-owned subcomponent views cannot be copied.
  SubcomponentContext(const SubcomponentContext &) = delete;

  /// Manager-owned subcomponent views cannot be reassigned.
  SubcomponentContext & operator=(const SubcomponentContext &) = delete;

  /// Return this subcomponent's loaded native instance or adapted client proxy.
  /// T must match its interface; the static cast does not check type compatibility.
  /// Throws LifecycleError after release/replacement. The pointer acquires no lifecycle guard.
  template<typename T>
  std::shared_ptr<T> get_instance() const
  {
    return context_->visit([](auto & component) {
               auto instance = std::static_pointer_cast<T>(component.instance_);
               if (!instance) {throw LifecycleError("Subcomponent instance is not loaded");}
               return instance;
    });
  }

  /// Observe the subcomponent's active state without reserving a later transition.
  bool is_active() const {return context_->is_active();}

  /// Return whether this view represents a direct native C++ component.
  bool is_native() const
  {
    return context_->visit([](auto & component) {return component.is_native_;});
  }

  /// Synchronously activate this subcomponent on the owner thread, optionally with handover state.
  /// Return the actual transition result; the manager validates its current state.
  CallbackResult activate(HandoverState state = {}) const;

  /// Synchronously deactivate this subcomponent and return its optional handover state.
  /// Concurrent mode drains slot-guarded calls first; timeout does not bound plugin callbacks.
  /// Direct plugin pointers remain callable under the plugin's own state rules.
  DeactivationResult<HandoverState> deactivate(Timeout timeout = Timeout::max()) const;

  /// Borrow the subcomponent instance UUID; copy it before retaining it beyond runtime release.
  const std::string & get_instance_id() const {return context_->get_instance_id();}
};

/// Owning range for lazily enumerating subcomponent views.
class SubcomponentContextRange
{
  using Contexts = std::vector<std::shared_ptr<const SubcomponentContext>>;
  std::shared_ptr<const Contexts> contexts_;

public:
  class iterator
  {
    std::shared_ptr<const Contexts> contexts_;
    size_t position_ = 0;

public:
    using value_type = std::shared_ptr<const SubcomponentContext>;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::input_iterator_tag;

    iterator() = default;
    iterator(std::shared_ptr<const Contexts> contexts, size_t position)
    : contexts_(std::move(contexts)), position_(position) {}

    const value_type & operator*() const {return contexts_->at(position_);}
    iterator & operator++() {++position_; return *this;}
    bool operator==(const iterator & other) const
    {return contexts_ == other.contexts_ && position_ == other.position_;}
    bool operator!=(const iterator & other) const {return !(*this == other);}
  };

  explicit SubcomponentContextRange(Contexts contexts)
  : contexts_(std::make_shared<const Contexts>(std::move(contexts))) {}

  iterator begin() const {return {contexts_, 0};}
  iterator end() const {return {contexts_, contexts_->size()};}
};

template<typename T>
T ComponentContext::get_parameter(const std::string & name) const
{
  return visit([&](auto & component) {
             const auto & parameters = component.parameters_;
             if (!parameters.contains(name)) {
               throw std::runtime_error("Parameter '" + name + "' not found.");
             }
             return parameters.template get<T>(name);
  });
}

template<typename T, typename Callback>
auto ComponentContext::with_active_component(
  const std::string & slot_name, const std::string & name, Callback && callback,
  std::optional<std::string_view> expected_instance_id) const -> std::invoke_result_t<Callback, T &>
{
  return visit([&](auto & component) -> std::invoke_result_t<Callback, T &> {
             return component.subcomponent_slot(slot_name).template with_active<T>(
      name, std::forward<Callback>(callback), expected_instance_id);
  });
}

template<typename T>
ActiveComponent<T> ComponentContext::get_active_component(
  const std::string & slot_name, std::optional<std::string> component_name,
  std::optional<std::string_view> expected_instance_id) const
{
  return visit([&](auto & component) {
             return component.subcomponent_slot(slot_name).template get_active<T>(
      std::move(component_name), expected_instance_id);
  });
}

}  // namespace rpp
