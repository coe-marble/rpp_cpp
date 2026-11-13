#pragma once

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include <kj/async-io.h>

namespace rpp
{

class ComponentCallExecutor final {
public:
  ComponentCallExecutor() = default;

  ~ComponentCallExecutor()
  {
    stop();
  }

  ComponentCallExecutor(const ComponentCallExecutor &) = delete;
  ComponentCallExecutor & operator=(const ComponentCallExecutor &) = delete;

  void start()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
      return;
    }
    stopping_ = false;
    thread_ = std::thread(&ComponentCallExecutor::run, this);
    started_ = true;
  }

  template<typename Function>
  auto call(Function && function)
  -> std::invoke_result_t<Function, kj::AsyncIoContext &>
  {
    using Result = std::invoke_result_t<Function, kj::AsyncIoContext &>;
    if (std::this_thread::get_id() == thread_.get_id()) {
      throw std::runtime_error(
                "Component call executor cannot block its own worker thread.");
    }

    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    auto callable = std::make_shared<std::decay_t<Function>>(
            std::forward<Function>(function));

    enqueue([promise, callable](kj::AsyncIoContext & io) {
        try {
          if constexpr (std::is_void_v<Result>) {
            (*callable)(io);
            promise->set_value();
          } else {
            promise->set_value((*callable)(io));
          }
        } catch (...) {
          promise->set_exception(std::current_exception());
        }
        });
    return future.get();
  }

  /// Retain a response on its RPC worker without copying its message. All
  /// returned views must be released before stopping/destroying this executor.
  template<typename Response>
  std::shared_ptr<std::decay_t<Response>> retain(Response && response)
  {
    using Owned = std::decay_t<Response>;
    const auto worker = std::this_thread::get_id();
    if (worker != thread_.get_id()) {
      throw std::runtime_error("RPC responses must be retained on the executor thread.");
    }
    return std::shared_ptr<Owned>(new Owned(std::forward<Response>(response)),
      [this, worker](Owned * value) {
        if (std::this_thread::get_id() == worker) {
          delete value;
        } else {
          // The existing queue is drained by stop(). Release all views before
          // shutdown so this cleanup is queued before the RPC context closes.
          enqueue([value](kj::AsyncIoContext &) {delete value;});
        }
      });
  }

  void stop()
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!started_) {
        return;
      }
      stopping_ = true;
    }
    condition_.notify_one();
    thread_.join();
    started_ = false;
  }

private:
  using Task = std::function<void(kj::AsyncIoContext &)>;

  void enqueue(Task task)
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!started_ || stopping_) {
        throw std::runtime_error("Component call executor is not running.");
      }
      tasks_.push_back(std::move(task));
    }
    condition_.notify_one();
  }

  void run()
  {
    auto io = kj::setupAsyncIo();
    while (true) {
      Task task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] {
            return stopping_ || !tasks_.empty();
                });
        if (stopping_ && tasks_.empty()) {
          return;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task(io);
    }
  }

  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Task> tasks_;
  std::thread thread_;
  bool started_ = false;
  bool stopping_ = false;
};

}  // namespace rpp
