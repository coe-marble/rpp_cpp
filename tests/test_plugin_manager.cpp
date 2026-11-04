#include <gtest/gtest.h>
#include <future>
#include <thread>
#include <vector>

#include "rpp_cpp/plugin_manager.hpp"

namespace rpp
{
class ComponentContextAccess
{
public:
  static ComponentContext make(std::shared_ptr<Plugin> plugin)
  {
    return ComponentContext(std::move(plugin), params::Parameters{}, nullptr,
      ComponentContext::SubcomponentMap{}, ClockOptions{});
  }
};
}  // namespace rpp

namespace
{
struct Estimate {std::vector<double> position{1.0};};

class Localizer : public rpp::LifecyclePlugin<Estimate>
{
public:
  Estimate estimate;
  std::vector<std::string> events;
  std::string reject;
  rpp::CallbackResult on_configure(const rpp::ComponentContext &) override
  {
    events.push_back("configure");
    return reject == "configure" ? rpp::CallbackResult::failure("configuration rejected") : rpp::CallbackResult{};
  }
  rpp::CallbackResult on_activate(const std::optional<Estimate> & state) override
  {
    events.push_back("activate");
    if (state) {estimate = *state;}
    return reject == "activate" ? rpp::CallbackResult::failure("activation rejected") : rpp::CallbackResult{};
  }
  rpp::DeactivationResult<Estimate> on_deactivate() override
  {
    events.push_back("deactivate");
    return {reject == "deactivate" ? rpp::CallbackResult::failure("still active") : rpp::CallbackResult{}, estimate};
  }
  rpp::CallbackResult on_cleanup() override {events.push_back("cleanup"); return {};}
};

class ManagerTest : public ::testing::Test
{
public:
  std::vector<std::shared_ptr<Localizer>> plugins;
  rpp::PluginManager manager{[this](const std::string &) {
      auto plugin = std::make_shared<Localizer>();
      plugins.push_back(plugin);
      return rpp::ComponentContextAccess::make(plugin);
    }};

  void ready(const std::string & name = "localizer")
  {
    ASSERT_TRUE(manager.load(name, "/workspace/localizer").success);
    ASSERT_TRUE(manager.configure(name).success);
    ASSERT_TRUE(manager.activate(name).success);
  }
};
}  // namespace

TEST_F(ManagerTest, NormalFlowIdempotenceAndGeneration)
{
  ready();
  auto generation = manager.get_info("localizer").generation;
  ASSERT_TRUE(manager.activate("localizer").success);
  EXPECT_EQ(plugins[0]->events, (std::vector<std::string>{"configure", "activate"}));
  EXPECT_EQ(manager.with_active<Localizer>("localizer", [](auto & plugin) {
      return plugin.estimate.position;
    }), (std::vector<double>{1.0}));
  auto stopped = manager.deactivate("localizer");
  ASSERT_TRUE(stopped.result.success);
  ASSERT_TRUE(stopped.state.has_value());
  EXPECT_EQ(std::any_cast<Estimate>(*stopped.state).position, (std::vector<double>{1.0}));
  EXPECT_FALSE(manager.deactivate("localizer").state.has_value());
  ASSERT_TRUE(manager.cleanup("localizer").success);
  ASSERT_TRUE(manager.unload("localizer").success);
  EXPECT_EQ(manager.get_state("localizer"), rpp::LifecycleState::UNLOADED);
  ready();
  EXPECT_GT(manager.get_info("localizer").generation, generation);
  EXPECT_THROW(manager.with_active<Localizer>("localizer", [](auto &) {}, generation), rpp::LifecycleError);
}

TEST_F(ManagerTest, InvalidTransitionLeavesStateAndBlocksDomainCalls)
{
  ASSERT_TRUE(manager.load("localizer", "/workspace/localizer").success);
  EXPECT_FALSE(manager.activate("localizer").success);
  EXPECT_EQ(manager.get_state("localizer"), rpp::LifecycleState::UNCONFIGURED);
  EXPECT_TRUE(plugins[0]->events.empty());
  EXPECT_THROW(manager.with_active<Localizer>("localizer", [](auto &) {}), rpp::LifecycleError);
}

TEST_F(ManagerTest, FailedConfigurationRetainsTruthfulStateAndDoesNotAffectSibling)
{
  ready("other");
  ASSERT_TRUE(manager.load("failed", "/workspace/failed").success);
  plugins.back()->reject = "configure";
  EXPECT_FALSE(manager.configure("failed").success);
  EXPECT_EQ(manager.get_state("failed"), rpp::LifecycleState::ERROR);
  EXPECT_FALSE(manager.deactivate("failed").result.success);
  EXPECT_EQ(manager.get_state("other"), rpp::LifecycleState::ACTIVE);
  EXPECT_TRUE(manager.unload("failed").success);
}

TEST_F(ManagerTest, DrainTimeoutRetainsInstanceAndAllowsRetry)
{
  ready();
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  auto caller = std::async(std::launch::async, [&] {manager.with_active<Localizer>("localizer", [&](auto &) {
      entered.set_value();
      released.wait();
    });});
  auto entered_state = entered.get_future();
  const auto observed = entered_state.wait_for(std::chrono::milliseconds(500));
  if (observed != std::future_status::ready) {release.set_value();}
  ASSERT_EQ(observed, std::future_status::ready);
  auto result = manager.unload("localizer", std::chrono::milliseconds(0));
  EXPECT_FALSE(result.success);
  EXPECT_EQ(manager.get_state("localizer"), rpp::LifecycleState::ERROR);
  EXPECT_EQ(plugins[0]->events, (std::vector<std::string>{"configure", "activate"}));
  release.set_value();
  caller.get();
  EXPECT_TRUE(manager.unload("localizer").success);
}

TEST_F(ManagerTest, SwitchingCopiesStateAndDoesNotReactivateOnTargetFailure)
{
  ready("ekf");
  ASSERT_TRUE(manager.load("particle", "/workspace/particle").success);
  ASSERT_TRUE(manager.switch_plugins("ekf", "particle", true).success);
  plugins[0]->estimate.position.push_back(99.0);
  EXPECT_EQ(manager.with_active<Localizer>("particle", [](auto & plugin) {
      return plugin.estimate.position;
    }), (std::vector<double>{1.0}));
  plugins[0]->reject = "activate";
  EXPECT_FALSE(manager.switch_plugins("particle", "ekf", true).success);
  EXPECT_EQ(manager.get_state("particle"), rpp::LifecycleState::INACTIVE);
  EXPECT_EQ(manager.get_state("ekf"), rpp::LifecycleState::ERROR);
}

TEST_F(ManagerTest, ReloadConstructsNewContextAndOptionallyTransfersState)
{
  ready();
  plugins[0]->estimate.position = {7.0};
  auto generation = manager.get_info("localizer").generation;
  ASSERT_TRUE(manager.reload("localizer", true).success);
  EXPECT_GT(manager.get_info("localizer").generation, generation);
  EXPECT_EQ(plugins.size(), 2u);
  EXPECT_EQ(manager.with_active<Localizer>("localizer", [](auto & plugin) {
      return plugin.estimate.position;
    }), (std::vector<double>{7.0}));
  ASSERT_TRUE(manager.reload("localizer").success);
  EXPECT_EQ(plugins.back()->estimate.position, (std::vector<double>{1.0}));
}

TEST_F(ManagerTest, ReentrantManagementFromGuardIsRejected)
{
  ready();
  auto result = manager.with_active<Localizer>("localizer", [&](auto &) {
      return manager.deactivate("localizer");
    });
  EXPECT_FALSE(result.result.success);
  EXPECT_EQ(manager.get_state("localizer"), rpp::LifecycleState::ACTIVE);
}

TEST_F(ManagerTest, ShutdownContinuesOtherEntries)
{
  ASSERT_TRUE(manager.load("first", "/workspace/first").success);
  ASSERT_TRUE(manager.configure("first").success);
  ASSERT_TRUE(manager.activate("first").success);
  ready("second");
  plugins[1]->reject = "deactivate";
  EXPECT_FALSE(manager.close().success);
  EXPECT_EQ(manager.get_state("first"), rpp::LifecycleState::UNLOADED);
  EXPECT_EQ(manager.get_state("second"), rpp::LifecycleState::ERROR);
  plugins[1]->reject.clear();
  EXPECT_TRUE(manager.close().success);
  EXPECT_FALSE(manager.load("new", "/workspace/new").success);
}
