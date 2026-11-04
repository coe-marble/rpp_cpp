#include "rpp_cpp/plugin_manager.hpp"

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "rpp_cpp/context_builder.hpp"

namespace rpp
{
struct PluginManager::Entry
{
  explicit Entry(std::string name) {info.name = std::move(name);}
  ComponentInfo info;
  std::unique_ptr<ComponentContext> context;
  std::recursive_mutex operation;
  std::mutex mutex;
  std::condition_variable changed;
  std::map<std::thread::id, size_t> callers;
  std::thread::id operation_thread;
  bool configured = false;
  bool may_be_active = false;
};

std::shared_ptr<PluginManager::Entry> PluginManager::entry(const std::string & name) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = entries_.find(name);
  if (found == entries_.end()) {throw LifecycleError("Unknown component: " + name);}
  return found->second;
}

ComponentInfo PluginManager::get_info(const std::string & name) const
{
  auto item = entry(name);
  std::lock_guard<std::mutex> lock(item->mutex);
  return item->info;
}

std::optional<CallbackResult> PluginManager::begin(
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
    reason = "Plugin manager is closing";
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

CallbackResult PluginManager::finish(Entry & item, LifecycleState state, CallbackResult result)
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
CallbackResult PluginManager::capture(const std::string & operation, Callback callback)
{
  try {
    return callback();
  } catch (const std::exception & error) {
    return CallbackResult::failure(operation + " failed: " + error.what());
  } catch (...) {
    return CallbackResult::failure(operation + " failed: unknown exception");
  }
}

CallbackResult PluginManager::drain(Entry & item, Timeout timeout)
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

CallbackResult PluginManager::load(const std::string & name, const std::string & component)
{
  if (name.empty()) {return CallbackResult::failure("Component name must not be empty");}
  std::shared_ptr<Entry> item;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_) {return CallbackResult::failure("Plugin manager is closing");}
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
      item->context = std::make_unique<ComponentContext>(factory_(item->info.component));
      {
        std::lock_guard<std::mutex> state_lock(item->mutex);
        ++item->info.generation;
      }
      item->configured = false;
      item->may_be_active = false;
      return CallbackResult{};
    });
  return finish(
    *item, result.success ? LifecycleState::UNCONFIGURED : LifecycleState::ERROR,
    result);
}

CallbackResult PluginManager::configure(const std::string & name)
{
  auto item = entry(name);
  std::lock_guard<std::recursive_mutex> operation(item->operation);
  auto rejected = begin(*item, "configure", {LifecycleState::UNCONFIGURED});
  if (rejected) {return *rejected;}
  item->configured = true;
  auto result = capture("configure", [&] {return item->context->configure();});
  return finish(*item, result.success ? LifecycleState::INACTIVE : LifecycleState::ERROR, result);
}

CallbackResult PluginManager::activate(const std::string & name, const std::any & state)
{
  auto item = entry(name);
  std::lock_guard<std::recursive_mutex> operation(item->operation);
  if (item->info.state == LifecycleState::ACTIVE && !item->info.transition && !closing_) {
    return {};
  }
  auto rejected = begin(*item, "activate", {LifecycleState::INACTIVE});
  if (rejected) {return *rejected;}
  item->may_be_active = true;
  auto result = capture("activate", [&] {return item->context->activate(state);});
  return finish(*item, result.success ? LifecycleState::ACTIVE : LifecycleState::ERROR, result);
}

DeactivationResult<std::any> PluginManager::stop(Entry & item)
{
  try {
    auto result = item.context->deactivate();
    if (result.result.success) {item.may_be_active = false;} else {result.state.reset();}
    return result;
  } catch (const std::exception & error) {
    return {CallbackResult::failure(std::string("deactivate failed: ") + error.what()), {}};
  } catch (...) {
    return {CallbackResult::failure("deactivate failed: unknown exception"), {}};
  }
}

DeactivationResult<std::any> PluginManager::deactivate(const std::string & name, Timeout timeout)
{
  auto item = entry(name);
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

CallbackResult PluginManager::cleanup(const std::string & name)
{
  auto item = entry(name);
  std::lock_guard<std::recursive_mutex> operation(item->operation);
  if (item->may_be_active) {
    return CallbackResult::failure("Deactivate the component before cleanup");
  }
  auto rejected = begin(*item, "cleanup", {LifecycleState::INACTIVE, LifecycleState::ERROR});
  if (rejected) {return *rejected;}
  auto result = capture(
    "cleanup", [&] {
      return item->context ? item->context->cleanup() : CallbackResult{};
    });
  if (result.success) {item->configured = false;}
  return finish(
    *item, result.success ? LifecycleState::UNCONFIGURED : LifecycleState::ERROR,
    result);
}

CallbackResult PluginManager::unload(const std::string & name, Timeout timeout)
{
  auto item = entry(name);
  std::lock_guard<std::recursive_mutex> operation(item->operation);
  auto rejected = begin(
    *item, "unload", {LifecycleState::UNLOADED, LifecycleState::UNCONFIGURED,
      LifecycleState::INACTIVE, LifecycleState::ACTIVE, LifecycleState::ERROR}, true);
  if (rejected) {return *rejected;}
  auto result = drain(*item, timeout);
  if (result.success && item->context) {
    if (item->may_be_active) {result = stop(*item).result;}
    if (result.success && item->configured) {
      result = capture("cleanup", [&] {return item->context->cleanup();});
      if (result.success) {item->configured = false;}
    }
    if (result.success) {
      result = capture("unload", [&] {return item->context->close();});
      if (result.success) {item->context.reset();}
    }
  }
  return finish(*item, result.success ? LifecycleState::UNLOADED : LifecycleState::ERROR, result);
}

CallbackResult PluginManager::switch_plugins(
  const std::string & source, const std::string & target,
  bool transfer_state, Timeout timeout)
{
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

CallbackResult PluginManager::reload(
  const std::string & name, bool transfer_state, Timeout timeout,
  std::function<void(const std::string &)> refresh)
{
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

CallbackResult PluginManager::close(Timeout timeout)
{
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

PluginManager::PluginManager(ContextFactory factory)
: factory_(std::move(factory)) {}

PluginManager::PluginManager(const std::string & workspace)
{
  auto data = std::make_shared<RppDataManager>(RPP_HOME, workspace);
  auto builder = std::make_shared<ComponentContextBuilder>(*data);
  factory_ = [data, builder](const std::string & path) {
      (void)data;
      return builder->build_component_from_path(path);
    };
}

PluginManager::~PluginManager()
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

PluginManager::ActiveCall PluginManager::borrow(
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

LifecycleState PluginManager::get_state(const std::string & name) const
{
  return get_info(name).state;
}

std::vector<ComponentInfo> PluginManager::list() const
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
