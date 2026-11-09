#pragma once

#include <any>
#include "component_status.hpp"

namespace rpp
{

class ComponentContext;

class Plugin {
public:
  virtual ~Plugin() = default;

  // Plugins receive the context facade; runtime and managers execute lifecycle operations.
  virtual CallbackResult on_configure(const ComponentContext &) {return {};}
  virtual CallbackResult on_cleanup() {return {};}

  CallbackResult configure(const ComponentContext & context)
  {
    return invoke_lifecycle("on_configure", [&]() {return on_configure(context);});
  }

  CallbackResult cleanup()
  {
    return invoke_lifecycle("on_cleanup", [&]() {return on_cleanup();});
  }
  virtual void reset() {}

  // Internal dispatch entry points; typed callbacks live on LifecyclePlugin.
  virtual const std::type_info & handover_state_type() const {return typeid(void);}
  virtual CallbackResult validate_handover_state(const std::any &) const
  {
    return CallbackResult::failure("Plugin does not implement lifecycle activation");
  }

  virtual CallbackResult activate_state(const std::any & = {})
  {
    return CallbackResult::failure("Plugin does not implement lifecycle activation");
  }

  virtual DeactivationResult<std::any> deactivate_state()
  {
    return {CallbackResult::failure("Plugin does not implement lifecycle deactivation"), {}};
  }

protected:
  template<typename Callback>
  static CallbackResult invoke_lifecycle(const std::string & operation, Callback && callback)
  {
    try {
      return callback();
    } catch (const std::exception & error) {
      return CallbackResult::failure(operation + " failed: " + error.what());
    } catch (...) {
      return CallbackResult::failure(operation + " failed: unknown exception");
    }
  }
};

template<typename State>
class LifecyclePlugin : public Plugin
{
public:
  const std::type_info & handover_state_type() const final {return typeid(State);}
  virtual CallbackResult on_activate(const std::optional<State> & state) = 0;
  virtual DeactivationResult<State> on_deactivate() = 0;

  CallbackResult validate_handover_state(const std::any & state) const final
  {
    if (state.has_value() && state.type() != typeid(State)) {
      return CallbackResult::failure("on_activate received an incompatible handover state");
    }
    return {};
  }

  CallbackResult activate_state(const std::any & state = {}) final
  {
    auto validation = validate_handover_state(state);
    if (!validation.success) {return validation;}
    return invoke_lifecycle("on_activate", [&]() {
               const auto owned = state.has_value() ?
               std::optional<State>(std::any_cast<const State &>(state)) : std::nullopt;
               return on_activate(owned);
    });
  }

  DeactivationResult<std::any> deactivate_state() final
  {
    std::optional<std::any> snapshot;
    auto result = invoke_lifecycle("on_deactivate", [&]() {
          const auto deactivation = on_deactivate();
          if (deactivation.result.success && deactivation.state) {
            snapshot.emplace(*deactivation.state);
          }
          return deactivation.result;
    });
    return {result, result.success ? std::move(snapshot) : std::nullopt};
  }
};

template<>
class LifecyclePlugin<void>: public Plugin
{
public:
  virtual CallbackResult on_activate() {return {};}
  virtual CallbackResult on_deactivate() {return {};}

  CallbackResult validate_handover_state(const std::any & state) const final
  {
    if (state.has_value()) {
      return CallbackResult::failure("on_activate accepts no state");
    }
    return {};
  }

  CallbackResult activate_state(const std::any & state = {}) final
  {
    auto validation = validate_handover_state(state);
    if (!validation.success) {return validation;}
    return invoke_lifecycle("on_activate", [&]() {return on_activate();});
  }

  DeactivationResult<std::any> deactivate_state() final
  {
    return {invoke_lifecycle("on_deactivate", [&]() {return on_deactivate();}), {}};
  }
};


}  // namespace rpp
