#pragma once

#include <stdexcept>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace rpp
{

enum class LifecycleState {UNLOADED, UNCONFIGURED, INACTIVE, ACTIVE, ERROR};

class LifecycleError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

struct CallbackResult
{
  bool success = true;
  std::string message;

  static CallbackResult failure(std::string message)
  {
    return {false, std::move(message)};
  }

  void require_success(const std::string & operation = "") const
  {
    if (!success) {
      const auto reason = message.empty() ? "Operation failed" : message;
      throw LifecycleError(operation.empty() ? reason : operation + " failed: " + reason);
    }
  }
};

template<typename State>
struct DeactivationResult
{
  CallbackResult result;
  std::optional<State> state;
};

struct ComponentInfo
{
  std::string name;
  std::string component;
  uint64_t generation = 0;
  LifecycleState state = LifecycleState::UNLOADED;
  std::optional<std::string> transition;
  CallbackResult last_result;
  bool callable = false;
  size_t in_flight = 0;
};

}  // namespace rpp
