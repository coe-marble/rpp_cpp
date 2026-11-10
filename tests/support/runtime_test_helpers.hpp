#pragma once

#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "rpp_cpp/runtime.hpp"
#include "fake_plugins.hpp"

namespace rpp
{
// The one test-only bridge to private construction. Policies, admission and
// lifecycle behavior always come from the production builder/manager/runtime.
class ComponentRuntimeAccess
{
  template<class Threading>
  using Runtime = rpp::ComponentRuntime<Threading::concurrent>;
  template<class Threading>
  using Script = rpp::ScriptRuntime<Threading::concurrent>;
  template<class Threading>
  using Manager = BasicSlotManager<Threading>;
  template<class Threading>
  using Constructor = ComponentConstructor<Threading>;

public:
  template<class Threading = SingleThreaded>
  static typename Constructor<Threading>::SlotManagers assemble_test_slots(
    std::map<std::string, std::vector<Runtime<Threading>>> children,
    const std::map<std::string, std::string> & spec = {},
    const typename Manager<Threading>::RuntimeFactory & factory = {},
    const typename Manager<Threading>::DynamicComponentPolicy & policy = {},
    const typename Manager<Threading>::RegistrationValidators & named = {},
    const std::map<std::string, typename Manager<Threading>::Validator> & validators = {})
  {
    auto options = std::make_shared<typename Constructor<Threading>::BuildOptions>();
    options->dynamic_policy = policy;
    options->validators = named;
    typename Constructor<Threading>::SlotBuildRequests requests;
    auto pending = std::make_shared<std::map<std::string, std::shared_ptr<Runtime<Threading>>>>();
    for (auto & [slot, entries] : children) {
      auto & initial = requests[slot];
      for (auto & runtime : entries) {
        // Synthetic fixture paths let the one factory serve initial and dynamic
        // construction, without exposing a second initialization/factory API.
        const auto & name = runtime.component_data().component_name_;
        const auto path = slot + "/" + name;
        initial.push_back({name, path, path, "", {}});
        pending->emplace(path, std::make_shared<Runtime<Threading>>(std::move(runtime)));
      }
    }
    typename Manager<Threading>::RuntimeFactory construct =
      [pending, factory](const std::string & path) {
        auto found = pending->find(path);
        if (found != pending->end()) {
          auto runtime = std::move(*found->second);
          pending->erase(found);
          return runtime;
        }
        if (!factory) {throw LifecycleError("No fake component registered for: " + path);}
        return factory(path);
      };
    return Constructor<Threading>::create_slots(
      requests, spec, options, {}, std::move(construct), validators);
  }

  template<class Threading = SingleThreaded>
  static Runtime<Threading> make(
    std::shared_ptr<Plugin> instance, std::vector<Runtime<Threading>> children = {})
  {
    for (size_t i = 0; i < children.size(); ++i) {
      auto & child = children[i];
      auto & name = child.component_data().component_name_;
      if (name.empty()) {name = "test_child_" + std::to_string(i);}
    }
    std::map<std::string, std::vector<Runtime<Threading>>> slots;
    slots.emplace("children", std::move(children));
    auto data = std::make_unique<detail::ComponentData<Threading>>(
      std::move(instance), params::Parameters{}, nullptr,
      assemble_test_slots<Threading>(std::move(slots)), ClockOptions{}, true);
    return Manager<Threading>::create_root(std::move(data));
  }

  template<class Threading = SingleThreaded>
  static Runtime<Threading> plugin(std::shared_ptr<Plugin> instance)
  {
    return Manager<Threading>::create_root(std::make_unique<detail::ComponentData<Threading>>(
      std::move(instance), params::Parameters{}, nullptr,
      typename Constructor<Threading>::SlotManagers{}, ClockOptions{}, true));
  }

  template<class Threading = SingleThreaded>
  static Runtime<Threading> named(
    std::shared_ptr<Plugin> instance, const std::string & name,
    std::vector<Runtime<Threading>> children = {},
    std::string plugin_type = "controllers::Controller")
  {
    auto runtime = make<Threading>(std::move(instance), std::move(children));
    runtime.component_data().component_name_ = name;
    runtime.component_data().plugin_type_ = std::move(plugin_type);
    return runtime;
  }

  template<class Threading = SingleThreaded>
  static Script<Threading> script(
    std::vector<Runtime<Threading>> children, const std::string & declaration,
    bool dynamic = false, typename Manager<Threading>::RuntimeFactory factory = {},
    typename Runtime<Threading>::RegistrationValidator validator = {})
  {
    std::map<std::string, std::vector<Runtime<Threading>>> slots;
    slots.emplace("controllers", std::move(children));
    auto managers = assemble_test_slots<Threading>(
      std::move(slots), {{"controllers", declaration}}, factory,
      [dynamic](const std::string &, const ComponentSlot &) {return dynamic;},
      {}, {{"controllers", std::move(validator)}});
    return Script<Threading>(std::make_unique<detail::ComponentData<Threading>>(
      nullptr, params::Parameters{}, nullptr, std::move(managers), ClockOptions{}, false));
  }

  template<class Threading = SingleThreaded>
  static Script<Threading> policy_slots(
    const std::map<std::string, std::string> & spec,
    typename Manager<Threading>::DynamicComponentPolicy policy,
    typename Manager<Threading>::RuntimeFactory factory,
    const std::map<std::string, typename Manager<Threading>::NamedValidators> & validators = {})
  {
    auto managers = assemble_test_slots<Threading>({}, spec, factory, policy, validators);
    return Script<Threading>(std::make_unique<detail::ComponentData<Threading>>(
      nullptr, params::Parameters{}, nullptr, std::move(managers), ClockOptions{}, false));
  }

  static ScriptRuntime<> with_logger_and_parameters(
    std::shared_ptr<RppLogger> logger,
    std::map<std::string, params::ParameterValue> parameters)
  {
    auto runtime = Script<SingleThreaded>(std::make_unique<detail::ComponentData<SingleThreaded>>(
      nullptr, params::Parameters{}, nullptr,
      Constructor<SingleThreaded>::SlotManagers{}, ClockOptions{}, false));
    runtime.component_data().logger_ = std::move(logger);
    runtime.component_data().parameters_ = params::Parameters(std::move(parameters));
    return runtime;
  }

  template<class Threading = SingleThreaded>
  static Script<Threading> script_slots(std::vector<Runtime<Threading>> components = {})
  {
    for (size_t i = 0; i < components.size(); ++i) {
      auto & name = components[i].component_data().component_name_;
      if (name.empty()) {name = "test_child_" + std::to_string(i);}
    }
    std::map<std::string, std::vector<Runtime<Threading>>> assignments;
    assignments.emplace("children", std::move(components));
    return Script<Threading>(std::make_unique<detail::ComponentData<Threading>>(
      nullptr, params::Parameters{}, nullptr,
      assemble_test_slots<Threading>(std::move(assignments)), ClockOptions{}, false));
  }

  // Test nested administration through production slot lookup without exposing it to plugins.
  template<bool Concurrent, bool IsScript>
  static auto registered_component(const BasicRuntime<Concurrent, IsScript> & runtime,
    const std::string & slot_name, const std::string & component_name)
  {
    auto handle = runtime.slot(slot_name).runtime(component_name);
    if (!handle) {throw LifecycleError("Subcomponent is not loaded: " + component_name);}
    return std::move(*handle);
  }

  template<bool Concurrent, bool IsScript>
  static auto registered_components(const BasicRuntime<Concurrent, IsScript> & runtime,
    const std::string & slot_name)
  {return runtime.slot(slot_name).runtimes();}

  template<bool Concurrent>
  static auto owning_slot(const ComponentRuntime<Concurrent> & runtime)
  {return std::weak_ptr<typename ComponentRuntime<Concurrent>::SlotManager>(runtime.storage_.slot);}

  template<bool Concurrent>
  static ComponentStatus status(const ComponentRuntime<Concurrent> & runtime)
  {return runtime.storage_.slot->get_status(runtime.storage_.name);}

  template<bool Concurrent>
  static bool has_external_component_process(const ComponentRuntime<Concurrent> & runtime)
  {return static_cast<bool>(runtime.component_data().external_component_process_);}

  template<bool Concurrent, bool IsScript>
  static auto slot_manager(
    const BasicRuntime<Concurrent, IsScript> & runtime, const std::string & slot_name)
  {return runtime.component_data().subcomponent_slots_.at(slot_name);}


};
}  // namespace rpp

namespace rpp::testing
{
using ScenarioControllers = std::map<std::string, std::shared_ptr<ScenarioController>>;

template<class Threading>
rpp::ScriptRuntime<Threading::concurrent> simple_controllers(
  EventLog & log, ScenarioControllers & instances,
  const std::string & declaration = "Option[controllers::Controller]", bool dynamic = true)
{
  using Runtime = rpp::ComponentRuntime<Threading::concurrent>;
  typename BasicSlotManager<Threading>::RuntimeFactory factory =
    [&log, &instances](const std::string & name) {
      auto instance = std::make_shared<ScenarioController>(name, log);
      instances[name] = instance;
      return ComponentRuntimeAccess::named<Threading>(std::move(instance), name);
    };
  std::vector<Runtime> children;
  for (const auto * name : {"a", "b"}) {children.push_back(factory(name));}
  return ComponentRuntimeAccess::script<Threading>(
    std::move(children), declaration, dynamic, factory);
}

// Declarative fake trees keep construction noise out of scenario tests.
// Internal nodes are composites; leaves are simple stateful controllers.
struct FakeComponentTree
{
  std::string name;
  std::vector<FakeComponentTree> children;
};

template<class Threading>
rpp::ComponentRuntime<Threading::concurrent> controller_tree(
  const FakeComponentTree & tree, EventLog & log, ScenarioControllers & instances)
{
  std::vector<rpp::ComponentRuntime<Threading::concurrent>> children;
  for (const auto & child : tree.children) {
    children.push_back(controller_tree<Threading>(child, log, instances));
  }
  std::shared_ptr<ScenarioController> instance;
  if (tree.children.empty()) {instance = std::make_shared<ScenarioController>(tree.name, log);} else {
    instance = std::make_shared<CompositeController>(tree.name, log);
  }
  instances[tree.name] = instance;
  return ComponentRuntimeAccess::named<Threading>(
    std::move(instance), tree.name, std::move(children));
}

template<class Threading>
rpp::ScriptRuntime<Threading::concurrent> nested_controllers(EventLog & log, ScenarioControllers & instances)
{
  // script.controllers (exclusive)
  //   a -> children (list): a.left -> a.left.leaf, a.right
  //   b -> children (list): b.left -> b.left.leaf, b.right
  std::vector<rpp::ComponentRuntime<Threading::concurrent>> branches;
  for (const auto * name : {"a", "b"}) {
    const std::string parent = name;
    branches.push_back(controller_tree<Threading>(
        {parent, {{parent + ".left", {{parent + ".left.leaf", {}}}},
          {parent + ".right", {}}}}, log, instances));
  }
  return ComponentRuntimeAccess::script<Threading>(
    std::move(branches), "Option[controllers::Controller]", true);
}

template<class Threading>
rpp::ComponentRuntime<Threading::concurrent> component_root(
  const FakeComponentTree & tree, EventLog & log, ScenarioControllers & instances)
{
  return controller_tree<Threading>(tree, log, instances);
}

inline ComponentRuntime<> controller(const std::string & name, std::set<std::string> axes = {})
{
  return ComponentRuntimeAccess::named(std::make_shared<AxisController>(std::move(axes)), name);
}

inline std::vector<ComponentRuntime<>> controllers(
  std::initializer_list<std::pair<std::string, std::set<std::string>>> definitions)
{
  std::vector<ComponentRuntime<>> result;
  for (const auto & [name, axes] : definitions) {result.push_back(controller(name, axes));}
  return result;
}

inline CallbackResult no_axis_overlap(
  const ComponentRuntime<> & candidate,
  const std::vector<std::shared_ptr<const ComponentRuntime<>>> & registered)
{
  const auto incoming = candidate.get_instance<AxisController>();
  for (const auto & existing : registered) {
    for (const auto & axis : existing->get_instance<AxisController>()->axes) {
      if (incoming->axes.count(axis)) {
        return CallbackResult::failure("Control axis already registered: " + axis);
      }
    }
  }
  return {};
}

inline ScriptRuntime<> managed_recorder(
  std::shared_ptr<Recorder> instance, SlotManager::RuntimeFactory factory = {})
{
  std::vector<ComponentRuntime<>> children;
  children.push_back(ComponentRuntimeAccess::named(std::move(instance), "plugin"));
  return ComponentRuntimeAccess::script(
    std::move(children), "controllers::Controller", true, std::move(factory));
}
}  // namespace rpp::testing
