#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <set>
#include <vector>

#include "rpp_cpp/context.hpp"
#include "rpp_cpp/context_builder.hpp"
#include "rpp_cpp/component_slots.hpp"

TEST(ComponentSlots, ConstructionCardinalityAndActivationAreIndependent)
{
  const auto single = rpp::parse_component_slot_type("controllers::Controller");
  const auto option = rpp::parse_component_slot_type("Option[controllers::Controller]");
  const auto multiple = rpp::parse_component_slot_type("List[controllers::Controller]");
  EXPECT_EQ(single.kind, rpp::SlotKind::SINGLE);
  EXPECT_FALSE(single.accepts_component_list());
  EXPECT_EQ(option.kind, rpp::SlotKind::OPTION);
  EXPECT_TRUE(option.accepts_component_list());
  EXPECT_EQ(single.activation_policy(), rpp::ActivationPolicy::EXCLUSIVE);
  EXPECT_EQ(single.activation_policy(), option.activation_policy());
  EXPECT_TRUE(multiple.accepts_component_list());
  EXPECT_EQ(multiple.activation_policy(), rpp::ActivationPolicy::MULTIPLE);
  EXPECT_EQ(single.plugin_type, "controllers::Controller");
  EXPECT_EQ(single.plugin_type, option.plugin_type);
  EXPECT_EQ(single.plugin_type, multiple.plugin_type);
}

TEST(ComponentSlots, WhitespaceIsTrimmedAndInvalidDeclarationsAreRejected)
{
  const auto option = rpp::parse_component_slot_type("  Option[ controllers::Controller ]  ");
  EXPECT_EQ(option.plugin_type, "controllers::Controller");
  EXPECT_EQ(option.kind, rpp::SlotKind::OPTION);
  for (const auto * declaration : {"", "List[]", "Option[ ]", "Option[controllers::Controller",
      "List[Option[controllers::Controller]]", "Options[controllers::Controller]"})
  {
    EXPECT_THROW(rpp::parse_component_slot_type(declaration), std::invalid_argument);
  }
}

namespace rpp
{
class ComponentContextAccess
{
public:
  static ComponentContext make(
    std::shared_ptr<Plugin> plugin,
    std::vector<ComponentContext> children = {})
  {
    for (size_t i = 0; i < children.size(); ++i) {
      if (children[i].component_name_.empty()) {
        children[i].component_name_ = "test_child_" + std::to_string(i);
      }
    }
    ComponentContextBuilder::SubcomponentMap children_by_slot;
    children_by_slot.emplace("children", std::move(children));
    auto managers = ComponentContextBuilder::make_slot_managers(std::move(children_by_slot));
    return ComponentContext(
      std::move(plugin), params::Parameters{}, nullptr,
      std::move(managers), ClockOptions{});
  }

  static ComponentContext named(
    std::shared_ptr<Plugin> plugin, const std::string & name,
    std::vector<ComponentContext> children = {})
  {
    auto context = make(std::move(plugin), std::move(children));
    context.component_name_ = name;
    context.plugin_type_ = "controllers::Controller";
    return context;
  }

  static ComponentContext script(
    std::vector<ComponentContext> children, const std::string & declaration,
    bool dynamic = false, ComponentManager::ContextFactory factory = {},
    ComponentContext::RegistrationValidator validator = {})
  {
    ComponentContextBuilder::SubcomponentMap children_by_slot;
    children_by_slot.emplace("controllers", std::move(children));
    auto managers = ComponentContextBuilder::make_slot_managers(
      std::move(children_by_slot), {{"controllers", declaration}}, factory,
      [dynamic](const std::string &, const ComponentSlot &) {return dynamic;},
      {}, {{"controllers", std::move(validator)}});
    return ComponentContext(std::move(managers), ClockOptions{});
  }

  static ComponentContext policy_slots(
    const std::map<std::string, std::string> & spec,
    ComponentContextBuilder::DynamicComponentPolicy policy,
    ComponentManager::ContextFactory factory,
    const std::map<std::string, ComponentManager::NamedValidators> & validators = {})
  {
    auto managers = ComponentContextBuilder::make_slot_managers(
      {}, spec, factory, policy, validators);
    return ComponentContext(std::move(managers), ClockOptions{});
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
      auto result = context->activate_components();
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
      auto children = context->deactivate_components();
      if (result.success) {result = children;}
    }
    return result;
  }
};
}  // namespace

TEST(ContextLifecycle, ConfiguresChildrenFirstAndCleansParentFirst)
{
  std::vector<std::string> events;
  auto parent = std::make_shared<Recorder>("parent", events);
  std::vector<rpp::ComponentContext> children;
  children.push_back(
    rpp::ComponentContextAccess::make(
      std::make_shared<Recorder>(
        "first",
        events)));
  children.push_back(
    rpp::ComponentContextAccess::make(
      std::make_shared<Recorder>(
        "second",
        events)));
  auto context = rpp::ComponentContextAccess::make(parent, std::move(children));
  ASSERT_TRUE(context.configure().success);
  EXPECT_EQ(parent->context, &context);
  ASSERT_TRUE(context.cleanup().success);
  EXPECT_EQ(
    events, (std::vector<std::string>{"configure first", "configure second",
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
    children.push_back(
      rpp::ComponentContextAccess::make(
        std::make_shared<Recorder>(
          "unreached",
          events)));
    auto context = rpp::ComponentContextAccess::make(
      std::make_shared<Recorder>("parent", events), std::move(children));
    auto result = context.configure();
    EXPECT_FALSE(result.success);
    EXPECT_NE(
      result.message.find(throws ? "configure exception" : "configure rejected"),
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
  children.push_back(
    rpp::ComponentContextAccess::make(
      std::make_shared<Recorder>(
        "first",
        events)));
  children.push_back(rpp::ComponentContextAccess::make(second));
  auto context = rpp::ComponentContextAccess::make(parent, std::move(children));
  auto result = context.cleanup();
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "cleanup rejected");
  EXPECT_EQ(
    events,
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

TEST(ContextLifecycle, ActivationAndDeactivationAffectOnlyTheCurrentPlugin)
{
  std::vector<std::string> events;
  auto parent = std::make_shared<Recorder>("parent", events);
  std::vector<rpp::ComponentContext> children;
  children.push_back(
    rpp::ComponentContextAccess::make(
      std::make_shared<Recorder>(
        "child",
        events)));
  auto context = rpp::ComponentContextAccess::make(parent, std::move(children));
  ASSERT_TRUE(context.configure().success);
  events.clear();
  ASSERT_TRUE(context.activate().success);
  EXPECT_EQ(events, (std::vector<std::string>{"activate parent"}));
  EXPECT_EQ(context.list_components("children")[0].state, rpp::LifecycleState::INACTIVE);

  ASSERT_TRUE(context.activate_components().success);
  events.clear();
  ASSERT_TRUE(context.deactivate().result.success);
  EXPECT_EQ(events, (std::vector<std::string>{"deactivate parent"}));
  EXPECT_EQ(context.list_components("children")[0].state, rpp::LifecycleState::ACTIVE);
  EXPECT_FALSE(context.cleanup().success);
  EXPECT_TRUE(context.close().success);
}

TEST(ContextLifecycle, PluginsCanManageComponentsFromTheirCallbacks)
{
  std::vector<std::string> events;
  auto parent = std::make_shared<Recorder>("parent", events);
  parent->manage_components = true;
  std::vector<rpp::ComponentContext> children;
  for (const auto * name : {"first", "second"}) {
    children.push_back(rpp::ComponentContextAccess::make(std::make_shared<Recorder>(name, events)));
  }
  auto context = rpp::ComponentContextAccess::make(parent, std::move(children));
  ASSERT_TRUE(context.configure().success);
  events.clear();
  ASSERT_TRUE(context.activate().success);
  ASSERT_TRUE(context.deactivate().result.success);
  EXPECT_EQ(
    events, (std::vector<std::string>{"activate first", "activate second", "activate parent",
      "deactivate parent", "deactivate second", "deactivate first"}));
  EXPECT_TRUE(context.close().success);
}

TEST(ContextLifecycle, ParentCallbackFailureLeavesChildRecoveryToThePlugin)
{
  std::vector<std::string> events;
  auto parent = std::make_shared<Recorder>("parent", events);
  parent->manage_components = true;
  parent->fail_activate = true;
  std::vector<rpp::ComponentContext> children;
  children.push_back(
    rpp::ComponentContextAccess::make(
      std::make_shared<Recorder>(
        "child",
        events)));
  auto context = rpp::ComponentContextAccess::make(parent, std::move(children));
  ASSERT_TRUE(context.configure().success);
  EXPECT_FALSE(context.activate().success);
  EXPECT_EQ(context.list_components("children")[0].state, rpp::LifecycleState::ACTIVE);
  ASSERT_TRUE(context.deactivate().result.success);
  EXPECT_EQ(context.list_components("children")[0].state, rpp::LifecycleState::INACTIVE);
  EXPECT_TRUE(context.close().success);
}

TEST(ContextLifecycle, ActivationHelperRollsBackOnlyNewlyActivatedChildren)
{
  for (const bool already_active : {false, true}) {
    std::vector<std::string> events;
    auto second = std::make_shared<Recorder>("second", events);
    second->fail_activate = true;
    std::vector<rpp::ComponentContext> children;
    children.push_back(
      rpp::ComponentContextAccess::make(
        std::make_shared<Recorder>(
          "first",
          events)));
    children.push_back(rpp::ComponentContextAccess::make(second));
    auto context = rpp::ComponentContextAccess::make(nullptr, std::move(children));
    ASSERT_TRUE(context.configure().success);
    if (already_active) {
      ASSERT_TRUE(context.activate_component("children", "test_child_0").success);
    }
    events.clear();
    const auto result = context.activate_components();
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.message, "activation rejected");
    const auto expected = already_active ? std::vector<std::string>{"activate second"} :
    std::vector<std::string>{"activate first", "activate second", "deactivate first"};
    EXPECT_EQ(events, expected);
    EXPECT_EQ(
      context.list_components("children")[0].state,
      already_active ? rpp::LifecycleState::ACTIVE : rpp::LifecycleState::INACTIVE);
    EXPECT_TRUE(context.close().success);
  }
}

TEST(ContextLifecycle, ActivationHelperContinuesRollbackAndPreservesActivationFailure)
{
  std::vector<std::string> events;
  auto second = std::make_shared<Recorder>("second", events);
  second->fail_deactivate = true;
  auto failed = std::make_shared<Recorder>("failed", events);
  failed->fail_activate = true;
  std::vector<rpp::ComponentContext> children;
  children.push_back(
    rpp::ComponentContextAccess::make(
      std::make_shared<Recorder>(
        "first",
        events)));
  children.push_back(rpp::ComponentContextAccess::make(second));
  children.push_back(rpp::ComponentContextAccess::make(failed));
  auto context = rpp::ComponentContextAccess::make(nullptr, std::move(children));
  ASSERT_TRUE(context.configure().success);
  events.clear();
  const auto result = context.activate_components();
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "activation rejected");
  EXPECT_EQ(
    events, (std::vector<std::string>{"activate first", "activate second", "activate failed",
      "deactivate second", "deactivate first"}));
  second->fail_deactivate = false;
  EXPECT_TRUE(context.close().success);
}

TEST(ContextLifecycle, DeactivationHelperContinuesAndPreservesFirstFailure)
{
  std::vector<std::string> events;
  auto first = std::make_shared<Recorder>("first", events);
  auto second = std::make_shared<Recorder>("second", events);
  first->fail_deactivate = true;
  second->fail_deactivate = true;
  std::vector<rpp::ComponentContext> children;
  children.push_back(rpp::ComponentContextAccess::make(first));
  children.push_back(rpp::ComponentContextAccess::make(second));
  auto context = rpp::ComponentContextAccess::make(nullptr, std::move(children));
  ASSERT_TRUE(context.configure().success);
  ASSERT_TRUE(context.activate_components().success);
  events.clear();
  const auto result = context.deactivate_components();
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "deactivation rejected second");
  EXPECT_EQ(events, (std::vector<std::string>{"deactivate second", "deactivate first"}));
  first->fail_deactivate = false;
  second->fail_deactivate = false;
  EXPECT_TRUE(context.close().success);
}

TEST(ContextLifecycle, WrongRootStateDoesNotActivateChildren)
{
  std::vector<std::string> events;
  std::vector<rpp::ComponentContext> children;
  children.push_back(
    rpp::ComponentContextAccess::make(
      std::make_shared<Recorder>(
        "child",
        events)));
  auto context = rpp::ComponentContextAccess::make(
    std::make_shared<Recorder>("parent", events), std::move(children));
  EXPECT_FALSE(context.activate(std::string("wrong state")).success);
  EXPECT_TRUE(events.empty());
}

TEST(ContextLifecycle, HandoverIsIndependentOfChildTransitions)
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
  ASSERT_TRUE(context.configure().success);
  ASSERT_TRUE(context.activate(input).success);
  EXPECT_EQ(context.list_components("children")[0].state, rpp::LifecycleState::INACTIVE);
  ASSERT_TRUE(context.activate_components().success);
  std::any_cast<Estimate &>(input).position.push_back(2.0);
  const auto result = context.deactivate();
  ASSERT_TRUE(result.result.success);
  ASSERT_TRUE(result.state.has_value());
  parent->estimate->position.push_back(3.0);
  EXPECT_EQ(std::any_cast<const Estimate &>(*result.state).position, (std::vector<double>{1.0}));

  ASSERT_TRUE(context.activate(*result.state).success);
  child->fail_deactivate = true;
  const auto stopped_parent = context.deactivate();
  EXPECT_TRUE(stopped_parent.result.success);
  EXPECT_TRUE(stopped_parent.state.has_value());
  EXPECT_FALSE(context.deactivate_components().success);
  child->fail_deactivate = false;
  EXPECT_TRUE(context.close().success);
}

TEST(ContextLifecycle, NestedPluginsChooseWhichDescendantsParticipate)
{
  std::vector<std::string> events;
  std::vector<rpp::ComponentContext> first_nested;
  first_nested.push_back(
    rpp::ComponentContextAccess::make(
      std::make_shared<Recorder>("first nested", events)));
  std::vector<rpp::ComponentContext> second_nested;
  second_nested.push_back(
    rpp::ComponentContextAccess::make(
      std::make_shared<Recorder>("second nested", events)));
  auto second = std::make_shared<Recorder>("second", events);
  second->manage_components = true;
  std::vector<rpp::ComponentContext> children;
  children.push_back(
    rpp::ComponentContextAccess::make(
      std::make_shared<Recorder>("first", events), std::move(first_nested)));
  children.push_back(rpp::ComponentContextAccess::make(second, std::move(second_nested)));
  auto context = rpp::ComponentContextAccess::make(nullptr, std::move(children));
  ASSERT_TRUE(context.configure().success);
  events.clear();
  ASSERT_TRUE(context.activate().success);
  EXPECT_TRUE(events.empty());
  ASSERT_TRUE(context.activate_components().success);
  ASSERT_TRUE(context.deactivate_components().success);
  EXPECT_EQ(
    events, (std::vector<std::string>{"activate first", "activate second nested", "activate second",
      "deactivate second", "deactivate second nested", "deactivate first"}));
  EXPECT_TRUE(context.close().success);
}

TEST(ContextLifecycle, CloseStopsDescendantsUnderInactiveParents)
{
  std::vector<std::string> events;
  std::vector<rpp::ComponentContext> grandchildren;
  grandchildren.push_back(
    rpp::ComponentContextAccess::make(
      std::make_shared<Recorder>("grandchild", events)));
  std::vector<rpp::ComponentContext> children;
  children.push_back(
    rpp::ComponentContextAccess::make(
      std::make_shared<Recorder>("middle", events), std::move(grandchildren)));
  auto context = rpp::ComponentContextAccess::make(nullptr, std::move(children));
  ASSERT_TRUE(context.configure().success);
  ASSERT_TRUE(context.get_subcomponent_contexts("children").front()->activate_components().success);
  events.clear();
  ASSERT_TRUE(context.close().success);
  EXPECT_EQ(
    events,
    (std::vector<std::string>{"deactivate grandchild", "cleanup middle", "cleanup grandchild"}));
}

namespace
{
class AxisController : public rpp::LifecyclePlugin<void>
{
public:
  explicit AxisController(std::set<std::string> axes = {})
  : axes(std::move(axes)) {}
  std::set<std::string> axes;
};

rpp::ComponentContext controller(const std::string & name, std::set<std::string> axes = {})
{
  return rpp::ComponentContextAccess::named(
    std::make_shared<AxisController>(std::move(axes)),
    name);
}

std::vector<rpp::ComponentContext> controllers(
  std::initializer_list<std::pair<std::string, std::set<std::string>>> definitions)
{
  std::vector<rpp::ComponentContext> result;
  for (const auto & [name, axes] : definitions) {
    result.push_back(controller(name, axes));
  }
  return result;
}

rpp::CallbackResult no_axis_overlap(
  const rpp::ComponentContext & candidate,
  const std::vector<std::shared_ptr<const rpp::ComponentContext>> & registered)
{
  const auto incoming = candidate.get_instance<AxisController>();
  for (const auto & existing : registered) {
    for (const auto & axis : existing->get_instance<AxisController>()->axes) {
      if (incoming->axes.count(axis)) {
        return rpp::CallbackResult::failure("Control axis already registered: " + axis);
      }
    }
  }
  return {};
}
}  // namespace

TEST(ContextSlots, StaticOptionAllowsLifecycleButRejectsDynamicSelectionAndMembership)
{
  auto context = rpp::ComponentContextAccess::script(
    controllers({{"first", {}}, {"second", {}}}), "Option[controllers::Controller]");
  EXPECT_FALSE(context.allows_dynamic_components());
  ASSERT_TRUE(context.configure().success);
  ASSERT_TRUE(context.activate_components().success);
  const auto first = context.get_component<AxisController>("controllers");
  EXPECT_FALSE(context.activate_component("controllers", "second").success);
  EXPECT_FALSE(context.switch_component("controllers", "first", "second").success);
  EXPECT_FALSE(context.load_component("controllers", "third", "/third").success);
  EXPECT_FALSE(context.unload_component("controllers", "first").success);
  EXPECT_FALSE(context.reload_component("controllers", "first").success);
  EXPECT_FALSE(context.get_subcomponent_context("controllers", "second").activate().success);
  EXPECT_EQ(context.get_component<AxisController>("controllers"), first);
  ASSERT_TRUE(context.deactivate_components().success);
  ASSERT_TRUE(context.activate_components().success);
  EXPECT_TRUE(context.close().success);
}

TEST(ContextSlots, PolicyReceivesEmptySlotTypesAndFixesEachPermissionAtConstruction)
{
  std::map<std::string, rpp::SlotKind> observed;
  bool enabled = true;
  auto context = rpp::ComponentContextAccess::policy_slots(
    {{"dynamic", "Option[controllers::Controller]"}, {"fixed", "List[controllers::Controller]"}},
    [&](const std::string & name, const rpp::ComponentSlot & type) {
      EXPECT_TRUE(observed.emplace(name, type.kind).second);
      EXPECT_EQ(type.plugin_type, "controllers::Controller");
      return enabled && name == "dynamic";
    }, [](const std::string &) {return controller("loaded");});
  enabled = false;
  EXPECT_EQ(observed.size(), 2u);
  EXPECT_EQ(observed.at("dynamic"), rpp::SlotKind::OPTION);
  EXPECT_EQ(observed.at("fixed"), rpp::SlotKind::LIST);
  EXPECT_TRUE(context.allows_dynamic_components("dynamic"));
  EXPECT_FALSE(context.allows_dynamic_components("fixed"));
  EXPECT_TRUE(context.load_component("dynamic", "candidate", "/candidate").success);
  EXPECT_FALSE(context.load_component("fixed", "candidate", "/candidate").success);
  EXPECT_FALSE(context.allows_dynamic_components());
  const auto & candidate = context.get_subcomponent_context("dynamic", "candidate");
  EXPECT_TRUE(candidate.allows_dynamic_components());
  EXPECT_FALSE(candidate.allows_dynamic_components("children"));
  EXPECT_EQ(observed.size(), 2u);
  EXPECT_TRUE(context.close().success);
}

TEST(ContextSlots, ParentSwitchAndUnloadKeepChildSwitchPermissionsIndependent)
{
  std::vector<rpp::ComponentContext> parents;
  parents.push_back(
    rpp::ComponentContextAccess::named(
      std::make_shared<AxisController>(), "first",
      controllers({{"child_first", {}}, {"child_second", {}}})));
  parents.push_back(controller("second"));
  auto root = rpp::ComponentContextAccess::script(
    std::move(parents), "Option[controllers::Controller]", true);
  ASSERT_TRUE(root.configure().success);
  ASSERT_TRUE(root.activate_components().success);

  const auto & parent = root.get_subcomponent_context("controllers", "first");
  ASSERT_TRUE(parent.activate_components().success);
  EXPECT_TRUE(parent.allows_dynamic_components());
  EXPECT_FALSE(parent.allows_dynamic_components("children"));
  EXPECT_FALSE(
    parent.get_subcomponent_context(
      "children",
      "child_first").allows_dynamic_components());
  const auto rejected = parent.switch_component("children", "child_first", "child_second", false);
  EXPECT_FALSE(rejected.success);
  EXPECT_NE(rejected.message.find("disabled"), std::string::npos);

  ASSERT_TRUE(root.switch_component("controllers", "first", "second", false).success);
  for (const auto & child : parent.list_components("children")) {
    EXPECT_EQ(child.state, rpp::LifecycleState::ACTIVE);
  }
  EXPECT_FALSE(parent.allows_dynamic_components("children"));
  ASSERT_TRUE(root.unload_component("controllers", "first").success);
  EXPECT_TRUE(root.close().success);
}

TEST(ContextSlots, AdmissionRulesMatchBaseTypeAndCandidateRegistrationName)
{
  for (const auto & declaration :
    {"Option[controllers::Controller]", "List[controllers::Controller]"})
  {
    int validations = 0;
    auto context = rpp::ComponentContextAccess::policy_slots(
      {{"controllers", declaration}},
      [](const std::string &, const rpp::ComponentSlot &) {return true;},
      [](const std::string &) {return controller("factory_name");},
      {{"controllers::Controller", {{"blocked", [&](const auto &, const auto & registered) {
            ++validations;
            EXPECT_EQ(registered.size(), 1u);
            return rpp::CallbackResult::failure("Rejected candidate");
          }}}},
        {"other::Controller", {{"allowed", [](const auto &, const auto &) {
              return rpp::CallbackResult::failure("Wrong type rule");
            }}}}});
    ASSERT_TRUE(context.load_component("controllers", "allowed", "/allowed").success);
    EXPECT_FALSE(context.load_component("controllers", "blocked", "/blocked").success);
    EXPECT_EQ(validations, 1);
    const auto entries = context.list_components("controllers");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries.front().name, "allowed");
    EXPECT_TRUE(context.close().success);
  }
}

TEST(ContextSlots, SingleAndOptionUseTheSameExclusivePolicy)
{
  for (const auto & declaration : {"controllers::Controller", "Option[controllers::Controller]"}) {
    auto context = rpp::ComponentContextAccess::script(
      controllers({{"first", {}}}), declaration,
      true, [](const std::string &) {return controller("second");});
    ASSERT_TRUE(context.configure().success);
    ASSERT_TRUE(context.activate_components().success);
    auto first = context.get_subcomponent_contexts("controllers").front();
    ASSERT_TRUE(context.load_component("controllers", "second", "/second").success);
    ASSERT_TRUE(context.configure_component("controllers", "second").success);
    EXPECT_FALSE(context.activate_component("controllers", "second").success);
    ASSERT_TRUE(context.switch_component("controllers", "first", "second").success);
    const auto info = context.list_components("controllers");
    ASSERT_EQ(info.size(), 2u);
    EXPECT_EQ(info[0].state, rpp::LifecycleState::INACTIVE);
    EXPECT_EQ(info[1].state, rpp::LifecycleState::ACTIVE);
    EXPECT_EQ(
      context.get_component<AxisController>("controllers"),
      context.get_subcomponent_context("controllers", "second").get_instance<AxisController>());
    ASSERT_TRUE(context.unload_component("controllers", "first").success);
    ASSERT_TRUE(context.load_component("controllers", "first", "/replacement").success);
    ASSERT_TRUE(context.configure_component("controllers", "first").success);
    ASSERT_TRUE(context.deactivate_component("controllers", "second").result.success);
    EXPECT_FALSE(first->activate().success);
    EXPECT_TRUE(context.close().success);
  }
}

TEST(ContextSlots, ListActivatesMultipleComponentsAndReturnsOwnedContexts)
{
  auto context = rpp::ComponentContextAccess::script(
    controllers({{"surge", {}}, {"sway", {}}}), "List[controllers::Controller]");
  ASSERT_TRUE(context.configure().success);
  ASSERT_TRUE(context.activate_components().success);
  const auto children = context.get_subcomponent_contexts("controllers");
  ASSERT_EQ(children.size(), 2u);
  EXPECT_EQ(context.get_components<AxisController>("controllers").size(), 2u);
  for (const auto & info : context.list_components("controllers")) {
    EXPECT_EQ(info.state, rpp::LifecycleState::ACTIVE);
  }
  EXPECT_TRUE(context.deactivate_component("controllers", "surge").result.success);
  EXPECT_TRUE(context.activate_component("controllers", "surge").success);
  EXPECT_TRUE(context.close().success);
  EXPECT_THROW(children.front()->activate(), rpp::LifecycleError);
}

TEST(ContextSlots, AdmissionChecksInactiveComponentsWithoutMutatingExistingEntries)
{
  auto context = rpp::ComponentContextAccess::script(
    controllers({{"surge", {"surge"}}, {"sway", {"sway"}}}), "List[controllers::Controller]", true,
    [](const std::string &) {return controller("new", {"surge", "yaw"});}, no_axis_overlap);
  ASSERT_TRUE(context.configure().success);
  auto result = context.load_component("controllers", "surge_yaw", "/surge_yaw");
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.message.find("surge"), std::string::npos);
  const auto info = context.list_components("controllers");
  ASSERT_EQ(info.size(), 2u);
  for (const auto & item : info) {
    EXPECT_EQ(item.state, rpp::LifecycleState::INACTIVE);
    EXPECT_EQ(item.generation, 1u);
  }
  EXPECT_TRUE(context.close().success);
}

TEST(ContextSlots, InitialAdmissionAndSingleConstructionEnforceTheirContracts)
{
  EXPECT_THROW(
    rpp::ComponentContextAccess::script(
      controllers({{"", {}}}), "controllers::Controller"), rpp::LifecycleError);
  EXPECT_THROW(
    rpp::ComponentContextAccess::script(
      controllers({{"surge", {"surge"}}, {"surge_yaw", {"surge", "yaw"}}}),
      "Option[controllers::Controller]", false, {}, no_axis_overlap), rpp::LifecycleError);
  EXPECT_THROW(
    rpp::ComponentContextAccess::script(
      controllers({{"first", {}}, {"second", {}}}),
      "controllers::Controller"), rpp::LifecycleError);
}

TEST(ContextSlots, ValidatorInstallationAndReentrantMutationUseThePublicContext)
{
  auto context = rpp::ComponentContextAccess::script(
    controllers({{"first", {}}}),
    "List[controllers::Controller]", true, [](const std::string &) {return controller("new");});
  ASSERT_TRUE(
    context.set_registration_validator(
      "controllers", [&](const auto &, const auto &) {
        return context.load_component("controllers", "reentrant", "/other");
      }).success);
  EXPECT_FALSE(context.load_component("controllers", "new", "/new").success);
  const auto info = context.list_components("controllers");
  ASSERT_EQ(info.size(), 1u);
  EXPECT_EQ(info[0].name, "first");
  EXPECT_TRUE(context.close().success);
}

TEST(ContextSlots, GuardedComponentCannotCloseItsRootContext)
{
  auto context = rpp::ComponentContextAccess::script(
    controllers({{"first", {}}}),
    "controllers::Controller");
  ASSERT_TRUE(context.configure().success);
  ASSERT_TRUE(context.activate_components().success);
  auto result = context.with_active_component<AxisController>(
    "controllers", "first", [&](auto &) {
      return context.close();
    });
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(context.close().success);
}

TEST(ContextSlots, ParentActivationFailurePreservesAlreadyActiveChild)
{
  std::vector<std::string> events;
  auto parent = std::make_shared<Recorder>("parent", events);
  parent->fail_activate = true;
  std::vector<rpp::ComponentContext> children;
  children.push_back(controller("first"));
  auto context = rpp::ComponentContextAccess::make(parent, std::move(children));
  ASSERT_TRUE(context.configure().success);
  ASSERT_TRUE(context.activate_component("children", "first").success);
  EXPECT_FALSE(context.activate().success);
  EXPECT_EQ(context.list_components("children")[0].state, rpp::LifecycleState::ACTIVE);
  EXPECT_TRUE(context.close().success);
}
