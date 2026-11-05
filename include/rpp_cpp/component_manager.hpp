#pragma once

#include <any>
#include <atomic>
#include <chrono>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "plugin_def.hpp"
#include "component_slots.hpp"

namespace rpp
{
class ComponentContext;
class ComponentContextBuilder;

/**
 * Owns the component entries in one context slot.
 * Enforces registration, exclusive/multiple activation, lifecycle state,
 * generation identity, and guarded-call draining. Applications normally use
 * the owning ComponentContext; this class creates no update loop or executor.
 */
class ComponentManager : public std::enable_shared_from_this<ComponentManager>
{
  friend class ComponentContextBuilder;

public:
  using ContextFactory = std::function<ComponentContext(const std::string &)>;
  using Timeout = std::chrono::milliseconds;
  using Validator = std::function<CallbackResult(
        const ComponentContext &,
        const std::vector<std::shared_ptr<const ComponentContext>> &)>;

  using NamedValidators = std::map<std::string, Validator>;

private:
  struct Entry;
  ContextFactory factory_;
  mutable std::mutex mutex_;
  std::recursive_mutex build_mutex_;
  std::map<std::string, std::shared_ptr<Entry>> entries_;
  std::vector<std::string> order_;
  std::atomic<bool> closing_{false};
  mutable std::recursive_mutex mutation_;
  ActivationPolicy policy_;
  bool dynamic_;
  bool validating_ = false;
  std::string plugin_type_;
  std::string selected_;
  Validator validator_;
  NamedValidators named_validators_;
  /// Builder-only initial population; applications request runtime construction through load().
  CallbackResult adopt(
    const std::string & name, std::shared_ptr<ComponentContext> context,
    const std::string & component = "", bool initial = false);
  CallbackResult validate(const ComponentContext &, const std::string & name);

  std::shared_ptr<Entry> entry(const std::string & name) const;
  std::optional<CallbackResult> begin(
    Entry &, const std::string & operation,
    std::initializer_list<LifecycleState> allowed, bool during_close = false);
  static CallbackResult finish(Entry &, LifecycleState, CallbackResult);
  template<typename Callback>
  static CallbackResult capture(const std::string & operation, Callback callback);
  static CallbackResult drain(Entry &, Timeout);
  static DeactivationResult<std::any> stop(Entry &);

  struct ActiveCall
  {
    std::shared_ptr<Plugin> instance;
    std::function<void()> release;
    ActiveCall(std::shared_ptr<Plugin> plugin, std::function<void()> done)
    : instance(std::move(plugin)), release(std::move(done)) {}
    ActiveCall(const ActiveCall &) = delete;
    ActiveCall(ActiveCall &&) = default;
    ~ActiveCall() {instance.reset(); if (release) {release();}}
  };
  ActiveCall borrow(const std::string &, std::optional<uint64_t> generation);

public:
  explicit ComponentManager(
    ContextFactory factory = {},
    ActivationPolicy policy = ActivationPolicy::MULTIPLE, bool allow_dynamic_components = false,
    std::string plugin_type = "", Validator validator = {}, NamedValidators named_validators = {});
  ~ComponentManager();
  ComponentManager(const ComponentManager &) = delete;
  ComponentManager & operator=(const ComponentManager &) = delete;
  /// Return the fixed permission for runtime replacement/selection in this slot.
  bool allows_dynamic_components() const;
  /// Whether this thread is executing a guarded call in this slot, without waiting for it to finish.
  bool has_guarded_call_on_current_thread() const;
  /**
   * Activate the selected entry in an exclusive slot, or all loaded entries in a list slot.
   * Selection, state checks, and activation share the manager's mutation lock.
   * Append only newly activated contexts to activated for the caller's rollback;
   * the shared pointers keep those context objects alive. Already-active entries
   * are preserved. Stop and return on the first activation failure.
   */
  CallbackResult activate_selected(std::vector<std::shared_ptr<ComponentContext>> & activated);
  CallbackResult set_registration_validator(Validator validator);
  std::vector<std::shared_ptr<ComponentContext>> contexts() const;
  std::shared_ptr<ComponentContext> context(const std::string & name) const;
  std::shared_ptr<ComponentContext> selected_context() const;
  std::vector<std::shared_ptr<ComponentContext>> activation_contexts() const;
  std::vector<std::shared_ptr<ComponentContext>> deactivation_contexts() const;

  CallbackResult load(const std::string & name, const std::string & component = "");
  CallbackResult configure(const std::string & name, const ComponentContext * expected = nullptr);
  CallbackResult activate(
    const std::string & name, const std::any & state = {},
    const ComponentContext * expected = nullptr);
  DeactivationResult<std::any> deactivate(
    const std::string & name,
    Timeout timeout = Timeout::max(), const ComponentContext * expected = nullptr);
  CallbackResult cleanup(const std::string & name, const ComponentContext * expected = nullptr);
  CallbackResult unload(
    const std::string & name, Timeout timeout = Timeout::max(),
    const ComponentContext * expected = nullptr);
  CallbackResult switch_components(
    const std::string & source, const std::string & target,
    bool transfer_state = true, Timeout timeout = Timeout::max());
  CallbackResult reload(
    const std::string & name, bool transfer_state = false,
    Timeout timeout = Timeout::max(), std::function<void(const std::string &)> refresh = {});
  CallbackResult close(Timeout timeout = Timeout::max());
  ComponentInfo get_info(const std::string & name) const;
  LifecycleState get_state(const std::string & name) const;
  std::vector<ComponentInfo> list() const;

  template<typename T, typename Callback>
  auto with_active(
    const std::string & name, Callback && callback,
    std::optional<uint64_t> generation = {}) -> std::invoke_result_t<Callback, T &>
  {
    using Result = std::invoke_result_t<Callback, T &>;
    static_assert(
      !std::is_pointer_v<Result>&& !std::is_reference_v<Result>,
      "Return owned values, not borrowed plugin pointers/references");
    auto guard = borrow(name, generation);
    auto * plugin = dynamic_cast<T *>(guard.instance.get());
    if (!plugin) {throw LifecycleError("Managed plugin type mismatch");}
    return std::invoke(std::forward<Callback>(callback), *plugin);
  }
};
}  // namespace rpp
