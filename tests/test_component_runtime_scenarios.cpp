#include "support/runtime_test_helpers.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "rpp_cpp/context.hpp"

namespace
{
using namespace std::chrono_literals;

using rpp::testing::ControllerState;
using rpp::testing::EventLog;
using rpp::testing::ScenarioController;

template<typename Runtime, typename = void>
struct HasRootInstanceAccessor : std::false_type {};

template<typename Runtime>
struct HasRootInstanceAccessor<Runtime, std::void_t<
    decltype(std::declval<const Runtime &>().template get_instance<ScenarioController>())>>
  : std::true_type {};

template<class Threading>
class ComponentRuntimeScenarios : public ::testing::Test
{
public:
  using Runtime = rpp::ComponentRuntime<Threading::concurrent>;
  using ScriptRuntime = rpp::ScriptRuntime<Threading::concurrent>;
  EventLog log;
  std::map<std::string, std::shared_ptr<ScenarioController>> controllers;
  std::unique_ptr<ScriptRuntime> root;
  std::unique_ptr<Runtime> component_root;

  void build(
    const std::string & declaration = "Option[controllers::Controller]",
    bool dynamic = true)
  {
    root = std::make_unique<ScriptRuntime>(
      rpp::testing::simple_controllers<Threading>(log, controllers, declaration, dynamic));
  }

  void build_nested()
  {
    root = std::make_unique<ScriptRuntime>(
      rpp::testing::nested_controllers<Threading>(log, controllers));
  }

  void build_root(const rpp::testing::FakeComponentTree & tree = {"root", {}})
  {
    component_root = std::make_unique<Runtime>(
      rpp::testing::component_root<Threading>(tree, log, controllers));
  }

  void TearDown() override
  {
    for (auto & [name, controller] : controllers) {
      (void)name;
      controller->reject_activation = false;
      controller->reject_deactivation = false;
    }
    if (root) {EXPECT_TRUE(root->close().success);}
    if (component_root) {EXPECT_TRUE(component_root->close().success);}
  }

  rpp::LifecycleState state(const std::string & name)
  {
    for (const auto & info : root->get_subcomponent_statuses("controllers")) {
      if (info.name == name) {return info.state;}
    }
    throw std::logic_error("Missing scenario controller: " + name);
  }

  int call(const std::string & name)
  {
    return root->get_context().template with_active_component<ScenarioController>(
      "controllers", name, [](auto & controller) {return controller.state.value;});
  }
};

struct PolicyNames
{
  template<class Policy>
  static std::string GetName(int)
  {
    return Policy::concurrent ? "Concurrent" : "SingleThreaded";
  }
};
using Policies = ::testing::Types<rpp::SingleThreaded, rpp::Concurrent>;
TYPED_TEST_SUITE(ComponentRuntimeScenarios, Policies, PolicyNames);

TYPED_TEST(ComponentRuntimeScenarios, ScriptOwnsSlotsDirectlyAndCandidatesShareTheirSlotManager)
{
  this->build();
  const auto & context = this->root->get_context();
  using Script = std::remove_reference_t<decltype(*this->root)>;
  static_assert(!HasRootInstanceAccessor<Script>::value);
  EXPECT_TRUE(context.get_instance_id().empty());
  EXPECT_FALSE(context.is_active());
  EXPECT_FALSE(context.request_deactivation().success);

  auto a = rpp::ComponentRuntimeAccess::registered_component(*this->root, "controllers", "a");
  auto b = rpp::ComponentRuntimeAccess::registered_component(*this->root, "controllers", "b");
  EXPECT_EQ(rpp::ComponentRuntimeAccess::owning_slot(a).lock(),
    rpp::ComponentRuntimeAccess::owning_slot(b).lock());
  ASSERT_TRUE(this->root->configure().success);
  ASSERT_TRUE(context.activate_subcomponents().success);
  ASSERT_TRUE(a.get_context().request_deactivation().success);
  ASSERT_TRUE(this->root->process_deactivations().success);
  EXPECT_EQ(this->state("a"), rpp::LifecycleState::INACTIVE);
}

TYPED_TEST(ComponentRuntimeScenarios, ListLookupRequiresANameEvenWithOnlyOneLoadedSubcomponent)
{
  this->build("List[controllers::Controller]");
  const auto & context = this->root->get_context();
  EXPECT_THROW(context.template get_subcomponent_instance<ScenarioController>("controllers"),
    rpp::LifecycleError);
  EXPECT_EQ(this->root->get_context().template get_subcomponent_instance<ScenarioController>(
      "controllers", "a"),
    this->controllers.at("a"));
  EXPECT_EQ(context.template get_subcomponent_instance<ScenarioController>("controllers", "b"),
    this->controllers.at("b"));

  ASSERT_TRUE(this->root->unload_component("controllers", "b").success);
  EXPECT_THROW(context.template get_subcomponent_instance<ScenarioController>("controllers"),
    rpp::LifecycleError);
  EXPECT_EQ(context.template get_subcomponent_instance<ScenarioController>("controllers", "a"),
    this->controllers.at("a"));
  EXPECT_THROW(context.template get_subcomponent_instance<ScenarioController>("controllers", "b"),
    rpp::LifecycleError);
}

TYPED_TEST(ComponentRuntimeScenarios, RetainedSubcomponentViewDoesNotKeepPluginAlive)
{
  auto plugin = std::make_shared<ScenarioController>("only", this->log);
  std::weak_ptr<ScenarioController> lifetime = plugin;
  std::vector<typename TestFixture::Runtime> components;
  components.push_back(rpp::ComponentRuntimeAccess::named<TypeParam>(std::move(plugin), "only"));
  this->root = std::make_unique<typename TestFixture::ScriptRuntime>(
    rpp::ComponentRuntimeAccess::script<TypeParam>(std::move(components),
      "controllers::Controller"));
  const auto view = *this->root->get_context().enumerate_subcomponent_contexts(
    "controllers").begin();
  ASSERT_FALSE(lifetime.expired());
  ASSERT_TRUE(this->root->configure().success);
  ASSERT_TRUE(this->root->get_context().activate_subcomponents().success);

  ASSERT_TRUE(this->root->close().success);
  EXPECT_TRUE(lifetime.expired());
  EXPECT_THROW(view->template get_instance<ScenarioController>(), rpp::LifecycleError);
  EXPECT_THROW(view->is_active(), rpp::LifecycleError);
  EXPECT_EQ(this->log.snapshot(), (std::vector<std::string>{
      "configure only", "activate only", "deactivate only", "cleanup only"}));
}

TYPED_TEST(ComponentRuntimeScenarios, RetrievedInstanceOwnsPluginUntilItsLastPointerIsReleased)
{
  auto plugin = std::make_shared<ScenarioController>("only", this->log);
  std::weak_ptr<ScenarioController> lifetime = plugin;
  std::vector<typename TestFixture::Runtime> components;
  components.push_back(rpp::ComponentRuntimeAccess::named<TypeParam>(std::move(plugin), "only"));
  this->root = std::make_unique<typename TestFixture::ScriptRuntime>(
    rpp::ComponentRuntimeAccess::script<TypeParam>(std::move(components),
      "controllers::Controller"));
  const auto view = *this->root->get_context().enumerate_subcomponent_contexts(
    "controllers").begin();
  ASSERT_TRUE(this->root->configure().success);
  ASSERT_TRUE(view->activate().success);
  auto instance = view->template get_instance<ScenarioController>();
  instance->state.value = 42;

  ASSERT_TRUE(this->root->close().success);
  ASSERT_FALSE(lifetime.expired());
  EXPECT_EQ(instance->state.value, 42);
  EXPECT_THROW(view->template get_instance<ScenarioController>(), rpp::LifecycleError);
  EXPECT_EQ(this->log.snapshot(), (std::vector<std::string>{
      "configure only", "activate only", "deactivate only", "cleanup only"}));

  instance.reset();
  EXPECT_TRUE(lifetime.expired());
  EXPECT_THROW(view->is_active(), rpp::LifecycleError);
}

TYPED_TEST(ComponentRuntimeScenarios,
    ComponentRootRetainsItsFixedSlotAndMovesWithoutRebindingThePluginContext)
{
  this->build_root({"root", {{"child", {{"grandchild", {}}}}}});
  const auto manager = rpp::ComponentRuntimeAccess::owning_slot(*this->component_root);
  ASSERT_FALSE(manager.expired());
  EXPECT_FALSE(this->component_root->allows_dynamic_components());
  EXPECT_EQ(manager.lock()->list().size(), 1u);
  EXPECT_EQ(rpp::ComponentRuntimeAccess::status(*this->component_root).state,
      rpp::LifecycleState::UNCONFIGURED);
  EXPECT_EQ(this->component_root->template get_instance<ScenarioController>(),
      this->controllers.at("root"));
  ASSERT_TRUE(this->component_root->configure().success);
  const auto * context = &this->component_root->get_context();
  const auto instance_id = this->component_root->get_context().get_instance_id();
  auto moved = std::move(*this->component_root);
  this->component_root = std::make_unique<typename TestFixture::Runtime>(std::move(moved));
  EXPECT_EQ(&this->component_root->get_context(), context);
  EXPECT_EQ(context->get_instance_id(), instance_id);
  ASSERT_TRUE(this->component_root->activate().success);
  EXPECT_TRUE(context->is_active());
  EXPECT_TRUE(this->controllers.at("grandchild")->context->is_active());
  ASSERT_TRUE(this->component_root->deactivate().result.success);
  EXPECT_FALSE(this->controllers.at("grandchild")->context->is_active());
  ASSERT_TRUE(this->component_root->cleanup().success);
  ASSERT_TRUE(this->component_root->close().success);
  EXPECT_THROW(context->is_active(), rpp::LifecycleError);
  this->component_root.reset();
  EXPECT_TRUE(manager.expired());  // The manager/runtime/context graph has no owning cycle.
  EXPECT_EQ(this->log.snapshot(), (std::vector<std::string>{
      "configure grandchild", "configure child", "configure root",
      "activate grandchild", "activate child", "activate root",
      "deactivate root", "deactivate child", "deactivate grandchild",
      "cleanup root", "cleanup child", "cleanup grandchild"}));
}

TYPED_TEST(ComponentRuntimeScenarios, LastRootHandleShutsDownThroughItsSlotWithoutAnOwningCycle)
{
  this->build_root();
  const auto manager = rpp::ComponentRuntimeAccess::owning_slot(*this->component_root);
  ASSERT_TRUE(this->component_root->configure().success);
  ASSERT_TRUE(this->component_root->activate().success);
  this->log.clear();
  this->component_root.reset();
  EXPECT_TRUE(manager.expired());
  EXPECT_EQ(this->log.snapshot(), (std::vector<std::string>{"deactivate root", "cleanup root"}));
}

TYPED_TEST(ComponentRuntimeScenarios, ComponentRootUsesSlotStateChecksAndExplicitFailureRecovery)
{
  this->build_root();
  EXPECT_FALSE(this->component_root->activate().success);
  ASSERT_TRUE(this->component_root->configure().success);
  EXPECT_FALSE(this->component_root->configure().success);
  this->controllers.at("root")->reject_activation = true;
  EXPECT_FALSE(this->component_root->activate().success);
  EXPECT_EQ(rpp::ComponentRuntimeAccess::status(*this->component_root).state,
      rpp::LifecycleState::ERROR);
  EXPECT_FALSE(this->component_root->configure().success);
  ASSERT_TRUE(this->component_root->deactivate().result.success);
  ASSERT_TRUE(this->component_root->cleanup().success);
  this->controllers.at("root")->reject_activation = false;
  ASSERT_TRUE(this->component_root->configure().success);
  ASSERT_TRUE(this->component_root->activate().success);
  EXPECT_EQ(rpp::ComponentRuntimeAccess::status(*this->component_root).state,
      rpp::LifecycleState::ACTIVE);
  ASSERT_TRUE(this->component_root->close().success);
}

TYPED_TEST(ComponentRuntimeScenarios, ActivateCallDeactivateAndReactivate)
{
  this->build();
  EXPECT_TRUE(this->log.snapshot().empty());
  ASSERT_TRUE(this->root->configure().success);
  EXPECT_THROW(this->call("a"), rpp::LifecycleError);
  ASSERT_TRUE(this->root->get_context().activate_subcomponents().success);
  ASSERT_TRUE(this->root->get_context().activate_subcomponents().success);
  EXPECT_EQ(this->call("a"), 0);
  EXPECT_EQ(this->state("b"), rpp::LifecycleState::INACTIVE);
  this->controllers.at("a")->state.value = 42;
  auto stopped = this->root->deactivate_component("controllers", "a");
  ASSERT_TRUE(stopped.result.success);
  ASSERT_TRUE(stopped.state.has_value());
  EXPECT_EQ(stopped.state->template get<ControllerState>().value, 42);
  EXPECT_THROW(this->call("a"), rpp::LifecycleError);
  ASSERT_TRUE(
    this->root->activate_component("controllers", "a", std::move(*stopped.state)).success);
  EXPECT_EQ(this->call("a"), 42);
  ASSERT_TRUE(this->root->close().success);
  EXPECT_EQ(this->log.snapshot(), (std::vector<std::string>{
      "configure a", "configure b", "activate a", "deactivate a", "activate a",
      "deactivate a", "cleanup b", "cleanup a"}));
}

TYPED_TEST(ComponentRuntimeScenarios, ExclusiveSwitchTransfersStateThenColdSwitchResetsState)
{
  this->build();
  ASSERT_TRUE(this->root->configure().success);
  ASSERT_TRUE(this->root->get_context().activate_subcomponents().success);
  this->controllers.at("a")->state.value = 42;
  EXPECT_FALSE(this->root->activate_component("controllers", "b").success);
  this->log.clear();

  ASSERT_TRUE(this->root->switch_component("controllers", "a", "b", true).success);
  EXPECT_EQ(this->state("a"), rpp::LifecycleState::INACTIVE);
  EXPECT_EQ(this->state("b"), rpp::LifecycleState::ACTIVE);
  EXPECT_THROW(this->call("a"), rpp::LifecycleError);
  EXPECT_EQ(this->call("b"), 42);
  this->controllers.at("a")->state.value = 99;
  EXPECT_EQ(this->call("b"), 42);  // Handover is an owned snapshot.
  EXPECT_EQ(this->root->get_context().template get_subcomponent_instance<ScenarioController>(
      "controllers"),
    this->controllers.at("b"));

  ASSERT_TRUE(this->root->switch_component("controllers", "b", "a", false).success);
  EXPECT_EQ(this->call("a"), 0);
  EXPECT_THROW(this->call("b"), rpp::LifecycleError);
  EXPECT_EQ(this->log.snapshot(), (std::vector<std::string>{
      "deactivate a", "activate b", "deactivate b", "activate a"}));
}

TYPED_TEST(ComponentRuntimeScenarios, SourceDeactivationFailurePreventsTargetActivation)
{
  this->build();
  ASSERT_TRUE(this->root->configure().success);
  ASSERT_TRUE(this->root->get_context().activate_subcomponents().success);
  this->controllers.at("a")->reject_deactivation = true;
  this->log.clear();
  EXPECT_FALSE(this->root->switch_component("controllers", "a", "b").success);
  EXPECT_EQ(this->state("a"), rpp::LifecycleState::ERROR);
  EXPECT_EQ(this->state("b"), rpp::LifecycleState::INACTIVE);
  EXPECT_THROW(this->call("a"), rpp::LifecycleError);
  EXPECT_THROW(this->call("b"), rpp::LifecycleError);
  EXPECT_EQ(this->log.snapshot(), (std::vector<std::string>{"deactivate a"}));

  // Recovery is explicit: stop the failed source before activating the target.
  this->controllers.at("a")->reject_deactivation = false;
  ASSERT_TRUE(this->root->deactivate_component("controllers", "a").result.success);
  ASSERT_TRUE(this->root->activate_component("controllers", "b").success);
  EXPECT_EQ(this->call("b"), 0);
}

TYPED_TEST(ComponentRuntimeScenarios,
    TargetActivationFailureLeavesSourceStoppedUntilExplicitRecovery)
{
  this->build();
  ASSERT_TRUE(this->root->configure().success);
  ASSERT_TRUE(this->root->get_context().activate_subcomponents().success);
  this->controllers.at("b")->reject_activation = true;
  this->log.clear();
  EXPECT_FALSE(this->root->switch_component("controllers", "a", "b").success);
  EXPECT_EQ(this->state("a"), rpp::LifecycleState::INACTIVE);
  EXPECT_EQ(this->state("b"), rpp::LifecycleState::ERROR);
  EXPECT_THROW(this->call("a"), rpp::LifecycleError);
  EXPECT_THROW(this->call("b"), rpp::LifecycleError);
  EXPECT_EQ(this->log.snapshot(), (std::vector<std::string>{"deactivate a", "activate b"}));

  this->controllers.at("b")->reject_activation = false;
  ASSERT_TRUE(this->root->deactivate_component("controllers", "b").result.success);
  ASSERT_TRUE(this->root->cleanup_component("controllers", "b").success);
  ASSERT_TRUE(this->root->configure_component("controllers", "b").success);
  ASSERT_TRUE(this->root->activate_component("controllers", "b").success);
  EXPECT_EQ(this->call("b"), 0);
}

TYPED_TEST(ComponentRuntimeScenarios, StaticSlotAllowsLifecycleButRejectsSwitching)
{
  this->build("Option[controllers::Controller]", false);
  ASSERT_TRUE(this->root->configure().success);
  ASSERT_TRUE(this->root->get_context().activate_subcomponents().success);
  this->log.clear();
  EXPECT_FALSE(this->root->switch_component("controllers", "a", "b").success);
  EXPECT_FALSE(this->root->activate_component("controllers", "b").success);
  EXPECT_TRUE(this->log.snapshot().empty());
  EXPECT_EQ(this->call("a"), 0);
  ASSERT_TRUE(this->root->get_context().deactivate_subcomponents().success);
  ASSERT_TRUE(this->root->get_context().activate_subcomponents().success);
  EXPECT_EQ(this->call("a"), 0);
}

TYPED_TEST(ComponentRuntimeScenarios, ListActivatesBothAndShutdownUsesReverseRegistrationOrder)
{
  this->build("List[controllers::Controller]");
  ASSERT_TRUE(this->root->configure().success);
  ASSERT_TRUE(this->root->get_context().activate_subcomponents().success);
  EXPECT_EQ(this->call("a"), 0);
  EXPECT_EQ(this->call("b"), 0);
  ASSERT_TRUE(this->root->deactivate_component("controllers", "a").result.success);
  EXPECT_EQ(this->call("b"), 0);
  ASSERT_TRUE(this->root->activate_component("controllers", "a").success);
  this->log.clear();
  ASSERT_TRUE(this->root->close().success);
  EXPECT_EQ(this->log.snapshot(), (std::vector<std::string>{
      "deactivate b", "deactivate a", "cleanup b", "cleanup a"}));
}

TYPED_TEST(ComponentRuntimeScenarios, NestedTreeConfiguresAndActivatesChildrenBeforeParents)
{
  this->build_nested();
  EXPECT_TRUE(this->log.snapshot().empty());
  ASSERT_TRUE(this->root->configure().success);
  EXPECT_EQ(this->log.snapshot(), (std::vector<std::string>{
      "configure a.left.leaf", "configure a.left", "configure a.right", "configure a",
      "configure b.left.leaf", "configure b.left", "configure b.right", "configure b"}));
  this->log.clear();
  ASSERT_TRUE(this->root->get_context().activate_subcomponents().success);
  EXPECT_EQ(this->log.snapshot(), (std::vector<std::string>{
      "activate a.left.leaf", "activate a.left", "activate a.right", "activate a"}));
  const auto & a = *this->controllers.at("a")->context;
  const auto & left = *this->controllers.at("a.left")->context;
  EXPECT_TRUE(a.is_active());
  EXPECT_TRUE(left.is_active());
  EXPECT_TRUE(this->controllers.at("a.left.leaf")->context->is_active());
  EXPECT_FALSE(this->controllers.at("b")->context->is_active());

  this->log.clear();
  ASSERT_TRUE(this->root->get_context().deactivate_subcomponents().success);
  EXPECT_EQ(this->log.snapshot(), (std::vector<std::string>{
      "deactivate a", "deactivate a.right", "deactivate a.left", "deactivate a.left.leaf"}));
  EXPECT_FALSE(a.is_active());
  EXPECT_FALSE(left.is_active());
  EXPECT_FALSE(this->controllers.at("a.left.leaf")->context->is_active());
  ASSERT_TRUE(this->root->get_context().activate_subcomponents().success);
  EXPECT_TRUE(this->controllers.at("a.left.leaf")->context->is_active());
}

TYPED_TEST(ComponentRuntimeScenarios, NestedSwitchStopsOldDescendantsAndStartsNewDescendants)
{
  this->build_nested();
  ASSERT_TRUE(this->root->configure().success);
  ASSERT_TRUE(this->root->get_context().activate_subcomponents().success);
  this->controllers.at("a")->state.value = 42;
  this->controllers.at("a.left.leaf")->state.value = 7;
  this->log.clear();
  ASSERT_TRUE(this->root->switch_component("controllers", "a", "b", true).success);
  EXPECT_EQ(this->log.snapshot(), (std::vector<std::string>{
      "deactivate a", "deactivate a.right", "deactivate a.left", "deactivate a.left.leaf",
      "activate b.left.leaf", "activate b.left", "activate b.right", "activate b"}));
  EXPECT_EQ(this->call("b"), 42);
  const auto & a_left = *this->controllers.at("a.left")->context;
  const auto & b_left = *this->controllers.at("b.left")->context;
  EXPECT_THROW(a_left.template with_active_component<ScenarioController>(
      "children", "a.left.leaf", [](auto & plugin) {return plugin.state.value;}),
    rpp::LifecycleError);
  EXPECT_EQ(b_left.template with_active_component<ScenarioController>(
      "children", "b.left.leaf", [](auto & plugin) {return plugin.state.value;}), 0);

  // Root handover does not implicitly transfer each descendant's state.
  EXPECT_EQ(this->controllers.at("a.left.leaf")->state.value, 7);
  this->log.clear();
  ASSERT_TRUE(this->root->close().success);
  EXPECT_EQ(this->log.snapshot(), (std::vector<std::string>{
      "deactivate b", "deactivate b.right", "deactivate b.left", "deactivate b.left.leaf",
      "cleanup b", "cleanup b.right", "cleanup b.left", "cleanup b.left.leaf",
      "cleanup a", "cleanup a.right", "cleanup a.left", "cleanup a.left.leaf"}));
}

TYPED_TEST(ComponentRuntimeScenarios, NestedActivationFailureRollsBackThePreviouslyActivatedSubtree)
{
  this->build_nested();
  ASSERT_TRUE(this->root->configure().success);
  this->controllers.at("a.right")->reject_activation = true;
  this->log.clear();
  EXPECT_FALSE(this->root->get_context().activate_subcomponents().success);
  EXPECT_EQ(this->log.snapshot(), (std::vector<std::string>{
      "activate a.left.leaf", "activate a.left", "activate a.right",
      "deactivate a.left", "deactivate a.left.leaf"}));
  EXPECT_EQ(this->state("a"), rpp::LifecycleState::ERROR);
  EXPECT_EQ(this->state("b"), rpp::LifecycleState::INACTIVE);
  const auto & a = rpp::ComponentRuntimeAccess::registered_component(*this->root, "controllers",
      "a");
  const auto children = a.get_subcomponent_statuses("children");
  ASSERT_EQ(children.size(), 2u);
  EXPECT_EQ(children[0].state, rpp::LifecycleState::INACTIVE);
  EXPECT_EQ(children[1].state, rpp::LifecycleState::ERROR);
  EXPECT_FALSE(this->controllers.at("a.left.leaf")->context->is_active());

  // A failed callback can retain resources. Explicit shutdown must reach that
  // child as well as its already-rolled-back sibling and grandchild.
  this->controllers.at("a.right")->reject_activation = false;
  this->log.clear();
  ASSERT_TRUE(this->root->close().success);
  EXPECT_EQ(this->log.snapshot(), (std::vector<std::string>{
      "deactivate a", "deactivate a.right",
      "cleanup b", "cleanup b.right", "cleanup b.left", "cleanup b.left.leaf",
      "cleanup a", "cleanup a.right", "cleanup a.left", "cleanup a.left.leaf"}));
}

using ConcurrentRuntimeScenarios = ComponentRuntimeScenarios<rpp::Concurrent>;

TEST_F(ConcurrentRuntimeScenarios, SwitchClosesAdmissionAndWaitsForTheGuardedCall)
{
  build();
  ASSERT_TRUE(root->configure().success);
  ASSERT_TRUE(root->get_context().activate_subcomponents().success);
  log.clear();
  std::promise<void> entered;
  auto ready = entered.get_future();
  auto worker = std::async(std::launch::async, [&] {
        return root->get_context().with_active_component<ScenarioController>("controllers", "a",
               [&](auto &) {
                 log.add("call entered");
                 entered.set_value();
      // Only snapshot metadata from this worker. Lifecycle stays on the test thread.
                 const auto deadline = std::chrono::steady_clock::now() + 2s;
                 while (std::chrono::steady_clock::now() < deadline) {
                   const auto entries = root->get_subcomponent_statuses("controllers");
                   if (entries.front().transition.has_value()) {
                     bool rejected = false;
                     try {
                       call("a");
                     } catch (const rpp::LifecycleError &) {
                       rejected = true;
                     }
                     log.add("call finished");
                     return rejected;
                   }
                   std::this_thread::yield();
                 }
                 log.add("call timed out");
                 return false;
    });
  });
  ASSERT_EQ(ready.wait_for(2s), std::future_status::ready);
  const auto result = root->switch_component("controllers", "a", "b", true, 3s);
  EXPECT_TRUE(worker.get());
  ASSERT_TRUE(result.success) << result.message;
  EXPECT_EQ(log.snapshot(), (std::vector<std::string>{
      "call entered", "call finished", "deactivate a", "activate b"}));
  EXPECT_EQ(state("a"), rpp::LifecycleState::INACTIVE);
  EXPECT_EQ(call("b"), 0);
}

TEST_F(ConcurrentRuntimeScenarios, DeactivationTimeoutKeepsInstanceAndAllowsExplicitRetry)
{
  build();
  ASSERT_TRUE(root->configure().success);
  ASSERT_TRUE(root->get_context().activate_subcomponents().success);
  const auto identity = root->get_subcomponent_statuses("controllers").front().instance_id;
  log.clear();
  std::promise<void> entered, release;
  auto ready = entered.get_future();
  auto released = release.get_future();
  auto worker = std::async(std::launch::async, [&] {
        return root->get_context().with_active_component<ScenarioController>("controllers", "a",
               [&](auto &) {
                 entered.set_value();
                 return released.wait_for(3s) == std::future_status::ready;
    });
  });
  const auto observed = ready.wait_for(2s);
  if (observed != std::future_status::ready) {release.set_value();}
  ASSERT_EQ(observed, std::future_status::ready);
  EXPECT_FALSE(root->deactivate_component("controllers", "a", 0ms).result.success);
  EXPECT_EQ(state("a"), rpp::LifecycleState::ERROR);
  EXPECT_EQ(root->get_subcomponent_statuses("controllers").front().instance_id, identity);
  EXPECT_TRUE(log.snapshot().empty());  // No callback while the guard is held.
  EXPECT_THROW(call("a"), rpp::LifecycleError);
  release.set_value();
  EXPECT_TRUE(worker.get());
  ASSERT_TRUE(root->deactivate_component("controllers", "a", 1s).result.success);
  EXPECT_EQ(state("a"), rpp::LifecycleState::INACTIVE);
  ASSERT_TRUE(root->activate_component("controllers", "a").success);
  EXPECT_EQ(call("a"), 0);
  EXPECT_EQ(root->get_subcomponent_statuses("controllers").front().instance_id, identity);
}

TEST_F(ConcurrentRuntimeScenarios, SwitchTimeoutDoesNotActivateTargetAndRecoveryPreservesState)
{
  build();
  ASSERT_TRUE(root->configure().success);
  ASSERT_TRUE(root->get_context().activate_subcomponents().success);
  controllers.at("a")->state.value = 42;
  log.clear();
  std::promise<void> entered, release;
  auto ready = entered.get_future();
  auto released = release.get_future();
  auto worker = std::async(std::launch::async, [&] {
        return root->get_context().with_active_component<ScenarioController>("controllers", "a",
               [&](auto &) {
                 entered.set_value();
                 return released.wait_for(3s) == std::future_status::ready;
    });
  });
  const auto observed = ready.wait_for(2s);
  if (observed != std::future_status::ready) {release.set_value();}
  ASSERT_EQ(observed, std::future_status::ready);
  EXPECT_FALSE(root->switch_component("controllers", "a", "b", true, 0ms).success);
  EXPECT_EQ(state("a"), rpp::LifecycleState::ERROR);
  EXPECT_EQ(state("b"), rpp::LifecycleState::INACTIVE);
  EXPECT_TRUE(log.snapshot().empty());
  EXPECT_THROW(call("a"), rpp::LifecycleError);
  EXPECT_THROW(call("b"), rpp::LifecycleError);
  release.set_value();
  EXPECT_TRUE(worker.get());

  // A timed-out source requires explicit lifecycle recovery before another switch.
  auto stopped = root->deactivate_component("controllers", "a", 1s);
  ASSERT_TRUE(stopped.result.success);
  ASSERT_TRUE(stopped.state.has_value());
  ASSERT_TRUE(root->activate_component("controllers", "a", std::move(*stopped.state)).success);
  ASSERT_TRUE(root->switch_component("controllers", "a", "b", true, 1s).success);
  EXPECT_EQ(call("b"), 42);
  EXPECT_EQ(log.snapshot(), (std::vector<std::string>{
      "deactivate a", "activate a", "deactivate a", "activate b"}));
}
}  // namespace
