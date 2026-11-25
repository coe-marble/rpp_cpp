#include "support/fake_plugins.hpp"
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rpp_cpp/plugin_def.hpp"

namespace
{
using Estimate = rpp::testing::SnapshotEstimate;
using Localizer = rpp::testing::SnapshotLocalizer;

struct MoveOnlyEstimate
{
  explicit MoveOnlyEstimate(int value)
  : value(std::make_unique<int>(value)) {}
  MoveOnlyEstimate(MoveOnlyEstimate &&) noexcept = default;
  MoveOnlyEstimate & operator=(MoveOnlyEstimate &&) noexcept = default;
  MoveOnlyEstimate(const MoveOnlyEstimate &) = delete;
  MoveOnlyEstimate & operator=(const MoveOnlyEstimate &) = delete;
  std::unique_ptr<int> value;
};

class MoveOnlyLocalizer final : public rpp::LifecyclePlugin<MoveOnlyEstimate>
{
public:
  explicit MoveOnlyLocalizer(int value)
  : estimate(value) {}
  MoveOnlyEstimate estimate;

  rpp::CallbackResult on_activate(const std::optional<MoveOnlyEstimate> & state) override
  {
    if (state) {received = *state->value;}
    return {};
  }

  rpp::CallbackResult on_deactivate() override
  {return {};}

  int received = 0;
};
}  // namespace

TEST(PluginLifecycle, SnapshotSurvivesSourceAndInputMutation)
{
  rpp::HandoverState snapshot;
  {
    Localizer source;
    source.estimate.position = {1.0, 2.0};
    snapshot = Estimate{source.estimate};
    ASSERT_TRUE(source.deactivate_state().success);
    source.estimate.position.push_back(99.0);
  }
  rpp::testing::AlternativeLocalizer target;
  ASSERT_TRUE(target.activate_state(std::move(snapshot)).success);
  EXPECT_EQ(target.estimate.position, (std::vector<double>{1.0, 2.0}));
}

TEST(PluginLifecycle, TransfersMoveOnlyStateWithoutCopying)
{
  MoveOnlyLocalizer source(17);
  auto stopped = source.deactivate_state();
  ASSERT_TRUE(stopped.success);
  MoveOnlyLocalizer target(0);
  ASSERT_TRUE(target.activate_state(std::move(source.estimate)).success);
  EXPECT_EQ(target.received, 17);
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

TEST(PluginLifecycle, FailedDeactivationReportsFailure)
{
  rpp::testing::FailedDeactivationLocalizer plugin;
  auto result = plugin.deactivate_state();
  EXPECT_FALSE(result.success);
}

TEST(PluginLifecycle, StatelessCallbacksAndPayloadRejection)
{
  rpp::testing::Stateless plugin;
  rpp::Plugin & base = plugin;
  EXPECT_FALSE(base.activate_state(Estimate{}).success);
  EXPECT_EQ(plugin.activated, 0);
  EXPECT_TRUE(base.activate_state().success);
  auto result = base.deactivate_state();
  EXPECT_TRUE(result.success);
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
  EXPECT_FALSE(deactivation.success);
  EXPECT_NE(deactivation.message.find("on_deactivate"), std::string::npos);
  EXPECT_NE(deactivation.message.find("still updating"), std::string::npos);
}
