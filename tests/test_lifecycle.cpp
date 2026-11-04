#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "rpp_cpp/plugin_def.hpp"

namespace
{
struct Estimate
{
  std::vector<double> position;
};

class Localizer : public rpp::LifecyclePlugin<Estimate>
{
public:
  Estimate estimate;

  rpp::CallbackResult on_activate(const std::optional<Estimate> & state) override
  {
    if (state) {
      estimate = *state;
    }
    return {};
  }

  rpp::DeactivationResult<Estimate> on_deactivate() override
  {
    return {{}, estimate};
  }
};
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
  class AlternativeLocalizer : public Localizer {} target;
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
  class Guarded : public Localizer
  {
public:
    bool called = false;
    rpp::CallbackResult on_activate(const std::optional<Estimate> &) override
    {
      called = true;
      return {};
    }
  } plugin;
  EXPECT_FALSE(plugin.activate_state(std::string("wrong type")).success);
  EXPECT_FALSE(plugin.called);
}

TEST(PluginLifecycle, FailedDeactivationDoesNotExportSnapshot)
{
  class Failed : public Localizer
  {
    rpp::DeactivationResult<Estimate> on_deactivate() override
    {
      return {rpp::CallbackResult::failure("still updating"), Estimate{{1.0}}};
    }
  } plugin;
  auto result = plugin.deactivate_state();
  EXPECT_FALSE(result.result.success);
  EXPECT_FALSE(result.state.has_value());
}

TEST(PluginLifecycle, StatelessCallbacksAndPayloadRejection)
{
  class Stateless : public rpp::LifecyclePlugin<void>
  {
public:
    int activated = 0;
    int deactivated = 0;
    rpp::CallbackResult on_activate() override {++activated; return {};}
    rpp::CallbackResult on_deactivate() override {++deactivated; return {};}
  } plugin;
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
  class Failed : public Localizer
  {
    rpp::CallbackResult on_activate(const std::optional<Estimate> &) override
    {
      return rpp::CallbackResult::failure("state rejected");
    }
    rpp::DeactivationResult<Estimate> on_deactivate() override
    {
      throw std::runtime_error("still updating");
    }
  } plugin;
  auto activation = plugin.activate_state();
  EXPECT_FALSE(activation.success);
  EXPECT_NE(activation.message.find("state rejected"), std::string::npos);
  auto deactivation = plugin.deactivate_state();
  EXPECT_FALSE(deactivation.result.success);
  EXPECT_NE(deactivation.result.message.find("on_deactivate"), std::string::npos);
  EXPECT_NE(deactivation.result.message.find("still updating"), std::string::npos);
  EXPECT_FALSE(deactivation.state.has_value());
}
