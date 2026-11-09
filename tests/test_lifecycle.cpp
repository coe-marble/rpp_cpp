#include "support/fake_plugins.hpp"
#include <any>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "rpp_cpp/plugin_def.hpp"

namespace
{
using Estimate = rpp::testing::SnapshotEstimate;
using Localizer = rpp::testing::SnapshotLocalizer;
}  // namespace

TEST(PluginLifecycle, SnapshotSurvivesSourceAndInputMutation)
{
  std::any snapshot;
  {
    Localizer source;
    source.estimate.position = {1.0, 2.0};
    auto result = source.deactivate_state();
    ASSERT_TRUE(result.result.success);
    snapshot = *result.state;
    source.estimate.position.push_back(99.0);
  }
  rpp::testing::AlternativeLocalizer target;
  ASSERT_TRUE(target.activate_state(snapshot).success);
  std::any_cast<Estimate &>(snapshot).position.push_back(100.0);
  EXPECT_EQ(target.estimate.position, (std::vector<double>{1.0, 2.0}));
}

TEST(PluginLifecycle, ColdActivationDoesNotSupplyDefaultState)
{
  Localizer plugin;
  plugin.estimate.position = {3.0};
  ASSERT_TRUE(plugin.activate_state().success);
  EXPECT_EQ(plugin.estimate.position, (std::vector<double>{3.0}));
  ASSERT_TRUE(plugin.activate_state(Estimate{}).success);
  EXPECT_TRUE(plugin.estimate.position.empty());
}

TEST(PluginLifecycle, RejectsWrongStateType)
{
  rpp::testing::GuardedLocalizer plugin;
  EXPECT_FALSE(plugin.activate_state(std::string("wrong type")).success);
  EXPECT_FALSE(plugin.called);
}

TEST(PluginLifecycle, FailedDeactivationDoesNotExportSnapshot)
{
  rpp::testing::FailedDeactivationLocalizer plugin;
  auto result = plugin.deactivate_state();
  EXPECT_FALSE(result.result.success);
  EXPECT_FALSE(result.state.has_value());
}

TEST(PluginLifecycle, StatelessCallbacksAndPayloadRejection)
{
  rpp::testing::Stateless plugin;
  rpp::Plugin & base = plugin;
  EXPECT_FALSE(base.activate_state(Estimate{}).success);
  EXPECT_EQ(plugin.activated, 0);
  EXPECT_TRUE(base.activate_state().success);
  auto result = base.deactivate_state();
  EXPECT_TRUE(result.result.success);
  EXPECT_FALSE(result.state.has_value());
  EXPECT_EQ(plugin.activated, 1);
  EXPECT_EQ(plugin.deactivated, 1);
}

TEST(PluginLifecycle, CallbackFailuresAndExceptionsAreReported)
{
  rpp::testing::FailingLifecycleLocalizer plugin;
  auto activation = plugin.activate_state();
  EXPECT_FALSE(activation.success);
  EXPECT_NE(activation.message.find("state rejected"), std::string::npos);
  auto deactivation = plugin.deactivate_state();
  EXPECT_FALSE(deactivation.result.success);
  EXPECT_NE(deactivation.result.message.find("on_deactivate"), std::string::npos);
  EXPECT_NE(deactivation.result.message.find("still updating"), std::string::npos);
  EXPECT_FALSE(deactivation.state.has_value());
}
