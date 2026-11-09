#include "support/runtime_test_helpers.hpp"
#include <any>
#include <map>
#include <memory>
#include <utility>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <type_traits>

#include "rpp_cpp/runtime.hpp"
#include "rpp_cpp/context.hpp"
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

namespace
{
using rpp::testing::Recorder;
}  // namespace

TEST(ContextLifecycle, ConfiguresChildrenFirstAndCleansParentFirst)
{
  std::vector<std::string> events;
  auto parent = std::make_shared<Recorder>("parent", events);
  std::vector<rpp::ComponentRuntime<>> children;
  children.push_back(
    rpp::ComponentRuntimeAccess::make(
      std::make_shared<Recorder>(
        "first",
        events)));
  children.push_back(
    rpp::ComponentRuntimeAccess::make(
      std::make_shared<Recorder>(
        "second",
        events)));
  auto context = rpp::ComponentRuntimeAccess::make(parent, std::move(children));
  ASSERT_TRUE(context.configure().success);
  ASSERT_NE(parent->context, nullptr);
  EXPECT_EQ(parent->context->get_logger(), context.get_context().get_logger());
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
    std::vector<rpp::ComponentRuntime<>> children;
    children.push_back(rpp::ComponentRuntimeAccess::make(failed));
    children.push_back(
      rpp::ComponentRuntimeAccess::make(
        std::make_shared<Recorder>(
          "unreached",
          events)));
    auto context = rpp::ComponentRuntimeAccess::make(
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
  std::vector<rpp::ComponentRuntime<>> children;
  children.push_back(
    rpp::ComponentRuntimeAccess::make(
      std::make_shared<Recorder>(
        "first",
        events)));
  children.push_back(rpp::ComponentRuntimeAccess::make(second));
  auto context = rpp::ComponentRuntimeAccess::make(parent, std::move(children));
  auto result = context.cleanup();
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "cleanup rejected");
  EXPECT_EQ(
    events,
    (std::vector<std::string>{"cleanup parent", "cleanup second", "cleanup first"}));
}

TEST(ContextLifecycle, EmptyContextSucceeds)
{
  auto context = rpp::ComponentRuntimeAccess::script_slots();
  EXPECT_TRUE(context.configure().success);
  EXPECT_TRUE(context.cleanup().success);
  EXPECT_TRUE(context.activate().success);
  EXPECT_TRUE(context.deactivate().result.success);
}

TEST(ContextLifecycle, ActivationAndDeactivationAffectOnlyTheCurrentPlugin)
{
  std::vector<std::string> events;
  auto parent = std::make_shared<Recorder>("parent", events);
  std::vector<rpp::ComponentRuntime<>> children;
  children.push_back(
    rpp::ComponentRuntimeAccess::make(
      std::make_shared<Recorder>(
        "child",
        events)));
  auto context = rpp::ComponentRuntimeAccess::make(parent, std::move(children));
  ASSERT_TRUE(context.configure().success);
  events.clear();
  ASSERT_TRUE(context.activate().success);
  EXPECT_EQ(events, (std::vector<std::string>{"activate parent"}));
  EXPECT_EQ(context.get_subcomponent_statuses("children")[0].state, rpp::LifecycleState::INACTIVE);

  ASSERT_TRUE(context.get_context().activate_subcomponents().success);
  events.clear();
  ASSERT_TRUE(context.deactivate().result.success);
  EXPECT_EQ(events, (std::vector<std::string>{"deactivate parent"}));
  EXPECT_EQ(context.get_subcomponent_statuses("children")[0].state, rpp::LifecycleState::ACTIVE);
  EXPECT_FALSE(context.cleanup().success);
  EXPECT_TRUE(context.close().success);
}

TEST(ContextLifecycle, PluginsCanManageComponentsFromTheirCallbacks)
{
  std::vector<std::string> events;
  auto parent = std::make_shared<Recorder>("parent", events);
  parent->manage_components = true;
  std::vector<rpp::ComponentRuntime<>> children;
  for (const auto * name : {"first", "second"}) {
    children.push_back(rpp::ComponentRuntimeAccess::make(std::make_shared<Recorder>(name, events)));
  }
  auto context = rpp::ComponentRuntimeAccess::make(parent, std::move(children));
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
  std::vector<rpp::ComponentRuntime<>> children;
  children.push_back(
    rpp::ComponentRuntimeAccess::make(
      std::make_shared<Recorder>(
        "child",
        events)));
  auto context = rpp::ComponentRuntimeAccess::make(parent, std::move(children));
  ASSERT_TRUE(context.configure().success);
  EXPECT_FALSE(context.activate().success);
  EXPECT_EQ(context.get_subcomponent_statuses("children")[0].state, rpp::LifecycleState::ACTIVE);
  ASSERT_TRUE(context.deactivate().result.success);
  EXPECT_EQ(context.get_subcomponent_statuses("children")[0].state, rpp::LifecycleState::INACTIVE);
  EXPECT_TRUE(context.close().success);
}

TEST(ContextLifecycle, ActivationHelperRollsBackOnlyNewlyActivatedChildren)
{
  for (const bool already_active : {false, true}) {
    std::vector<std::string> events;
    auto second = std::make_shared<Recorder>("second", events);
    second->fail_activate = true;
    std::vector<rpp::ComponentRuntime<>> children;
    children.push_back(
      rpp::ComponentRuntimeAccess::make(
        std::make_shared<Recorder>(
          "first",
          events)));
    children.push_back(rpp::ComponentRuntimeAccess::make(second));
    auto context = rpp::ComponentRuntimeAccess::script_slots(std::move(children));
    ASSERT_TRUE(context.configure().success);
    if (already_active) {
      ASSERT_TRUE(context.activate_component("children", "test_child_0").success);
    }
    events.clear();
    const auto result = context.get_context().activate_subcomponents();
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.message, "activation rejected");
    const auto expected = already_active ? std::vector<std::string>{"activate second"} :
    std::vector<std::string>{"activate first", "activate second", "deactivate first"};
    EXPECT_EQ(events, expected);
    EXPECT_EQ(
      context.get_subcomponent_statuses("children")[0].state,
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
  std::vector<rpp::ComponentRuntime<>> children;
  children.push_back(
    rpp::ComponentRuntimeAccess::make(
      std::make_shared<Recorder>(
        "first",
        events)));
  children.push_back(rpp::ComponentRuntimeAccess::make(second));
  children.push_back(rpp::ComponentRuntimeAccess::make(failed));
  auto context = rpp::ComponentRuntimeAccess::script_slots(std::move(children));
  ASSERT_TRUE(context.configure().success);
  events.clear();
  const auto result = context.get_context().activate_subcomponents();
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
  std::vector<rpp::ComponentRuntime<>> children;
  children.push_back(rpp::ComponentRuntimeAccess::make(first));
  children.push_back(rpp::ComponentRuntimeAccess::make(second));
  auto context = rpp::ComponentRuntimeAccess::script_slots(std::move(children));
  ASSERT_TRUE(context.configure().success);
  ASSERT_TRUE(context.get_context().activate_subcomponents().success);
  events.clear();
  const auto result = context.get_context().deactivate_subcomponents();
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
  std::vector<rpp::ComponentRuntime<>> children;
  children.push_back(
    rpp::ComponentRuntimeAccess::make(
      std::make_shared<Recorder>(
        "child",
        events)));
  auto context = rpp::ComponentRuntimeAccess::make(
    std::make_shared<Recorder>("parent", events), std::move(children));
  EXPECT_FALSE(context.activate(std::string("wrong state")).success);
  EXPECT_TRUE(events.empty());
}

TEST(ContextLifecycle, HandoverIsIndependentOfChildTransitions)
{
  using Estimate = rpp::testing::SnapshotEstimate;
  using Stateful = rpp::testing::HandoverPlugin;
  std::vector<std::string> events;
  auto parent = std::make_shared<Stateful>();
  auto child = std::make_shared<Recorder>("child", events);
  std::vector<rpp::ComponentRuntime<>> children;
  children.push_back(rpp::ComponentRuntimeAccess::make(child));
  auto context = rpp::ComponentRuntimeAccess::make(parent, std::move(children));
  std::any input = Estimate{{1.0}};
  ASSERT_TRUE(context.configure().success);
  ASSERT_TRUE(context.activate(input).success);
  EXPECT_EQ(context.get_subcomponent_statuses("children")[0].state, rpp::LifecycleState::INACTIVE);
  ASSERT_TRUE(context.get_context().activate_subcomponents().success);
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
  EXPECT_FALSE(context.get_context().deactivate_subcomponents().success);
  child->fail_deactivate = false;
  EXPECT_TRUE(context.close().success);
}

TEST(ContextLifecycle, NestedPluginsChooseWhichDescendantsParticipate)
{
  std::vector<std::string> events;
  std::vector<rpp::ComponentRuntime<>> first_nested;
  first_nested.push_back(
    rpp::ComponentRuntimeAccess::make(
      std::make_shared<Recorder>("first nested", events)));
  std::vector<rpp::ComponentRuntime<>> second_nested;
  second_nested.push_back(
    rpp::ComponentRuntimeAccess::make(
      std::make_shared<Recorder>("second nested", events)));
  auto second = std::make_shared<Recorder>("second", events);
  second->manage_components = true;
  std::vector<rpp::ComponentRuntime<>> children;
  children.push_back(
    rpp::ComponentRuntimeAccess::make(
      std::make_shared<Recorder>("first", events), std::move(first_nested)));
  children.push_back(rpp::ComponentRuntimeAccess::make(second, std::move(second_nested)));
  auto context = rpp::ComponentRuntimeAccess::script_slots(std::move(children));
  ASSERT_TRUE(context.configure().success);
  events.clear();
  ASSERT_TRUE(context.activate().success);
  EXPECT_TRUE(events.empty());
  ASSERT_TRUE(context.get_context().activate_subcomponents().success);
  ASSERT_TRUE(context.get_context().deactivate_subcomponents().success);
  EXPECT_EQ(
    events, (std::vector<std::string>{"activate first", "activate second nested", "activate second",
      "deactivate second", "deactivate second nested", "deactivate first"}));
  EXPECT_TRUE(context.close().success);
}

TEST(ContextLifecycle, CloseStopsDescendantsUnderInactiveParents)
{
  std::vector<std::string> events;
  std::vector<rpp::ComponentRuntime<>> grandchildren;
  grandchildren.push_back(
    rpp::ComponentRuntimeAccess::make(
      std::make_shared<Recorder>("grandchild", events)));
  std::vector<rpp::ComponentRuntime<>> children;
  children.push_back(
    rpp::ComponentRuntimeAccess::make(
      std::make_shared<Recorder>("middle", events), std::move(grandchildren)));
  auto context = rpp::ComponentRuntimeAccess::script_slots(std::move(children));
  ASSERT_TRUE(context.configure().success);
  ASSERT_TRUE(rpp::ComponentRuntimeAccess::registered_components(context, "children").front()->get_context().activate_subcomponents().success);
  events.clear();
  ASSERT_TRUE(context.close().success);
  EXPECT_EQ(
    events,
    (std::vector<std::string>{"deactivate grandchild", "cleanup middle", "cleanup grandchild"}));
}

namespace
{
using rpp::testing::AxisController;
using rpp::testing::controller;
using rpp::testing::controllers;
using rpp::testing::no_axis_overlap;
}  // namespace

TEST(ContextSlots, StaticOptionAllowsLifecycleButRejectsDynamicSelectionAndMembership)
{
  auto context = rpp::ComponentRuntimeAccess::script(
    controllers({{"first", {}}, {"second", {}}}), "Option[controllers::Controller]");
  EXPECT_FALSE(context.allows_dynamic_components());
  ASSERT_TRUE(context.configure().success);
  ASSERT_TRUE(context.get_context().activate_subcomponents().success);
  const auto first = context.get_context().get_subcomponent<AxisController>("controllers");
  EXPECT_FALSE(context.activate_component("controllers", "second").success);
  EXPECT_FALSE(context.switch_component("controllers", "first", "second").success);
  EXPECT_FALSE(context.load_component("controllers", "third", "/third").success);
  EXPECT_FALSE(context.unload_component("controllers", "first").success);
  EXPECT_FALSE(context.reload_component("controllers", "first").success);
  EXPECT_FALSE(rpp::ComponentRuntimeAccess::registered_component(context, "controllers", "second").activate().success);
  EXPECT_EQ(context.get_context().get_subcomponent<AxisController>("controllers"), first);
  ASSERT_TRUE(context.get_context().deactivate_subcomponents().success);
  ASSERT_TRUE(context.get_context().activate_subcomponents().success);
  EXPECT_TRUE(context.close().success);
}

TEST(ContextSlots, PolicyReceivesEmptySlotTypesAndFixesEachPermissionAtConstruction)
{
  std::map<std::string, rpp::SlotKind> observed;
  bool enabled = true;
  auto context = rpp::ComponentRuntimeAccess::policy_slots(
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
  const auto & candidate = rpp::ComponentRuntimeAccess::registered_component(context, "dynamic", "candidate");
  EXPECT_TRUE(candidate.allows_dynamic_components());
  EXPECT_FALSE(candidate.allows_dynamic_components("children"));
  EXPECT_EQ(observed.size(), 2u);
  EXPECT_TRUE(context.close().success);
}

TEST(ContextSlots, ParentSwitchAndUnloadKeepChildSwitchPermissionsIndependent)
{
  std::vector<rpp::ComponentRuntime<>> parents;
  parents.push_back(
    rpp::ComponentRuntimeAccess::named(
      std::make_shared<AxisController>(), "first",
      controllers({{"child_first", {}}, {"child_second", {}}})));
  parents.push_back(controller("second"));
  auto root = rpp::ComponentRuntimeAccess::script(
    std::move(parents), "Option[controllers::Controller]", true);
  ASSERT_TRUE(root.configure().success);
  ASSERT_TRUE(root.get_context().activate_subcomponents().success);

  const auto & parent = rpp::ComponentRuntimeAccess::registered_component(root, "controllers", "first");
  ASSERT_TRUE(parent.get_context().activate_subcomponents().success);
  EXPECT_TRUE(parent.allows_dynamic_components());
  EXPECT_FALSE(parent.allows_dynamic_components("children"));
  EXPECT_FALSE(
    rpp::ComponentRuntimeAccess::registered_component(parent, "children", "child_first").allows_dynamic_components());
  const auto rejected = parent.switch_component("children", "child_first", "child_second", false);
  EXPECT_FALSE(rejected.success);
  EXPECT_NE(rejected.message.find("disabled"), std::string::npos);

  ASSERT_TRUE(root.switch_component("controllers", "first", "second", false).success);
  for (const auto & child : parent.get_subcomponent_statuses("children")) {
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
    auto context = rpp::ComponentRuntimeAccess::policy_slots(
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
    const auto entries = context.get_subcomponent_statuses("controllers");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries.front().name, "allowed");
    EXPECT_TRUE(context.close().success);
  }
}

TEST(ContextSlots, SingleAndOptionUseTheSameExclusivePolicy)
{
  for (const auto & declaration : {"controllers::Controller", "Option[controllers::Controller]"}) {
    auto context = rpp::ComponentRuntimeAccess::script(
      controllers({{"first", {}}}), declaration,
      true, [](const std::string &) {return controller("second");});
    ASSERT_TRUE(context.configure().success);
    ASSERT_TRUE(context.get_context().activate_subcomponents().success);
    auto first = rpp::ComponentRuntimeAccess::registered_components(context, "controllers").front();
    ASSERT_TRUE(context.load_component("controllers", "second", "/second").success);
    ASSERT_TRUE(context.configure_component("controllers", "second").success);
    EXPECT_FALSE(context.activate_component("controllers", "second").success);
    ASSERT_TRUE(context.switch_component("controllers", "first", "second").success);
    const auto info = context.get_subcomponent_statuses("controllers");
    ASSERT_EQ(info.size(), 2u);
    EXPECT_EQ(info[0].state, rpp::LifecycleState::INACTIVE);
    EXPECT_EQ(info[1].state, rpp::LifecycleState::ACTIVE);
    EXPECT_EQ(
      context.get_context().get_subcomponent<AxisController>("controllers"),
      rpp::ComponentRuntimeAccess::registered_component(context, "controllers", "second").get_instance<AxisController>());
    ASSERT_TRUE(context.unload_component("controllers", "first").success);
    ASSERT_TRUE(context.load_component("controllers", "first", "/replacement").success);
    ASSERT_TRUE(context.configure_component("controllers", "first").success);
    ASSERT_TRUE(context.deactivate_component("controllers", "second").result.success);
    EXPECT_FALSE(first->activate().success);
    EXPECT_TRUE(context.close().success);
  }
}

TEST(ContextSlots, ListActivatesMultipleComponentsAndReturnsRetainableHandles)
{
  auto context = rpp::ComponentRuntimeAccess::script(
    controllers({{"surge", {}}, {"sway", {}}}), "List[controllers::Controller]");
  ASSERT_TRUE(context.configure().success);
  ASSERT_TRUE(context.get_context().activate_subcomponents().success);
  const auto children = rpp::ComponentRuntimeAccess::registered_components(context, "controllers");
  ASSERT_EQ(children.size(), 2u);
  EXPECT_EQ(context.get_context().get_subcomponents<AxisController>("controllers").size(), 2u);
  for (const auto & info : context.get_subcomponent_statuses("controllers")) {
    EXPECT_EQ(info.state, rpp::LifecycleState::ACTIVE);
  }
  EXPECT_TRUE(context.deactivate_component("controllers", "surge").result.success);
  EXPECT_TRUE(context.activate_component("controllers", "surge").success);
  EXPECT_TRUE(context.close().success);
  EXPECT_FALSE(children.front()->activate().success);
}

TEST(ContextSlots, AdmissionChecksInactiveComponentsWithoutMutatingExistingEntries)
{
  auto context = rpp::ComponentRuntimeAccess::script(
    controllers({{"surge", {"surge"}}, {"sway", {"sway"}}}), "List[controllers::Controller]", true,
    [](const std::string &) {return controller("new", {"surge", "yaw"});}, no_axis_overlap);
  ASSERT_TRUE(context.configure().success);
  const auto original = context.get_subcomponent_statuses("controllers");
  auto result = context.load_component("controllers", "surge_yaw", "/surge_yaw");
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.message.find("surge"), std::string::npos);
  const auto info = context.get_subcomponent_statuses("controllers");
  ASSERT_EQ(info.size(), 2u);
  for (size_t i = 0; i < info.size(); ++i) {
    EXPECT_EQ(info[i].state, rpp::LifecycleState::INACTIVE);
    EXPECT_FALSE(info[i].instance_id.empty());
    EXPECT_EQ(info[i].instance_id, original[i].instance_id);
  }
  EXPECT_TRUE(context.close().success);
}

TEST(ContextSlots, InitialAdmissionAndSingleConstructionEnforceTheirContracts)
{
  EXPECT_THROW(
    rpp::ComponentRuntimeAccess::script(
      controllers({{"", {}}}), "controllers::Controller"), rpp::LifecycleError);
  EXPECT_THROW(
    rpp::ComponentRuntimeAccess::script(
      controllers({{"surge", {"surge"}}, {"surge_yaw", {"surge", "yaw"}}}),
      "Option[controllers::Controller]", false, {}, no_axis_overlap), rpp::LifecycleError);
  EXPECT_THROW(
    rpp::ComponentRuntimeAccess::script(
      controllers({{"first", {}}, {"second", {}}}),
      "controllers::Controller"), rpp::LifecycleError);
}

TEST(ContextSlots, ValidatorInstallationAndReentrantMutationUseThePublicContext)
{
  auto context = rpp::ComponentRuntimeAccess::script(
    controllers({{"first", {}}}),
    "List[controllers::Controller]", true, [](const std::string &) {return controller("new");});
  ASSERT_TRUE(
    context.set_registration_validator(
      "controllers", [&](const auto &, const auto &) {
        return context.load_component("controllers", "reentrant", "/other");
      }).success);
  EXPECT_FALSE(context.load_component("controllers", "new", "/new").success);
  const auto info = context.get_subcomponent_statuses("controllers");
  ASSERT_EQ(info.size(), 1u);
  EXPECT_EQ(info[0].name, "first");
  EXPECT_TRUE(context.close().success);
}

TEST(ContextSlots, ManagerRejectsSelfWaitDuringOwnerShutdown)
{
  auto context = rpp::ComponentRuntimeAccess::script(
    controllers({{"first", {}}}),
    "controllers::Controller");
  ASSERT_TRUE(context.configure().success);
  ASSERT_TRUE(context.get_context().activate_subcomponents().success);
  auto result = context.get_context().with_active_component<AxisController>(
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
  std::vector<rpp::ComponentRuntime<>> children;
  children.push_back(controller("first"));
  auto context = rpp::ComponentRuntimeAccess::make(parent, std::move(children));
  ASSERT_TRUE(context.configure().success);
  ASSERT_TRUE(context.activate_component("children", "first").success);
  EXPECT_FALSE(context.activate().success);
  EXPECT_EQ(context.get_subcomponent_statuses("children")[0].state, rpp::LifecycleState::ACTIVE);
  EXPECT_TRUE(context.close().success);
}

namespace
{
template<typename T, typename = void>
struct HasSelfClose : std::false_type {};
template<typename T>
struct HasSelfClose<T, std::void_t<decltype(std::declval<T &>().close())>>: std::true_type {};
template<typename T, typename = void>
struct HasSelfActivate : std::false_type {};
template<typename T>
struct HasSelfActivate<T, std::void_t<decltype(std::declval<T &>().activate())>>: std::true_type {};
static_assert(!HasSelfClose<rpp::ComponentContext>::value);
static_assert(!HasSelfActivate<rpp::ComponentContext>::value);
static_assert(HasSelfActivate<rpp::SubcomponentContext>::value);
template<typename T, typename = void>
struct HasSelfDeactivate : std::false_type {};
template<typename T>
struct HasSelfDeactivate<T, std::void_t<decltype(std::declval<T &>().deactivate())>>: std::true_type {};
static_assert(!HasSelfDeactivate<rpp::ComponentContext>::value);
static_assert(HasSelfDeactivate<rpp::SubcomponentContext>::value);
template<typename T, typename = void>
struct HasDeactivationRequest : std::false_type {};
template<typename T>
struct HasDeactivationRequest<T,
  std::void_t<decltype(std::declval<T &>().request_deactivation())>>: std::true_type {};
static_assert(HasDeactivationRequest<rpp::ComponentContext>::value);
static_assert(!HasDeactivationRequest<rpp::SubcomponentContext>::value);
static_assert(
  std::is_same_v<decltype(std::declval<const rpp::ComponentContext &>().get_subcomponent_context(
    "slot", "component")),
  const rpp::SubcomponentContext &>);

using rpp::testing::managed_recorder;
}  // namespace

TEST(ContextLifecycle, ChildTransitionsAreSynchronousAndSelfDeactivationWaitsForSafePoint)
{
  std::vector<std::string> events;
  auto plugin = std::make_shared<Recorder>("plugin", events);
  auto runtime = managed_recorder(plugin);
  ASSERT_TRUE(runtime.configure().success);
  const auto retained_view = runtime.get_context().get_subcomponent_contexts("controllers").front();
  const auto & child = *retained_view;
  EXPECT_FALSE(child.is_active());
  ASSERT_TRUE(child.activate().success);
  events.clear();
  runtime.get_context().with_active_component<Recorder>("controllers", "plugin", [&](auto &) {
    ASSERT_TRUE(plugin->context->request_deactivation().success);
    ASSERT_TRUE(plugin->context->request_deactivation().success);
    EXPECT_TRUE(child.is_active());
    EXPECT_TRUE(events.empty());
  });
  EXPECT_TRUE(child.is_active());
  ASSERT_TRUE(runtime.process_deactivations().success);
  EXPECT_FALSE(child.is_active());
  EXPECT_EQ(events, (std::vector<std::string>{"deactivate plugin"}));
  ASSERT_TRUE(child.activate().success);
  ASSERT_TRUE(child.deactivate().result.success);
  EXPECT_FALSE(child.is_active());
  EXPECT_TRUE(runtime.close().success);
}

TEST(ContextLifecycle, ParentReceivesChildControlInsteadOfTheChildsSelfContext)
{
  std::vector<std::string> events;
  auto parent = std::make_shared<Recorder>("parent", events);
  auto child_plugin = std::make_shared<Recorder>("child", events);
  std::vector<rpp::ComponentRuntime<>> children;
  children.push_back(rpp::ComponentRuntimeAccess::make(child_plugin));
  auto runtime = rpp::ComponentRuntimeAccess::make(parent, std::move(children));
  ASSERT_TRUE(runtime.configure().success);
  const auto & child = parent->context->get_subcomponent_context("children", "test_child_0");
  EXPECT_EQ(child.get_instance<Recorder>(), child_plugin);
  const auto retained_view = parent->context->get_subcomponent_contexts("children").front();
  auto moved = std::move(runtime);
  EXPECT_EQ(retained_view.get(), &child);
  EXPECT_EQ(&parent->context->get_subcomponent_context("children", "test_child_0"), &child);
  EXPECT_EQ(retained_view->get_instance<Recorder>(), child_plugin);
  ASSERT_TRUE(child.activate().success);
  EXPECT_TRUE(child_plugin->context->is_active());
  ASSERT_TRUE(child.deactivate().result.success);
  EXPECT_FALSE(child_plugin->context->is_active());
  EXPECT_TRUE(moved.close().success);
  EXPECT_THROW(retained_view->is_active(), rpp::LifecycleError);
  EXPECT_THROW(retained_view->get_instance<Recorder>(), rpp::LifecycleError);
}

TEST(ContextLifecycle, FailedSelfDeactivationStaysPendingForRetry)
{
  std::vector<std::string> events;
  auto plugin = std::make_shared<Recorder>("plugin", events);
  auto runtime = managed_recorder(plugin);
  ASSERT_TRUE(runtime.configure().success);
  ASSERT_TRUE(runtime.get_context().activate_subcomponents().success);
  ASSERT_TRUE(plugin->context->request_deactivation().success);
  plugin->fail_deactivate = true;
  EXPECT_FALSE(runtime.process_deactivations().success);
  plugin->fail_deactivate = false;
  ASSERT_TRUE(runtime.process_deactivations().success);
  EXPECT_FALSE(plugin->context->is_active());
  EXPECT_TRUE(runtime.close().success);
}

TEST(ContextLifecycle, ClosedContextCannotTransitionItsReplacement)
{
  std::vector<std::string> events;
  auto original = std::make_shared<Recorder>("original", events);
  auto replacement = std::make_shared<Recorder>("replacement", events);
  auto runtime = managed_recorder(original, [&](const std::string &) {
    return rpp::ComponentRuntimeAccess::named(replacement, "plugin");
  });
  ASSERT_TRUE(runtime.configure().success);
  const auto old_runtime =
    rpp::ComponentRuntimeAccess::registered_components(runtime, "controllers").front();
  const auto * old_view = original->context;
  const auto old_child = runtime.get_context().get_subcomponent_contexts("controllers").front();
  ASSERT_TRUE(runtime.unload_component("controllers", "plugin").success);
  EXPECT_FALSE(old_runtime->activate().success);
  EXPECT_THROW(old_runtime->get_instance<Recorder>(), rpp::LifecycleError);
  ASSERT_TRUE(runtime.load_component("controllers", "plugin", "/replacement").success);
  ASSERT_TRUE(runtime.configure_component("controllers", "plugin").success);
  const auto new_child = runtime.get_context().get_subcomponent_contexts("controllers").front();
  EXPECT_NE(old_child, new_child);
  EXPECT_FALSE(old_runtime->activate().success);
  EXPECT_THROW(old_child->activate(), rpp::LifecycleError);
  EXPECT_THROW(old_child->get_instance<Recorder>(), rpp::LifecycleError);
  EXPECT_EQ(new_child->get_instance<Recorder>(), replacement);
  EXPECT_THROW(old_view->request_deactivation(), rpp::LifecycleError);
  EXPECT_THROW(old_view->is_active(), rpp::LifecycleError);
  EXPECT_FALSE(replacement->context->is_active());
  EXPECT_TRUE(runtime.close().success);
}

TEST(ContextLifecycle, RootMovePreservesTheBorrowedContext)
{
  std::vector<std::string> events;
  auto plugin = std::make_shared<Recorder>("root", events);
  auto runtime = rpp::ComponentRuntimeAccess::make(plugin);
  ASSERT_TRUE(runtime.configure().success);
  const auto * view = plugin->context;
  const auto instance_id = runtime.get_context().get_instance_id();
  auto moved = std::move(runtime);
  EXPECT_EQ(view->get_instance_id(), instance_id);
  EXPECT_EQ(view->get_clock(), moved.get_context().get_clock());
  ASSERT_TRUE(moved.activate().success);
  EXPECT_TRUE(view->is_active());
  ASSERT_TRUE(view->request_deactivation().success);
  auto moved_again = std::move(moved);
  ASSERT_TRUE(moved_again.process_deactivations().success);
  EXPECT_FALSE(moved_again.get_context().is_active());
  EXPECT_TRUE(moved_again.close().success);
}
