#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <utility>

namespace rpp
{
namespace detail
{
// BasicLockable for the owner-thread policy. Guards compile to no operations.
struct NoMutex
{
  void lock() noexcept {}

  void unlock() noexcept {}

  bool try_lock() noexcept {return true;}
};

struct LocalCalls
{
  // Reentrancy protection, not concurrent-call tracking.
  size_t depth = 0;
  bool active() const {return depth != 0;}

  void enter() {++depth;}

  void leave() {--depth;}
};

// Concurrent callers are counted by the slot gate, without identifying threads.
struct NoLocalCalls
{
  bool active() const {return false;}

  void enter() noexcept {}

  void leave() noexcept {}
};

struct NoCondition
{
  void notify_all() noexcept {}
};
}  // namespace detail

/// All framework access belongs to one thread. Plugin-owned workers must stop at cleanup.
struct SingleThreaded
{
  /// Framework calls and lifecycle changes share one owning thread.
  static constexpr bool concurrent = false;

  /// Slot guards perform no mutex operations in this mode.
  using Mutex = detail::NoMutex;

  /// Ordinary flags need no cross-thread synchronization.
  using Flag = bool;

  /// Consume and clear a pending flag on the owning thread.
  static bool take(Flag & flag) {return std::exchange(flag, false);}

  /// Local nesting counter used to reject unsafe owner-thread reentrancy.
  using LocalCalls = detail::LocalCalls;

  /// Single-threaded draining never blocks on a condition variable.
  using Condition = detail::NoCondition;
};

/// Lifecycle and membership operations belong to one owning thread in both modes.
/// Concurrent guarded calls use a slot gate; transitions close admission and drain it.
/// Direct plugin pointers remain unguarded. Moves and destruction require exclusive ownership.
struct Concurrent
{
  /// Guarded worker calls may overlap operations on the lifecycle owner thread.
  static constexpr bool concurrent = true;

  /// The slot's admission, snapshots and call counts share one mutex.
  using Mutex = std::mutex;

  /// Atomic flags support worker self-deactivation and observable runtime state.
  using Flag = std::atomic<bool>;

  /// Atomically consume and clear a pending flag.
  static bool take(Flag & flag) {return flag.exchange(false);}

  /// Concurrent guards use counts at the slot gate, without tracking callers.
  using LocalCalls = detail::NoLocalCalls;

  /// Wake the lifecycle owner when guarded calls have drained.
  using Condition = std::condition_variable;
};
}  // namespace rpp
