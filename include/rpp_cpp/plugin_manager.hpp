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

#include "context.hpp"

namespace rpp
{

class PluginManager
{
public:
  using ContextFactory = std::function<ComponentContext(const std::string &)>;
  using Timeout = std::chrono::milliseconds;

private:
  struct Entry;
  ContextFactory factory_;
  mutable std::mutex mutex_;
  std::recursive_mutex build_mutex_;
  std::map<std::string, std::shared_ptr<Entry>> entries_;
  std::vector<std::string> order_;
  std::atomic<bool> closing_{false};

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
  explicit PluginManager(ContextFactory factory);
  explicit PluginManager(const std::string & workspace = "");
  ~PluginManager();
  PluginManager(const PluginManager &) = delete;
  PluginManager & operator=(const PluginManager &) = delete;

  CallbackResult load(const std::string & name, const std::string & component = "");
  CallbackResult configure(const std::string & name);
  CallbackResult activate(const std::string & name, const std::any & state = {});
  DeactivationResult<std::any> deactivate(
    const std::string & name,
    Timeout timeout = Timeout::max());
  CallbackResult cleanup(const std::string & name);
  CallbackResult unload(const std::string & name, Timeout timeout = Timeout::max());
  CallbackResult switch_plugins(
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
