#include "support/runtime_test_helpers.hpp"
#include <gtest/gtest.h>
#include <future>
#include <vector>

#include "rpp_cpp/slot_manager.hpp"
#include "rpp_cpp/runtime.hpp"
#include "rpp_cpp/context.hpp"

namespace
{
using rpp::testing::Estimate;
using rpp::testing::Localizer;

template<class Threading>
class BasicManagerTest : public ::testing::Test
{
public:
  std::vector<std::shared_ptr<Localizer>> plugins;
  std::shared_ptr<rpp::BasicSlotManager<Threading>> manager_owner =
    std::make_shared<rpp::BasicSlotManager<Threading>>([this](const std::string &) {
      auto plugin = std::make_shared<Localizer>();
      plugins.push_back(plugin);
      return rpp::ComponentRuntimeAccess::plugin<Threading>(plugin);
    }, rpp::ActivationPolicy::MULTIPLE, true);
  rpp::BasicSlotManager<Threading> & manager = *manager_owner;

  void ready(const std::string & name = "localizer")
  {
    ASSERT_TRUE(manager.load(name, "/workspace/localizer").success);
    ASSERT_TRUE(manager.configure(name).success);
    ASSERT_TRUE(manager.activate(name).success);
  }
};
using ManagerTest = BasicManagerTest<rpp::SingleThreaded>;
using ConcurrentManagerTest = BasicManagerTest<rpp::Concurrent>;
}  // namespace

TEST(SlotManagerRecovery, FailedLoadCleanupPreservesErrorAndAllowsUnloadRetry)
{
  bool fail_load = true;
  auto manager_owner = std::make_shared<rpp::SlotManager>([&](const std::string &) {
      if (fail_load) {throw std::runtime_error("load rejected");}
      return rpp::ComponentRuntimeAccess::plugin(std::make_shared<Localizer>());
    }, rpp::ActivationPolicy::MULTIPLE, true);
  auto & manager = *manager_owner;
  EXPECT_FALSE(manager.load("localizer", "/workspace/localizer").success);
  auto result = manager.cleanup("localizer");
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.message.find("No loaded context"), std::string::npos);
  EXPECT_EQ(manager.get_state("localizer"), rpp::LifecycleState::ERROR);
  EXPECT_FALSE(manager.get_status("localizer").transition.has_value());
  ASSERT_TRUE(manager.unload("localizer").success);
  fail_load = false;
  ASSERT_TRUE(manager.load("localizer").success);
  EXPECT_TRUE(manager.configure("localizer").success);
}

TEST_F(ManagerTest, NormalFlowIdempotenceAndInstanceIdentity)
{
  ready();
  auto instance_id = manager.get_status("localizer").instance_id;
  ASSERT_FALSE(instance_id.empty());
  EXPECT_EQ(manager.runtime("localizer")->get_context().get_instance_id(), instance_id);
  ASSERT_TRUE(manager.activate("localizer").success);
  EXPECT_EQ(plugins[0]->events, (std::vector<std::string>{"configure", "activate"}));
  EXPECT_EQ(
    manager.with_active<Localizer>(
      "localizer", [](auto & plugin) {
        return plugin.estimate.position;
      }), (std::vector<double>{1.0}));
  auto stopped = manager.deactivate("localizer");
  ASSERT_TRUE(stopped.result.success);
  ASSERT_TRUE(stopped.state.has_value());
  EXPECT_EQ(stopped.state->get<Estimate>().position, (std::vector<double>{1.0}));
  EXPECT_FALSE(manager.deactivate("localizer").state.has_value());
  ASSERT_TRUE(manager.cleanup("localizer").success);
  EXPECT_EQ(manager.get_status("localizer").instance_id, instance_id);
  ASSERT_TRUE(manager.unload("localizer").success);
  EXPECT_TRUE(manager.get_status("localizer").instance_id.empty());
  EXPECT_EQ(manager.get_state("localizer"), rpp::LifecycleState::UNLOADED);
  ready();
  EXPECT_NE(manager.get_status("localizer").instance_id, instance_id);
  EXPECT_THROW(
    manager.with_active<Localizer>(
      "localizer", [](
        auto &) {}, instance_id), rpp::LifecycleError);
}

TEST_F(ManagerTest, SameComponentPathHasDistinctInstancesAndUniqueRegistrationNames)
{
  ASSERT_TRUE(manager.load("first", "/workspace/localizer").success);
  ASSERT_TRUE(manager.load("second", "/workspace/localizer").success);
  const auto first = manager.get_status("first").instance_id;
  const auto second = manager.get_status("second").instance_id;
  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());
  EXPECT_NE(first, second);
  EXPECT_FALSE(manager.load("first", "/workspace/localizer").success);
  EXPECT_EQ(manager.get_status("first").instance_id, first);
  EXPECT_TRUE(manager.close().success);
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

TEST_F(ConcurrentManagerTest, DirectPointersRemainCallableWhileInactive)
{
  ready();
  auto instance = manager.runtime("localizer")->get_instance<Localizer>();
  ASSERT_TRUE(manager.deactivate("localizer").result.success);
  EXPECT_EQ(instance->get_position(), (std::vector<double>{1.0}));
  EXPECT_EQ(manager.runtime("localizer")->get_instance<Localizer>(), instance);
  EXPECT_THROW(manager.with_active<Localizer>("localizer", [](auto &) {}), rpp::LifecycleError);
  EXPECT_TRUE(manager.close().success);
}

TEST_F(ConcurrentManagerTest, LifecycleCallbacksDoNotHoldTheSlotGate)
{
  ready();
  std::promise<void> callback_entered, queried;
  auto entered = callback_entered.get_future();
  auto completed = queried.get_future();
  auto reader = std::async(std::launch::async, [&] {
    if (entered.wait_for(std::chrono::milliseconds(500)) != std::future_status::ready) {return;}
    const auto info = manager.get_status("localizer");
    EXPECT_TRUE(info.transition.has_value());
    EXPECT_FALSE(info.callable);
    EXPECT_NE(manager.runtime("localizer"), nullptr);
    queried.set_value();
  });
  plugins[0]->before_deactivate = [&] {
    callback_entered.set_value();
    EXPECT_EQ(completed.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
  };
  const auto stopped = manager.deactivate("localizer");
  // Finish the reader even when the bounded callback observation fails.
  reader.get();
  EXPECT_TRUE(stopped.result.success);
  EXPECT_TRUE(manager.close().success);
}

TEST_F(ConcurrentManagerTest, ContextDelegatesToTheConcurrentManager)
{
  ready();
  const auto retained_view = manager.subcomponent_context("localizer");
  const auto & context = *retained_view;
  EXPECT_TRUE(context.is_active());
  ASSERT_TRUE(context.deactivate().result.success);
  EXPECT_EQ(manager.get_state("localizer"), rpp::LifecycleState::INACTIVE);
  ASSERT_TRUE(context.activate().success);
  EXPECT_TRUE(context.is_active());
  EXPECT_TRUE(manager.close().success);
}

TEST_F(ConcurrentManagerTest, SelfDeactivationDrainsConcurrentCallsAndRetriesAfterTimeout)
{
  ready();
  auto runtime = manager.runtime("localizer");
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  auto caller = std::async(std::launch::async, [&] {
    manager.with_active<Localizer>("localizer", [&](auto & plugin) {
      plugin.context->request_deactivation().require_success();
      entered.set_value();
      released.wait();
    });
  });
  const auto observed = entered.get_future().wait_for(std::chrono::milliseconds(500));
  if (observed != std::future_status::ready) {release.set_value();}
  ASSERT_EQ(observed, std::future_status::ready);
  EXPECT_FALSE(runtime->process_deactivations(std::chrono::milliseconds(0)).success);
  EXPECT_EQ(plugins[0]->events, (std::vector<std::string>{"configure", "activate"}));
  release.set_value();
  caller.get();
  ASSERT_TRUE(runtime->process_deactivations().success);
  EXPECT_EQ(manager.get_state("localizer"), rpp::LifecycleState::INACTIVE);
  EXPECT_TRUE(manager.close().success);
}

TEST_F(ConcurrentManagerTest, SlotGateCountsAllConcurrentCalls)
{
  ready();
  std::promise<void> first_entered, second_entered, release_first, release_second;
  auto first_release = release_first.get_future().share();
  auto second_release = release_second.get_future().share();
  auto first = std::async(std::launch::async, [&] {
    manager.with_active<Localizer>("localizer", [&](auto &) {
      first_entered.set_value();
      first_release.wait();
    });
  });
  auto second = std::async(std::launch::async, [&] {
    manager.with_active<Localizer>("localizer", [&](auto &) {
      second_entered.set_value();
      second_release.wait();
    });
  });
  const auto first_ready = first_entered.get_future().wait_for(std::chrono::milliseconds(500));
  const auto second_ready = second_entered.get_future().wait_for(std::chrono::milliseconds(500));
  if (first_ready != std::future_status::ready || second_ready != std::future_status::ready) {
    release_first.set_value();
    release_second.set_value();
    FAIL() << "Guarded calls did not enter";
  }
  EXPECT_EQ(manager.get_status("localizer").in_flight, 2u);
  release_first.set_value();
  first.get();
  EXPECT_EQ(manager.get_status("localizer").in_flight, 1u);
  EXPECT_FALSE(manager.deactivate("localizer", std::chrono::milliseconds(0)).result.success);
  release_second.set_value();
  second.get();
  EXPECT_EQ(manager.get_status("localizer").in_flight, 0u);
  EXPECT_TRUE(manager.deactivate("localizer").result.success);
  EXPECT_TRUE(manager.close().success);
}

TEST_F(ConcurrentManagerTest, DrainTimeoutRetainsInstanceAndAllowsRetry)
{
  ready();
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  auto caller = std::async(
    std::launch::async, [&] {
      manager.with_active<Localizer>(
        "localizer", [&](auto &) {
          entered.set_value();
          released.wait();
        });
    });
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
  ASSERT_TRUE(manager.switch_components("ekf", "particle", true).success);
  plugins[0]->estimate.position.push_back(99.0);
  EXPECT_EQ(
    manager.with_active<Localizer>(
      "particle", [](auto & plugin) {
        return plugin.estimate.position;
      }), (std::vector<double>{1.0}));
  plugins[0]->reject = "activate";
  EXPECT_FALSE(manager.switch_components("particle", "ekf", true).success);
  EXPECT_EQ(manager.get_state("particle"), rpp::LifecycleState::INACTIVE);
  EXPECT_EQ(manager.get_state("ekf"), rpp::LifecycleState::ERROR);
}

TEST_F(ManagerTest, ReloadConstructsNewContextAndOptionallyTransfersState)
{
  ready();
  plugins[0]->estimate.position = {7.0};
  auto instance_id = manager.get_status("localizer").instance_id;
  ASSERT_TRUE(manager.reload("localizer", true).success);
  EXPECT_NE(manager.get_status("localizer").instance_id, instance_id);
  EXPECT_EQ(plugins.size(), 2u);
  EXPECT_EQ(
    manager.with_active<Localizer>(
      "localizer", [](auto & plugin) {
        return plugin.estimate.position;
      }), (std::vector<double>{7.0}));
  ASSERT_TRUE(manager.reload("localizer").success);
  EXPECT_EQ(plugins.back()->estimate.position, (std::vector<double>{1.0}));
}

TEST_F(ManagerTest, ReentrantManagementFromGuardIsRejected)
{
  ready();
  auto result = manager.with_active<Localizer>(
    "localizer", [&](auto &) {
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
