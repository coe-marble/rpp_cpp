#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <vector>

#include "rpp_cpp/context.hpp"

namespace rpp
{
class ComponentContextAccess
{
public:
  static ComponentContext make(
    std::shared_ptr<Plugin> plugin,
    std::vector<ComponentContext> children = {})
  {
    ComponentContext::SubcomponentMap slots;
    slots.emplace("children", std::move(children));
    return ComponentContext(std::move(plugin), params::Parameters{}, nullptr,
      std::move(slots), ClockOptions{});
  }
};
}  // namespace rpp

namespace
{
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
    events.push_back("activate " + name);
    return fail_activate ? rpp::CallbackResult::failure("activation rejected") :
           rpp::CallbackResult{};
  }

  rpp::CallbackResult on_deactivate() override
  {
    events.push_back("deactivate " + name);
    return fail_deactivate ? rpp::CallbackResult::failure("deactivation rejected " +
        name) : rpp::CallbackResult{};
  }
};
}  // namespace

TEST(ContextLifecycle, ConfiguresChildrenFirstAndCleansParentFirst)
{
  std::vector<std::string> events;
  auto parent = std::make_shared<Recorder>("parent", events);
  std::vector<rpp::ComponentContext> children;
  children.push_back(rpp::ComponentContextAccess::make(std::make_shared<Recorder>("first",
    events)));
  children.push_back(rpp::ComponentContextAccess::make(std::make_shared<Recorder>("second",
    events)));
  auto context = rpp::ComponentContextAccess::make(parent, std::move(children));
  ASSERT_TRUE(context.configure().success);
  EXPECT_EQ(parent->context, &context);
  ASSERT_TRUE(context.cleanup().success);
  EXPECT_EQ(events, (std::vector<std::string>{"configure first", "configure second",
      "configure parent", "cleanup parent", "cleanup second", "cleanup first"}));
}

TEST(ContextLifecycle, ConfigurationFailureStopsTraversal)
{
  for (const bool throws : {false, true}) {
    std::vector<std::string> events;
    auto failed = std::make_shared<Recorder>("failed", events);
    failed->fail_configure = !throws;
    failed->throw_configure = throws;
    std::vector<rpp::ComponentContext> children;
    children.push_back(rpp::ComponentContextAccess::make(failed));
    children.push_back(rpp::ComponentContextAccess::make(std::make_shared<Recorder>("unreached",
      events)));
    auto context = rpp::ComponentContextAccess::make(
      std::make_shared<Recorder>("parent", events), std::move(children));
    auto result = context.configure();
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.message.find(throws ? "configure exception" : "configure rejected"),
      std::string::npos);
    EXPECT_EQ(events, (std::vector<std::string>{"configure failed"}));
  }
}

TEST(ContextLifecycle, CleanupContinuesAndPreservesFirstFailure)
{
  std::vector<std::string> events;
  auto parent = std::make_shared<Recorder>("parent", events);
  parent->fail_cleanup = true;
  auto second = std::make_shared<Recorder>("second", events);
  second->throw_cleanup = true;
  std::vector<rpp::ComponentContext> children;
  children.push_back(rpp::ComponentContextAccess::make(std::make_shared<Recorder>("first",
    events)));
  children.push_back(rpp::ComponentContextAccess::make(second));
  auto context = rpp::ComponentContextAccess::make(parent, std::move(children));
  auto result = context.cleanup();
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "cleanup rejected");
  EXPECT_EQ(events,
    (std::vector<std::string>{"cleanup parent", "cleanup second", "cleanup first"}));
}

TEST(ContextLifecycle, EmptyContextSucceeds)
{
  auto context = rpp::ComponentContextAccess::make(nullptr);
  EXPECT_TRUE(context.configure().success);
  EXPECT_TRUE(context.cleanup().success);
  EXPECT_TRUE(context.activate().success);
  EXPECT_TRUE(context.deactivate().result.success);
}

TEST(ContextLifecycle, ActivatesChildrenFirstAndDeactivatesParentFirst)
{
  std::vector<std::string> events;
  std::vector<rpp::ComponentContext> children;
  children.push_back(rpp::ComponentContextAccess::make(std::make_shared<Recorder>("first",
    events)));
  children.push_back(rpp::ComponentContextAccess::make(std::make_shared<Recorder>("second",
    events)));
  auto context = rpp::ComponentContextAccess::make(
    std::make_shared<Recorder>("parent", events), std::move(children));
  ASSERT_TRUE(context.activate().success);
  ASSERT_TRUE(context.deactivate().result.success);
  EXPECT_EQ(events,
    (std::vector<std::string>{"activate first", "activate second", "activate parent",
      "deactivate parent", "deactivate second", "deactivate first"}));
}

TEST(ContextLifecycle, FailedActivationRollsBackOnlyCompletedChildren)
{
  for (const bool fail_parent : {false, true}) {
    std::vector<std::string> events;
    auto parent = std::make_shared<Recorder>("parent", events);
    auto second = std::make_shared<Recorder>("second", events);
    parent->fail_activate = fail_parent;
    second->fail_activate = !fail_parent;
    std::vector<rpp::ComponentContext> children;
    children.push_back(rpp::ComponentContextAccess::make(std::make_shared<Recorder>("first",
      events)));
    children.push_back(rpp::ComponentContextAccess::make(second));
    auto context = rpp::ComponentContextAccess::make(parent, std::move(children));
    auto result = context.activate();
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.message, "activation rejected");
    auto expected = std::vector<std::string>{"activate first", "activate second"};
    if (fail_parent) {
      expected.push_back("activate parent");
      expected.push_back("deactivate second");
    }
    expected.push_back("deactivate first");
    EXPECT_EQ(events, expected);
  }
}

TEST(ContextLifecycle, RollbackContinuesAndPreservesActivationFailure)
{
  std::vector<std::string> events;
  auto parent = std::make_shared<Recorder>("parent", events);
  parent->fail_activate = true;
  auto second = std::make_shared<Recorder>("second", events);
  second->fail_deactivate = true;
  std::vector<rpp::ComponentContext> children;
  children.push_back(rpp::ComponentContextAccess::make(std::make_shared<Recorder>("first",
    events)));
  children.push_back(rpp::ComponentContextAccess::make(second));
  auto context = rpp::ComponentContextAccess::make(parent, std::move(children));
  auto result = context.activate();
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "activation rejected");
  ASSERT_EQ(events.size(), 5u);
  EXPECT_EQ(events[3], "deactivate second");
  EXPECT_EQ(events[4], "deactivate first");
}

TEST(ContextLifecycle, DeactivationContinuesAndPreservesFirstFailure)
{
  std::vector<std::string> events;
  auto parent = std::make_shared<Recorder>("parent", events);
  parent->fail_deactivate = true;
  auto second = std::make_shared<Recorder>("second", events);
  second->fail_deactivate = true;
  std::vector<rpp::ComponentContext> children;
  children.push_back(rpp::ComponentContextAccess::make(std::make_shared<Recorder>("first",
    events)));
  children.push_back(rpp::ComponentContextAccess::make(second));
  auto context = rpp::ComponentContextAccess::make(parent, std::move(children));
  ASSERT_TRUE(context.activate().success);
  events.clear();
  auto result = context.deactivate();
  EXPECT_FALSE(result.result.success);
  EXPECT_EQ(result.result.message, "deactivation rejected parent");
  EXPECT_FALSE(result.state.has_value());
  EXPECT_EQ(events,
    (std::vector<std::string>{"deactivate parent", "deactivate second", "deactivate first"}));
}

TEST(ContextLifecycle, WrongRootStateDoesNotActivateChildren)
{
  std::vector<std::string> events;
  std::vector<rpp::ComponentContext> children;
  children.push_back(rpp::ComponentContextAccess::make(std::make_shared<Recorder>("child",
    events)));
  auto context = rpp::ComponentContextAccess::make(
    std::make_shared<Recorder>("parent", events), std::move(children));
  EXPECT_FALSE(context.activate(std::string("wrong state")).success);
  EXPECT_TRUE(events.empty());
}

TEST(ContextLifecycle, HandoverIsRootOnlyAndSuppressedOnChildFailure)
{
  struct Estimate {std::vector<double> position;};
  class Stateful : public rpp::LifecyclePlugin<Estimate>
  {
public:
    std::optional<Estimate> estimate;
    rpp::CallbackResult on_activate(const std::optional<Estimate> & state) override
    {
      estimate = state;
      return {};
    }
    rpp::DeactivationResult<Estimate> on_deactivate() override {return {{}, estimate};}
  };
  std::vector<std::string> events;
  auto parent = std::make_shared<Stateful>();
  auto child = std::make_shared<Recorder>("child", events);
  std::vector<rpp::ComponentContext> children;
  children.push_back(rpp::ComponentContextAccess::make(child));
  auto context = rpp::ComponentContextAccess::make(parent, std::move(children));
  std::any input = Estimate{{1.0}};
  ASSERT_TRUE(context.activate(input).success);
  std::any_cast<Estimate &>(input).position.push_back(2.0);
  auto result = context.deactivate();
  ASSERT_TRUE(result.result.success);
  ASSERT_TRUE(result.state.has_value());
  parent->estimate->position.push_back(3.0);
  EXPECT_EQ(std::any_cast<const Estimate &>(*result.state).position, (std::vector<double>{1.0}));

  ASSERT_TRUE(context.activate(*result.state).success);
  child->fail_deactivate = true;
  auto failed = context.deactivate();
  EXPECT_FALSE(failed.result.success);
  EXPECT_FALSE(failed.state.has_value());
}

TEST(ContextLifecycle, NestedRollbackVisitsCompletedSubtreesOnce)
{
  std::vector<std::string> events;
  std::vector<rpp::ComponentContext> first_nested;
  first_nested.push_back(rpp::ComponentContextAccess::make(
    std::make_shared<Recorder>("first nested", events)));
  std::vector<rpp::ComponentContext> second_nested;
  second_nested.push_back(rpp::ComponentContextAccess::make(
    std::make_shared<Recorder>("second nested", events)));
  auto failed = std::make_shared<Recorder>("failed", events);
  failed->fail_activate = true;
  std::vector<rpp::ComponentContext> children;
  children.push_back(rpp::ComponentContextAccess::make(
    std::make_shared<Recorder>("first", events), std::move(first_nested)));
  children.push_back(rpp::ComponentContextAccess::make(failed, std::move(second_nested)));
  auto context = rpp::ComponentContextAccess::make(
    std::make_shared<Recorder>("parent", events), std::move(children));

  EXPECT_FALSE(context.activate().success);
  EXPECT_EQ(events, (std::vector<std::string>{"activate first nested", "activate first",
      "activate second nested", "activate failed", "deactivate second nested",
      "deactivate first", "deactivate first nested"}));
}
