#include "rpp_cpp/component_manager.hpp"

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "rpp_cpp/context.hpp"

namespace rpp
{
struct ComponentManager::Entry
{
  explicit Entry(std::string name) {info.name = std::move(name);}
  ComponentInfo info;
  std::shared_ptr<ComponentContext> context;
  std::recursive_mutex operation;
  std::mutex mutex;
  std::condition_variable changed;
  std::map<std::thread::id, size_t> callers;
  std::thread::id operation_thread;
  bool configured = false;
  std::atomic<bool> may_be_active{false};
};

bool ComponentManager::allows_dynamic_components() const
{
  return dynamic_;
}

CallbackResult ComponentManager::activate_selected(
  std::vector<std::shared_ptr<ComponentContext>> & activated)
{
  if (has_guarded_call_on_current_thread()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  std::lock_guard<std::recursive_mutex> mutation(mutation_);
  if (validating_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  for (const auto & child : activation_contexts()) {
    const bool was_active = get_state(child->owner_name_) == LifecycleState::ACTIVE;
    auto result = child->activate();
    if (!result.success) {return result;}
    if (!was_active) {activated.push_back(child);}
  }
  return {};
}

std::vector<std::shared_ptr<ComponentContext>> ComponentManager::contexts() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<ComponentContext>> result;
  for (const auto & name : order_) {
    auto item = entries_.at(name);
    std::lock_guard<std::mutex> state(item->mutex);
    if (item->context) {result.push_back(item->context);}
  }
  return result;
}

std::shared_ptr<ComponentContext> ComponentManager::context(const std::string & name) const
{
  auto item = entry(name);
  std::lock_guard<std::mutex> state(item->mutex);
  return item->context;
}

std::shared_ptr<ComponentContext> ComponentManager::selected_context() const
{
  std::string selected;
  {std::lock_guard<std::mutex> lock(mutex_); selected = selected_;}
  if (!selected.empty()) {
    auto found = context(selected);
    if (found) {return found;}
  }
  auto loaded = contexts();
  return loaded.empty() ? nullptr : loaded.front();
}

std::vector<std::shared_ptr<ComponentContext>> ComponentManager::activation_contexts() const
{
  if (policy_ != ActivationPolicy::EXCLUSIVE) {return contexts();}
  auto selected = selected_context();
  return selected ? std::vector<std::shared_ptr<ComponentContext>>{selected} :
         std::vector<std::shared_ptr<ComponentContext>>{};
}

std::vector<std::shared_ptr<ComponentContext>> ComponentManager::deactivation_contexts() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<ComponentContext>> result;
  for (const auto & name : order_) {
    const auto & item = entries_.at(name);
    std::lock_guard<std::mutex> state(item->mutex);
    if (item->context && item->may_be_active) {result.push_back(item->context);}
  }
  return result;
}

CallbackResult ComponentManager::validate(
  const ComponentContext & candidate,
  const std::string & name)
{
  if (!plugin_type_.empty() && !candidate.plugin_type_.empty() &&
    candidate.plugin_type_ != plugin_type_)
  {
    return CallbackResult::failure(
      "Expected component type " + plugin_type_ + ", got " +
      candidate.plugin_type_);
  }
  auto named = named_validators_.find(name);
  if (!validator_ && named == named_validators_.end()) {return {};}
  std::vector<std::shared_ptr<const ComponentContext>> registered;
  for (const auto & key : order_) {
    const auto & item = entries_.at(key);
    if (key != name && item->context) {registered.push_back(item->context);}
  }
  validating_ = true;
  auto result = capture(
    "registration validator", [&]() {
      if (validator_) {
        auto result = validator_(candidate, registered);
        if (!result.success) {return result;}
      }
      return named == named_validators_.end() ? CallbackResult{} :
      named->second(candidate, registered);
    });
  validating_ = false;
  return result;
}

bool ComponentManager::has_guarded_call_on_current_thread() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto & [name, item] : entries_) {
    (void)name;
    std::lock_guard<std::mutex> state(item->mutex);
    if (item->callers.count(std::this_thread::get_id())) {return true;}
  }
  return false;
}

CallbackResult ComponentManager::set_registration_validator(Validator validator)
{
  if (has_guarded_call_on_current_thread()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  std::lock_guard<std::recursive_mutex> mutation(mutation_);
  if (validating_) {
    return CallbackResult::failure("Cannot replace a running registration validator");
  }
  validator_ = std::move(validator);
  return {};
}

CallbackResult ComponentManager::adopt(
  const std::string & name,
  std::shared_ptr<ComponentContext> candidate, const std::string & component, bool initial)
{
  if (has_guarded_call_on_current_thread()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  std::lock_guard<std::recursive_mutex> mutation(mutation_);
  if (validating_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  if (!initial && !dynamic_) {
    return CallbackResult::failure("Dynamic component modification is disabled");
  }
  if (name.empty() || entries_.count(name) || !candidate) {
    return CallbackResult::failure("Component name must be nonempty and unique within its slot");
  }
  if (!candidate->owner_.expired()) {
    return CallbackResult::failure("Component context already belongs to a slot");
  }
  auto result = validate(*candidate, name);
  if (!result.success) {return result;}
  auto item = std::make_shared<Entry>(name);
  item->info.component = component;
  item->info.generation = 1;
  item->info.state = LifecycleState::UNCONFIGURED;
  candidate->owner_ = weak_from_this();
  candidate->owner_name_ = name;
  item->context = std::move(candidate);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.emplace(name, std::move(item));
    order_.push_back(name);
  }
  {std::lock_guard<std::mutex> lock(mutex_); if (selected_.empty()) {selected_ = name;}}
  return {};
}

std::shared_ptr<ComponentManager::Entry> ComponentManager::entry(const std::string & name) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = entries_.find(name);
  if (found == entries_.end()) {throw LifecycleError("Unknown component: " + name);}
  return found->second;
}

ComponentInfo ComponentManager::get_info(const std::string & name) const
{
  auto item = entry(name);
  std::lock_guard<std::mutex> lock(item->mutex);
  return item->info;
}

std::optional<CallbackResult> ComponentManager::begin(
  Entry & item, const std::string & operation,
  std::initializer_list<LifecycleState> allowed, bool during_close)
{
  std::lock_guard<std::mutex> lock(item.mutex);
  std::string reason;
  if (item.callers.count(std::this_thread::get_id())) {
    reason = "Cannot manage an entry from inside its guarded call";
  } else if (item.info.transition) {
    reason = "A lifecycle operation is already in progress";
  } else if (closing_ && !during_close) {
    reason = "Component manager is closing";
  } else if (std::find(allowed.begin(), allowed.end(), item.info.state) == allowed.end()) {
    reason = "Invalid lifecycle state for " + operation;
  }
  if (!reason.empty()) {
    auto failure = CallbackResult::failure(reason);
    if (!item.info.transition) {item.info.last_result = failure;}
    return failure;
  }
  item.info.transition = operation;
  item.operation_thread = std::this_thread::get_id();
  item.info.callable = false;
  return {};
}

CallbackResult ComponentManager::finish(Entry & item, LifecycleState state, CallbackResult result)
{
  std::lock_guard<std::mutex> lock(item.mutex);
  item.info.state = state;
  item.info.last_result = result;
  item.info.transition.reset();
  item.operation_thread = {};
  item.info.callable = state == LifecycleState::ACTIVE && result.success;
  item.changed.notify_all();
  return result;
}

template<typename Callback>
CallbackResult ComponentManager::capture(const std::string & operation, Callback callback)
{
  try {
    return callback();
  } catch (const std::exception & error) {
    return CallbackResult::failure(operation + " failed: " + error.what());
  } catch (...) {
    return CallbackResult::failure(operation + " failed: unknown exception");
  }
}

CallbackResult ComponentManager::drain(Entry & item, Timeout timeout)
{
  std::unique_lock<std::mutex> lock(item.mutex);
  auto empty = [&] {return item.info.in_flight == 0;};
  if (timeout == Timeout::max()) {
    item.changed.wait(lock, empty);
  } else if (!item.changed.wait_for(lock, timeout, empty)) {
    return CallbackResult::failure("Timed out draining active calls; instance retained");
  }
  return {};
}

CallbackResult ComponentManager::load(const std::string & name, const std::string & component)
{
  if (has_guarded_call_on_current_thread()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  if (!dynamic_) {return CallbackResult::failure("Dynamic component modification is disabled");}
  std::lock_guard<std::recursive_mutex> mutation(mutation_);
  if (validating_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  if (name.empty()) {return CallbackResult::failure("Component name must not be empty");}
  std::shared_ptr<Entry> item;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_) {return CallbackResult::failure("Component manager is closing");}
    auto inserted = entries_.emplace(name, nullptr);
    if (inserted.second) {
      inserted.first->second = std::make_shared<Entry>(name);
      order_.push_back(name);
    }
    item = inserted.first->second;
  }
  std::lock_guard<std::recursive_mutex> operation(item->operation);
  auto rejected = begin(*item, "load", {LifecycleState::UNLOADED});
  if (rejected) {return *rejected;}
  const auto previous_component = item->info.component;
  bool admission_rejected = false;
  {
    std::lock_guard<std::mutex> lock(item->mutex);
    if (!component.empty()) {item->info.component = component;}
  }
  auto result = capture(
    "load", [&] {
      if (item->info.component.empty()) {
        return CallbackResult::failure("A component path is required");
      }
      std::lock_guard<std::recursive_mutex> lock(build_mutex_);
      if (!factory_) {
        return CallbackResult::failure("No component builder is attached to this slot");
      }
      auto candidate = std::make_shared<ComponentContext>(factory_(item->info.component));
      auto admitted = validate(*candidate, name);
      if (!admitted.success) {
        auto released = candidate->close_impl();
        if (!released.success) {
          candidate->owner_ = weak_from_this();
          candidate->owner_name_ = name;
          std::lock_guard<std::mutex> state(item->mutex);
          item->context = std::move(candidate);
          return released;
        }
        admission_rejected = true;
        return admitted;
      }
      candidate->owner_ = weak_from_this();
      candidate->owner_name_ = name;
      {
        std::lock_guard<std::mutex> state_lock(item->mutex);
        item->context = std::move(candidate);
        ++item->info.generation;
      }
      item->configured = false;
      item->may_be_active = false;
      return CallbackResult{};
    });
  if (admission_rejected) {
    finish(*item, LifecycleState::UNLOADED, result);
    std::lock_guard<std::mutex> lock(mutex_);
    if (item->info.generation == 0) {
      entries_.erase(name);
      order_.erase(std::remove(order_.begin(), order_.end(), name), order_.end());
    } else {
      std::lock_guard<std::mutex> state(item->mutex);
      item->info.component = previous_component;
    }
    return result;
  }
  return finish(
    *item, result.success ? LifecycleState::UNCONFIGURED : LifecycleState::ERROR,
    result);
}

CallbackResult ComponentManager::configure(
  const std::string & name,
  const ComponentContext * expected)
{
  if (has_guarded_call_on_current_thread()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  std::lock_guard<std::recursive_mutex> mutation(mutation_);
  if (validating_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  auto item = entry(name);
  if (expected && item->context.get() != expected) {
    return CallbackResult::failure("Stale component context generation");
  }
  std::lock_guard<std::recursive_mutex> operation(item->operation);
  auto rejected = begin(*item, "configure", {LifecycleState::UNCONFIGURED});
  if (rejected) {return *rejected;}
  item->configured = true;
  auto result = capture("configure", [&] {return item->context->configure_impl();});
  return finish(*item, result.success ? LifecycleState::INACTIVE : LifecycleState::ERROR, result);
}

CallbackResult ComponentManager::activate(
  const std::string & name, const std::any & state,
  const ComponentContext * expected)
{
  if (has_guarded_call_on_current_thread()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  std::lock_guard<std::recursive_mutex> mutation(mutation_);
  if (validating_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  if (expected && entry(name)->context.get() != expected) {
    return CallbackResult::failure("Stale component context generation");
  }
  if (!dynamic_ && policy_ == ActivationPolicy::EXCLUSIVE && name != selected_) {
    return CallbackResult::failure("Dynamic component selection is disabled");
  }
  if (policy_ == ActivationPolicy::EXCLUSIVE) {
    for (const auto & [key, other] : entries_) {
      if (key != name && other->may_be_active) {
        return CallbackResult::failure("An exclusive slot already has an active component");
      }
    }
  }
  auto item = entry(name);
  std::lock_guard<std::recursive_mutex> operation(item->operation);
  if (item->context && item->context->external_component_process_) {
    return CallbackResult::failure("Adapted lifecycle activation is not supported yet");
  }
  if (item->info.state == LifecycleState::ACTIVE && !item->info.transition && !closing_) {
    return {};
  }
  auto rejected = begin(*item, "activate", {LifecycleState::INACTIVE});
  if (rejected) {return *rejected;}
  item->may_be_active = true;
  auto result = capture("activate", [&] {return item->context->activate_impl(state);});
  if (!result.success) {item->may_be_active = item->context->has_active_work();}
  if (result.success && policy_ == ActivationPolicy::EXCLUSIVE) {
    std::lock_guard<std::mutex> lock(mutex_);
    selected_ = name;
  }
  return finish(*item, result.success ? LifecycleState::ACTIVE : LifecycleState::ERROR, result);
}

DeactivationResult<std::any> ComponentManager::stop(Entry & item)
{
  try {
    auto result = item.context->deactivate_impl();
    if (result.result.success) {item.may_be_active = false;} else {result.state.reset();}
    return result;
  } catch (const std::exception & error) {
    return {CallbackResult::failure(std::string("deactivate failed: ") + error.what()), {}};
  } catch (...) {
    return {CallbackResult::failure("deactivate failed: unknown exception"), {}};
  }
}

DeactivationResult<std::any> ComponentManager::deactivate(
  const std::string & name, Timeout timeout,
  const ComponentContext * expected)
{
  if (has_guarded_call_on_current_thread()) {
    return {CallbackResult::failure("Cannot manage a slot from inside its guarded call"), {}};
  }
  std::lock_guard<std::recursive_mutex> mutation(mutation_);
  if (validating_) {
    return {CallbackResult::failure("Cannot mutate a slot from its registration validator"), {}};
  }
  auto item = entry(name);
  if (expected && item->context.get() != expected) {
    return {CallbackResult::failure("Stale component context generation"), {}};
  }
  std::lock_guard<std::recursive_mutex> operation(item->operation);
  if (item->info.state == LifecycleState::INACTIVE && !item->info.transition) {return {};}
  if (item->info.state == LifecycleState::ERROR && !item->may_be_active) {
    return {CallbackResult::failure("Failed configuration requires cleanup before reconfiguration"),
      {}};
  }
  auto rejected = begin(*item, "deactivate", {LifecycleState::ACTIVE, LifecycleState::ERROR});
  if (rejected) {return {*rejected, {}};}
  auto drained = drain(*item, timeout);
  auto result = drained.success ? stop(*item) : DeactivationResult<std::any>{drained, {}};
  finish(
    *item, result.result.success ? LifecycleState::INACTIVE : LifecycleState::ERROR,
    result.result);
  return result;
}

CallbackResult ComponentManager::cleanup(
  const std::string & name,
  const ComponentContext * expected)
{
  if (has_guarded_call_on_current_thread()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  std::lock_guard<std::recursive_mutex> mutation(mutation_);
  if (validating_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  auto item = entry(name);
  if (expected && item->context.get() != expected) {
    return CallbackResult::failure("Stale component context generation");
  }
  std::lock_guard<std::recursive_mutex> operation(item->operation);
  if (item->may_be_active) {
    return CallbackResult::failure("Deactivate the component before cleanup");
  }
  auto rejected = begin(
    *item, "cleanup", {LifecycleState::UNCONFIGURED,
      LifecycleState::INACTIVE, LifecycleState::ERROR});
  if (rejected) {return *rejected;}
  if (!item->context) {
    return finish(
      *item, LifecycleState::ERROR,
      CallbackResult::failure("No loaded context; unload before retrying load"));
  }
  auto result = capture(
    "cleanup", [&] {
      return item->context->cleanup_impl();
    });
  if (result.success) {item->configured = false;}
  return finish(
    *item, result.success ? LifecycleState::UNCONFIGURED : LifecycleState::ERROR,
    result);
}

CallbackResult ComponentManager::unload(
  const std::string & name, Timeout timeout,
  const ComponentContext * expected)
{
  if (has_guarded_call_on_current_thread()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  if (!dynamic_ && !closing_) {
    return CallbackResult::failure("Dynamic component modification is disabled");
  }
  std::lock_guard<std::recursive_mutex> mutation(mutation_);
  if (validating_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  auto item = entry(name);
  if (expected && item->context.get() != expected) {
    return CallbackResult::failure("Stale component context generation");
  }
  std::lock_guard<std::recursive_mutex> operation(item->operation);
  auto rejected = begin(
    *item, "unload", {LifecycleState::UNLOADED, LifecycleState::UNCONFIGURED,
      LifecycleState::INACTIVE, LifecycleState::ACTIVE, LifecycleState::ERROR}, true);
  if (rejected) {return *rejected;}
  auto result = drain(*item, timeout);
  if (result.success && item->context) {
    if (item->may_be_active) {result = stop(*item).result;}
    if (result.success) {
      result = capture(
        "deactivate descendants", [&] {
          return item->context->deactivate_components_for_shutdown(timeout);
        });
    }
    if (result.success && item->configured) {
      result = capture("cleanup", [&] {return item->context->cleanup_impl();});
      if (result.success) {item->configured = false;}
    }
    if (result.success) {
      result = capture("unload", [&] {return item->context->close_impl();});
      if (result.success) {std::lock_guard<std::mutex> lock(item->mutex); item->context.reset();}
    }
  }
  return finish(*item, result.success ? LifecycleState::UNLOADED : LifecycleState::ERROR, result);
}

CallbackResult ComponentManager::switch_components(
  const std::string & source, const std::string & target,
  bool transfer_state, Timeout timeout)
{
  if (has_guarded_call_on_current_thread()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  if (!dynamic_) {return CallbackResult::failure("Dynamic component modification is disabled");}
  std::lock_guard<std::recursive_mutex> mutation(mutation_);
  if (validating_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  if (source == target) {return CallbackResult::failure("Switch source and target must differ");}
  auto old = entry(source), next = entry(target);
  std::scoped_lock operations(old->operation, next->operation);
  for (const auto & item : {old, next}) {
    std::lock_guard<std::mutex> lock(item->mutex);
    if (item->callers.count(std::this_thread::get_id())) {
      return CallbackResult::failure("Cannot switch from inside a guarded call");
    }
  }
  if (old->info.state != LifecycleState::ACTIVE) {
    return CallbackResult::failure("Switch source must be active");
  }
  if (next->info.state == LifecycleState::UNLOADED) {
    auto result = load(target, "");
    if (!result.success) {return result;}
  }
  if (next->info.state == LifecycleState::UNCONFIGURED) {
    auto result = configure(target);
    if (!result.success) {return result;}
  }
  if (next->info.state != LifecycleState::INACTIVE) {
    return CallbackResult::failure("Switch target must be inactive");
  }
  if (transfer_state) {
    auto first = old->context->get_instance<Plugin>();
    auto second = next->context->get_instance<Plugin>();
    if (first->handover_state_type() != second->handover_state_type()) {
      return CallbackResult::failure("Incompatible handover state types");
    }
  }
  auto stopped = deactivate(source, timeout);
  if (!stopped.result.success) {return stopped.result;}
  return activate(target, transfer_state && stopped.state ? *stopped.state : std::any{});
}

CallbackResult ComponentManager::reload(
  const std::string & name, bool transfer_state, Timeout timeout,
  std::function<void(const std::string &)> refresh)
{
  if (has_guarded_call_on_current_thread()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  if (!dynamic_) {return CallbackResult::failure("Dynamic component modification is disabled");}
  std::lock_guard<std::recursive_mutex> mutation(mutation_);
  if (validating_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  auto item = entry(name);
  std::lock_guard<std::recursive_mutex> operation(item->operation);
  const bool was_active = item->info.state == LifecycleState::ACTIVE;
  std::any snapshot;
  if (was_active) {
    auto stopped = deactivate(name, timeout);
    if (!stopped.result.success) {return stopped.result;}
    if (transfer_state && stopped.state) {snapshot = *stopped.state;}
  }
  auto result = unload(name, timeout);
  if (!result.success) {return result;}
  if (refresh) {
    result = capture("refresh", [&] {refresh(item->info.component); return CallbackResult{};});
    if (!result.success) {return finish(*item, LifecycleState::UNLOADED, result);}
  }
  result = load(name, "");
  if (result.success) {result = configure(name);}
  if (result.success && was_active) {result = activate(name, snapshot);}
  return result;
}

CallbackResult ComponentManager::close(Timeout timeout)
{
  if (has_guarded_call_on_current_thread()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  std::lock_guard<std::recursive_mutex> mutation(mutation_);
  if (validating_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  std::vector<std::string> names;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto & name : order_) {
      auto item = entries_.at(name);
      std::lock_guard<std::mutex> state_lock(item->mutex);
      if (item->callers.count(std::this_thread::get_id()) ||
        item->operation_thread == std::this_thread::get_id())
      {return CallbackResult::failure("Cannot close manager from inside managed plugin code");}
    }
    closing_ = true;
    names = order_;
  }
  CallbackResult result;
  for (auto name = names.rbegin(); name != names.rend(); ++name) {
    auto stopped = unload(*name, timeout);
    if (!stopped.success && result.success) {result = stopped;}
  }
  return result;
}

ComponentManager::ComponentManager(
  ContextFactory factory, ActivationPolicy policy,
  bool allow_dynamic_components, std::string plugin_type, Validator validator,
  NamedValidators named_validators)
: factory_(std::move(factory)), policy_(policy), dynamic_(allow_dynamic_components),
  plugin_type_(std::move(plugin_type)), validator_(std::move(validator)),
  named_validators_(std::move(named_validators)) {}

ComponentManager::~ComponentManager()
{
  auto result = close(Timeout::zero());
  if (!result.success) {
    // Failed shutdown is not permission to unload active code. Keep only
    // unreleased entries alive; callers use explicit close to report/retry.
    static auto * retained_mutex = new std::mutex;
    static auto * retained = new std::vector<std::shared_ptr<Entry>>;
    std::lock_guard<std::mutex> lock(*retained_mutex);
    for (const auto & [name, item] : entries_) {
      (void)name;
      if (item->context) {retained->push_back(item);}
    }
  }
}

ComponentManager::ActiveCall ComponentManager::borrow(
  const std::string & name,
  std::optional<uint64_t> generation)
{
  auto item = entry(name);
  const auto thread = std::this_thread::get_id();
  std::shared_ptr<Plugin> instance;
  {
    std::lock_guard<std::mutex> lock(item->mutex);
    if (generation && *generation != item->info.generation) {
      throw LifecycleError("Stale component generation");
    }
    if (!item->info.callable || item->info.state != LifecycleState::ACTIVE) {
      throw LifecycleError("Component is not available for active calls");
    }
    instance = item->context->get_instance<Plugin>();
    ++item->info.in_flight;
    ++item->callers[thread];
  }
  return ActiveCall(
    std::move(instance), [item, thread] {
      std::lock_guard<std::mutex> lock(item->mutex);
      --item->info.in_flight;
      if (--item->callers[thread] == 0) {item->callers.erase(thread);}
      item->changed.notify_all();
    });
}

LifecycleState ComponentManager::get_state(const std::string & name) const
{
  return get_info(name).state;
}

std::vector<ComponentInfo> ComponentManager::list() const
{
  std::vector<std::string> names;
  {std::lock_guard<std::mutex> lock(mutex_); names = order_;}
  std::vector<ComponentInfo> result;
  for (const auto & name : names) {
    result.push_back(get_info(name));
  }
  return result;
}
}  // namespace rpp
