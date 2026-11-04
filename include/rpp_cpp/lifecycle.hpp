#pragma once

#include <stdexcept>
#include <optional>
#include <string>
#include <utility>

namespace rpp
{

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

  void require_success(const std::string & operation) const
  {
    if (!success) {
      throw LifecycleError(operation + " failed: " + message);
    }
  }
};

template<typename State>
struct DeactivationResult
{
  CallbackResult result;
  std::optional<State> state;
};

}  // namespace rpp
