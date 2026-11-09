# rpp_cpp

`rpp_cpp` provides the C++ plugin runtime and the C++ component server.

See [C++ context construction and lifecycle](docs/context-flow.md) for the
class responsibilities, call flow, ownership, and runtime operations.

Start a behavior review with
[the component runtime scenarios](tests/test_component_runtime_scenarios.cpp).
The same scenarios run for `SingleThreaded` and `Concurrent`: activation,
deactivation/reactivation, switching with and without state handover, source and
target failure recovery, static slots, multiple-active slots, and shutdown order.
Concurrent-only scenarios cover closing admission while draining guarded calls
and retrying after a drain timeout. Lifecycle operations stay on one owner thread.
These are in-process integration tests of runtime, managers, and lifecycle plugins;
their small construction bridge uses production slot creation and registration.
They do not cover description loading, shared-library loading, or external hosts.

Shared test support lives in
[`fake_plugins.hpp`](tests/support/fake_plugins.hpp) (plugin behavior, failure
controls, and event recording) and
[`runtime_test_helpers.hpp`](tests/support/runtime_test_helpers.hpp) (the single
private-construction bridge and reusable fixtures). `simple_controllers()` keeps
the two-controller case; `nested_controllers()` builds two switchable branches:

```text
script.controllers (exclusive)
├── a
│   ├── a.left
│   │   └── a.left.leaf
│   └── a.right
└── b
    ├── b.left
    │   └── b.left.leaf
    └── b.right
```

Composite plugins explicitly activate/deactivate their subcomponents. Nested scenarios
check callback order, branch switching, independent descendant state, rollback
after partial activation, and cleanup after failure under both threading policies.
`FakeComponentTree` can describe additional shapes without adding local fake classes.

The RPC control layer is separate: `RpcControlService` and `RpcControlClient` in
`rpc_control_service.hpp` expose remote configuration/cleanup and adapter discovery.
[Its tests](tests/test_rpc_control_service.cpp) use the `RpcControlService` suite.
The shared generated `plugin_runtime.capnp` protocol still uses the wire interface
name `PluginRuntime`, which is also consumed by Python.

It can be configured as a regular CMake project with Cap'n Proto **1.1.0**,
`nlohmann_json`, pybind11, Python development headers, and either `rclcpp` or
`spdlog`. The generated Cap'n Proto interfaces are version-locked, so a newer
or older Cap'n Proto release is deliberately rejected by CMake.

```bash
cmake -S . -B build
cmake --build build
```

When `ament_cmake` is available, the package also supports the normal ROS 2
and `colcon` workflow. ROS 2 logging is selected only when CMake finds
`rclcpp`; otherwise the runtime uses `spdlog`.

## Runtime, plugin context, and managed slots

`SlotManager` is the only manager type. Each single/option/list slot owns one.
A root component lives in a fixed single slot; a script directly owns its slot
collection and has no coordinating manager or artificial component entry.

`ComponentRuntimeBuilder` uses `ComponentConstructor` to resolve descriptions,
prepare external transports, load parameters and instances, and assemble slots.
It admits each component root to a fixed slot. Slot managers retain construction
settings for later loads and reloads, independently of the builder's lifetime.

`ComponentRuntime` exposes application lifecycle and administration commands.
`get_context()` returns the concrete, non-template `ComponentContext` for services
and subcomponent access. The context refers non-owningly to its component data;
a component entry records its owning slot and registration name for lifecycle calls.
`SubcomponentContext` exposes control, identity and instance access for one subcomponent.

A plugin cannot load, unload or switch itself in its owning slot. Its context only
exposes its own services, deferred self-deactivation and its subcomponent slots.
Membership changes belong to the parent/application controlling that slot.
Moving a runtime preserves its component data and context addresses.

Build a runtime, then use its accessors and slot operations. Managers stay
internal to the runtime:

```cpp
rpp::ComponentRuntimeBuilder builder(data_manager, rpp::ClockOptions{}, true);
auto runtime = builder.build_script_from_path(script_path);
runtime.configure().require_success();
runtime.get_context().activate_subcomponents().require_success();
auto controller = runtime.get_context().get_subcomponent<Controller>("controller");
// For List[T] slots, the component name is always required:
// auto front = runtime.get_context().get_subcomponent<Camera>("cameras", "front");
// Use the controller from the application's normal loop.
runtime.close().require_success();
```

The third constructor argument is `allow_dynamic_components` (default false).
It controls the slot managers created by this builder. False allows
initial setup, normal lifecycle, and whole-tree shutdown, but rejects runtime
load/unload, replacement, and switching to another exclusive candidate.
All slots use internal managers in both modes; native use creates no RPC worker
or event loop. Single and `Option[T]` slots share exclusive activation, while
`List[T]` permits several active entries.
Managers retain their constructor helper and build settings after the facade is
destroyed; later loads never create another builder. The helper's `create_slots()`
declares and initializes slots in one operation. External-component proxies have no local subcomponent managers: the
external host owns the real subcomponent tree.

Alternatively, pass a per-slot policy directly after the data manager:

```cpp
rpp::ComponentRuntimeBuilder builder(data_manager,
  [](const std::string & slot_name, const rpp::ComponentSlot & slot_type) {
    return slot_name == "controllers" && slot_type.kind == rpp::SlotKind::OPTION;
  });
```

The boolean and callback constructors are alternatives. The callback receives
each slot's name and parsed type once during construction, including empty slots.
Its result stays fixed for that slot; newly loaded descendants use the same policy.
`runtime.allows_dynamic_components()` queries whether this instance can be replaced
in its owning slot; fixed roots and script collections return false. The overload taking `slot_name`
queries a specific subcomponent slot. Permissions are independent: direct subcomponent switching
requires that subcomponent slot's permission. Unloading an allowed parent tears down its
whole subtree even if subcomponent switching is disabled; ordinary parent activation
and deactivation leave subcomponent transitions to the plugin callbacks.
The decision is local to each manager, regardless of its components' source language.
Neither the callback nor the boolean is forwarded to external hosts by the builder.

`runtime.get_context()` returns the manager-owned `ComponentContext`. Parameters,
logger, clock, component identity, active state, subcomponent access and guarded
calls belong to that view. `get_subcomponent_context(slot, name)` returns control
of one named subcomponent; its plural form returns shared views for a slot.
Runtime retains lifecycle and application administration: `load_component`,
`configure_component`, `activate_component`, `deactivate_component`,
`cleanup_component`, `unload_component`, `switch_component`, and `reload_component`.

Borrow with `runtime.get_context().with_active_component<T>(slot, name, callback)`
when application calls can overlap transitions. Stale context views cannot operate
on replacement instances under the same registration name.

Registration validators take a candidate runtime handle and a snapshot of existing
const runtime handles, including inactive entries, and return `CallbackResult`.
Inspect services and parameters through each handle's `get_context()`.
Validators inspect parameters/metadata and must not execute lifecycle operations.
Supply initial validators with `ComponentRuntimeBuilder::RegistrationValidators`,
keyed by slot plugin type and then the name of the component being registered.
For example, `validators["controllers::Controller"]["surge_controller"]` applies
to that candidate in any slot of this type, including `List` and `Option` slots.
Initial names use the workspace component name; empty names are rejected on
registration. Dynamic loads use the supplied registration name. Missing rules
allow normal type checks.
Pass validators after the clock/permission arguments in the chosen constructor.
`runtime.set_registration_validator(slot, validator)` changes future admission
without evicting existing entries. Rejection leaves other entries untouched.
Adapted configure/cleanup is supported; adapted activation/deactivation and
typed state handover remain pending.

Plugins configure resources in `on_configure(const ComponentContext &)` and
release them in `on_cleanup()`. Both callbacks return `rpp::CallbackResult`;
the default implementations succeed without doing work. `runtime.configure()`
configures subcomponents before their parent and stops on failure. `runtime.cleanup()`
visits the parent before subcomponents in reverse order and continues after errors,
returning the first failure. Check results with `require_success()`; an optional
label can add caller-specific context.
Adapted instances run these callbacks in their hosting process. Cleanup must be
safe after partially completed configuration. The old `initialize()` API has
been removed; regenerate interfaces and rebuild plugins together.

`runtime.activate(state)` and `runtime.deactivate()` affect only this plugin.
Omit the state for cold activation. The manager marks the entry ACTIVE only after
`on_activate()` succeeds. It drains guarded calls before `on_deactivate()` and
marks the entry INACTIVE only after that callback succeeds. Normal parent
transitions do not change subcomponent states automatically.

Plugins may opt into subcomponent lifecycle from their callbacks. For example, a
`LifecyclePlugin<void>` that saved `context_` in `on_configure()` can use:

```cpp
rpp::CallbackResult on_activate() override
{
  return context_->activate_subcomponents();
}

rpp::CallbackResult on_deactivate() override
{
  return context_->deactivate_subcomponents();
}
```

The plugin chooses where to call these helpers relative to its own initialization
or shutdown. `ComponentContext::activate_subcomponents()` activates selected exclusive candidates or
all list entries, without handover state. If this helper fails, it rolls back only
entries newly activated by that helper. A later failure in the parent's callback
does not automatically undo a successful helper call; the plugin controls recovery.
`ComponentContext::deactivate_subcomponents()` visits active entries in reverse order, discards subcomponent
snapshots, continues after errors, and returns the first failure. Each subcomponent still
decides whether to manage its own descendants.

Script contexts have no plugin callbacks. Applications explicitly call
`activate_subcomponents()` and `deactivate_subcomponents()` on them. `close()` and unload
remain explicit full-tree teardown operations: they stop remaining active
descendants before cleanup and release, even if normal callbacks left them running.
Configure/cleanup propagation is unchanged. Adapted activation/deactivation remains
unsupported until runtime lifecycle control is implemented.

## Component instance identity

Each component has an opaque UUID `instance_id`, available through
`runtime.get_context().get_instance_id()` or its plugin context, and in
`runtime.get_subcomponent_statuses(slot)` metadata. This is
the identity of the loaded instance, separate from the configured component path
and the slot-local registration name. Configure, activate, deactivate, cleanup,
and moves retain the same ID; reload creates a new one. Unloaded entries have an
empty ID. Multiple registrations loading the same component path get different
IDs, while registration names remain unique within each slot.

The optional `expected_instance_id` argument to `with_active_component()` rejects
a replacement under the same name. It borrows a string view for the synchronous
call instead of copying the ID into every guard. Omit it to use the currently
loaded instance under that name.

## Selecting a threading mode

Choose the builder alias in your application. Both aliases use one templated implementation:

```cpp
// Default: all framework access stays on one owning thread.
rpp::ComponentRuntimeBuilder builder(data_manager);
auto runtime = builder.build_component_from_path(component_path);

// One lifecycle thread, with concurrent guarded slot calls.
rpp::ConcurrentComponentRuntimeBuilder concurrent_builder(data_manager);
auto concurrent_runtime = concurrent_builder.build_component_from_path(component_path);
```

Component and script roots use distinct runtime aliases:

```cpp
rpp::ComponentRuntime<>              // single-threaded component root
rpp::ComponentRuntime<true>          // concurrent component root
rpp::ScriptRuntime<>                 // single-threaded script slot collection
rpp::ScriptRuntime<true>             // concurrent script slot collection
```

They share `BasicRuntime<Concurrent, IsScript>` internally. `IsScript` selects
the storage model at compile time: component roots retain their owning fixed-slot
handle, while scripts retain only their services and slot collection. Builders and
slot managers use the corresponding threading policy.
Initial subcomponents and later dynamically loaded/reloaded descendants retain that policy.
The choice applies locally; an external language host selects its own runtime policy.

Single-threaded mode has no mutexes, atomics, condition variables, thread IDs/maps,
or concurrent-call counters. Guards retain a local nesting depth to reject unsafe
reentrant lifecycle changes. Empty lock guards compile away in optimized builds.
Existing shared ownership and context variant dispatch are independent of this policy.

In both modes, one owning thread performs lifecycle and membership operations,
including switching. Concurrent mode permits worker threads to use guarded calls,
read slot snapshots, or submit `request_deactivation()`. It does not permit
competing lifecycle threads; there is no thread-ID tracking or owner-thread detection.

Each concurrent slot has one mutex and condition variable. The gate protects registry
snapshots, guarded-call admission, and per-entry call counts. Deactivation closes
admission and waits for those counts to reach zero. Waiting releases the mutex;
plugin callbacks and component construction also execute with the gate unlocked. There are no
runtime lifecycle locks, manager mutation locks, or per-entry mutexes.

Direct plugin pointers remain callable while inactive. The plugin owns its state,
worker shutdown, resource synchronization, and method behavior after lifecycle
notifications. The slot gate neither intercepts direct calls nor drains them.

Synchronous lifecycle operations on an affected slot must run outside its guarded
callbacks. Concurrent mode does not identify or detect self-waits; self-deactivation
uses `request_deactivation()` and the owner processes it at a safe point instead.
A guard's manager must remain alive until the callback returns. Builder calls, moves,
destruction, and borrowed context access across release require lifetime coordination.

## Subcomponent control and self-deactivation

```cpp
const auto & camera = context_->get_subcomponent_context("cameras", "front");
if (!camera.is_active()) {
  camera.activate().require_success();
}
auto instance = camera.get_instance<Camera>();
auto stopped = camera.deactivate();
stopped.result.require_success();
```

The accessor returns `SubcomponentContext`, which exposes only `activate()`,
`deactivate()`, `is_active()`, `get_instance_id()`, and `get_instance<T>()` for that one subcomponent. It provides
no access to the subcomponent's parameters, services, or descendants. Its lifecycle calls
are synchronous and return the actual outcome. The manager validates transitions;
`is_active()` is only an observation. In concurrent mode, subcomponent
deactivation closes admission and waits for existing guarded calls to finish.

The plugin's own `ComponentContext` exposes services and `is_active()`, but has no
synchronous self-activation/deactivation methods. Instead a plugin can record its
intent to deactivate without stopping itself while its callback is executing:

```cpp
// Inside a plugin callback:
context_->request_deactivation().require_success();
```

This sets one pending flag in manager-owned component data. Success means recorded, not deactivated.
The application processes the tree at a safe point on its lifecycle thread:

```cpp
runtime.get_context().with_active_component<Controller>("controller", "main", [](auto & plugin) {
  plugin.update(); // May call request_deactivation().
});
runtime.process_deactivations().require_success();
```

Processing performs normal synchronous deactivation. Concurrent mode waits for other
guarded calls to finish; timeouts or failures keep the flag set for retry. Repeated
self-deactivation calls coalesce. Direct plugin calls need the same explicit safe
point because runtime cannot observe their completion. No background processor is
created, and guard destructors never execute lifecycle callbacks.

Both context types delegate directly to managers. The self context provides services,
subcomponent access, and deferred self-deactivation. The subcomponent context provides lifecycle
control and identity for one subcomponent. Managers own lifecycle state and processing.

Runtime owns its self context; each manager entry owns its subcomponent-control view.
Plugins borrow their self context through cleanup. Root moves attach the same self
view to the destination and transfer subcomponent-slot managers with their views unchanged.
Retained subcomponent views keep only the self view alive and reject access after runtime
release; reloading creates a new view. Old runtime references cannot transition a
replacement registered under the same name.
