#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "rpp_cpp/context.hpp"
#include "rpp_cpp/plugin_def.hpp"

namespace rpp::testing
{
// In-process lifecycle fakes shared by unit and runtime integration tests.
class Recorder : public rpp::LifecyclePlugin<void>
{
public:
  Recorder(std::string name, std::vector<std::string> & events)
  : name(std::move(name)), events(events) {}

  std::string name;
  std::vector<std::string> & events;
  bool fail_configure = false;
  bool fail_cleanup = false;
  bool throw_configure = false;
  bool throw_cleanup = false;
  bool manage_components = false;
  bool fail_activate = false;
  bool fail_deactivate = false;
  const rpp::ComponentContext * context = nullptr;

  rpp::CallbackResult on_configure(const rpp::ComponentContext & received) override
  {
    context = &received;
    events.push_back("configure " + name);
    if (throw_configure) {throw std::runtime_error("configure exception");}
    return fail_configure ? rpp::CallbackResult::failure("configure rejected") :
           rpp::CallbackResult{};
  }

  rpp::CallbackResult on_cleanup() override
  {
    events.push_back("cleanup " + name);
    if (throw_cleanup) {throw std::runtime_error("cleanup exception");}
    return fail_cleanup ? rpp::CallbackResult::failure("cleanup rejected") : rpp::CallbackResult{};
  }

  rpp::CallbackResult on_activate() override
  {
    if (manage_components) {
      auto result = context->activate_subcomponents();
      if (!result.success) {return result;}
    }
    events.push_back("activate " + name);
    return fail_activate ? rpp::CallbackResult::failure("activation rejected") :
           rpp::CallbackResult{};
  }

  rpp::CallbackResult on_deactivate() override
  {
    events.push_back("deactivate " + name);
    auto result = fail_deactivate ? rpp::CallbackResult::failure(
      "deactivation rejected " + name) : rpp::CallbackResult{};
    if (manage_components) {
      auto children = context->deactivate_subcomponents();
      if (result.success) {result = children;}
    }
    return result;
  }
};

class AxisController : public rpp::LifecyclePlugin<void>
{
public:
  explicit AxisController(std::set<std::string> axes = {})
  : axes(std::move(axes)) {}
  std::set<std::string> axes;
};

struct Estimate {std::vector<double> position{1.0};};

class Localizer : public rpp::LifecyclePlugin<Estimate>
{
public:
  const rpp::ComponentContext * context = nullptr;
  Estimate estimate;
  std::vector<std::string> events;
  std::string reject;
  std::function<void()> before_deactivate;
  std::vector<double> get_position() const {return estimate.position;}
  rpp::CallbackResult on_configure(const rpp::ComponentContext & received) override
  {
    context = &received;
    events.push_back("configure");
    return reject ==
           "configure" ? rpp::CallbackResult::failure("configuration rejected") : rpp::
           CallbackResult{};
  }
  rpp::CallbackResult on_activate(const std::optional<Estimate> & state) override
  {
    events.push_back("activate");
    if (state) {estimate = *state;}
    return reject ==
           "activate" ? rpp::CallbackResult::failure("activation rejected") : rpp::CallbackResult{};
  }
  rpp::DeactivationResult<Estimate> on_deactivate() override
  {
    if (before_deactivate) {before_deactivate();}
    events.push_back("deactivate");
    return {reject ==
      "deactivate" ? rpp::CallbackResult::failure("still active") : rpp::CallbackResult{},
      estimate};
  }
  rpp::CallbackResult on_cleanup() override {events.push_back("cleanup"); return {};}
};

struct EventLog
{
  std::mutex mutex;
  std::vector<std::string> events;

  void add(std::string event)
  {
    std::lock_guard<std::mutex> lock(mutex);
    events.push_back(std::move(event));
  }

  std::vector<std::string> snapshot()
  {
    std::lock_guard<std::mutex> lock(mutex);
    return events;
  }

  void clear()
  {
    std::lock_guard<std::mutex> lock(mutex);
    events.clear();
  }
};

// Loaded from a real shared library by script-builder integration tests.
class ScriptBuilderPlugin : public rpp::LifecyclePlugin<void>
{
public:
  int value = 0;
  int configure_calls = 0;
  int activate_calls = 0;
  int deactivate_calls = 0;
  int cleanup_calls = 0;

  rpp::CallbackResult on_configure(const rpp::ComponentContext & context) override
  {
    ++configure_calls;
    value = context.get_parameter<int>("value");
    return {};
  }

  rpp::CallbackResult on_activate() override {++activate_calls; return {};}
  rpp::CallbackResult on_deactivate() override {++deactivate_calls; return {};}
  rpp::CallbackResult on_cleanup() override {++cleanup_calls; return {};}
};

struct ControllerState {int value = 0;};

class ScenarioController : public rpp::LifecyclePlugin<ControllerState>
{
public:
  ScenarioController(std::string name, EventLog & log) : name(std::move(name)), log(log) {}
  std::string name;
  EventLog & log;
  ControllerState state;
  bool reject_activation = false;
  bool reject_deactivation = false;
  const rpp::ComponentContext * context = nullptr;

  rpp::CallbackResult on_configure(const rpp::ComponentContext & component_context) override
  {
    context = &component_context;
    log.add("configure " + name);
    return {};
  }

  rpp::CallbackResult on_activate(const std::optional<ControllerState> & previous) override
  {
    log.add("activate " + name);
    if (reject_activation) {return rpp::CallbackResult::failure("activation rejected");}
    state = previous.value_or(ControllerState{});
    return {};
  }

  rpp::DeactivationResult<ControllerState> on_deactivate() override
  {
    log.add("deactivate " + name);
    return {reject_deactivation ? rpp::CallbackResult::failure("deactivation rejected") :
      rpp::CallbackResult{}, state};
  }

  rpp::CallbackResult on_cleanup() override
  {
    log.add("cleanup " + name);
    return {};
  }
};

// A composite explicitly opts into managing its children. Activation runs
// children first; deactivation logs/stops the parent before its children.
class CompositeController : public ScenarioController
{
public:
  using ScenarioController::ScenarioController;

  rpp::CallbackResult on_activate(const std::optional<ControllerState> & previous) override
  {
    auto children = context->activate_subcomponents();
    if (!children.success) {return children;}
    return ScenarioController::on_activate(previous);
  }

  rpp::DeactivationResult<ControllerState> on_deactivate() override
  {
    auto result = ScenarioController::on_deactivate();
    if (!result.result.success) {return result;}
    auto children = context->deactivate_subcomponents();
    if (!children.success) {return {children, std::nullopt};}
    return result;
  }
};

struct SnapshotEstimate {std::vector<double> position;};

class SnapshotLocalizer : public rpp::LifecyclePlugin<SnapshotEstimate>
{
public:
  SnapshotEstimate estimate;
  rpp::CallbackResult on_activate(const std::optional<SnapshotEstimate> & state) override
  {
    if (state) {estimate = *state;}
    return {};
  }
  rpp::DeactivationResult<SnapshotEstimate> on_deactivate() override {return {{}, estimate};}
};

class AlternativeLocalizer : public SnapshotLocalizer {};

class GuardedLocalizer : public SnapshotLocalizer
{
public:
  bool called = false;
  rpp::CallbackResult on_activate(const std::optional<SnapshotEstimate> &) override
  {
    called = true;
    return {};
  }
};

class FailedDeactivationLocalizer : public SnapshotLocalizer
{
  rpp::DeactivationResult<SnapshotEstimate> on_deactivate() override
  {return {rpp::CallbackResult::failure("still updating"), SnapshotEstimate{{1.0}}};}
};

class Stateless : public rpp::LifecyclePlugin<void>
{
public:
  int activated = 0;
  int deactivated = 0;
  rpp::CallbackResult on_activate() override {++activated; return {};}
  rpp::CallbackResult on_deactivate() override {++deactivated; return {};}
};

class FailingLifecycleLocalizer : public SnapshotLocalizer
{
  rpp::CallbackResult on_activate(const std::optional<SnapshotEstimate> &) override
  {return rpp::CallbackResult::failure("state rejected");}
  rpp::DeactivationResult<SnapshotEstimate> on_deactivate() override
  {throw std::runtime_error("still updating");}
};

class HandoverPlugin : public rpp::LifecyclePlugin<SnapshotEstimate>
{
public:
  std::optional<SnapshotEstimate> estimate;
  rpp::CallbackResult on_activate(const std::optional<SnapshotEstimate> & state) override
  {
    estimate = state;
    return {};
  }
  rpp::DeactivationResult<SnapshotEstimate> on_deactivate() override {return {{}, estimate};}
};
}  // namespace rpp::testing
