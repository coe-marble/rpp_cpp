#pragma once

#include "component_status.hpp"
#include "handover_state.hpp"

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
  virtual CallbackResult validate_handover_state(const HandoverState &) const
  {
    return CallbackResult::failure("Plugin does not implement lifecycle activation");
  }

  virtual CallbackResult activate_state(HandoverState = {})
  {
    return CallbackResult::failure("Plugin does not implement lifecycle activation");
  }

  virtual DeactivationResult<HandoverState> deactivate_state()
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

  CallbackResult validate_handover_state(const HandoverState & state) const final
  {
    if (state.has_value() && !state.holds<State>()) {
      return CallbackResult::failure("on_activate received an incompatible handover state");
    }
    return {};
  }

  CallbackResult activate_state(HandoverState state = {}) final
  {
    auto validation = validate_handover_state(state);
    if (!validation.success) {return validation;}
    return invoke_lifecycle("on_activate", [&]() {
               std::optional<State> owned;
               if (state.has_value()) {owned.emplace(std::move(state).template take<State>());}
               return on_activate(owned);
    });
  }

  DeactivationResult<HandoverState> deactivate_state() final
  {
    std::optional<HandoverState> snapshot;
    auto result = invoke_lifecycle("on_deactivate", [&]() {
          auto deactivation = on_deactivate();
          if (deactivation.result.success && deactivation.state) {
            snapshot.emplace(std::move(*deactivation.state));
          }
          return deactivation.result;
    });
    if (!result.success) {snapshot.reset();}
    return {std::move(result), std::move(snapshot)};
  }
};

template<>
class LifecyclePlugin<void>: public Plugin
{
public:
  virtual CallbackResult on_activate() {return {};}
  virtual CallbackResult on_deactivate() {return {};}

  CallbackResult validate_handover_state(const HandoverState & state) const final
  {
    if (state.has_value()) {
      return CallbackResult::failure("on_activate accepts no state");
    }
    return {};
  }

  CallbackResult activate_state(HandoverState state = {}) final
  {
    auto validation = validate_handover_state(state);
    if (!validation.success) {return validation;}
    return invoke_lifecycle("on_activate", [&]() {return on_activate();});
  }

  DeactivationResult<HandoverState> deactivate_state() final
  {
    return {invoke_lifecycle("on_deactivate", [&]() {return on_deactivate();}), {}};
  }
};


}  // namespace rpp
