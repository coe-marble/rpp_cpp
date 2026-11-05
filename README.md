# rpp_cpp

`rpp_cpp` provides the C++ plugin runtime and the C++ component server.

See [C++ context construction and lifecycle](docs/context-flow.md) for the
class responsibilities, call flow, ownership, and runtime operations.

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

## Contexts and managed slots

Build a context, then use its accessors and slot operations. Managers stay
internal to the context:

```cpp
rpp::ComponentContextBuilder builder(data_manager, rpp::ClockOptions{}, true);
auto context = builder.build_script_from_path(script_path);
context.configure().require_success();
context.activate_components().require_success();
auto controller = context.get_component<Controller>("controller");
// Use the controller from the application's normal loop.
context.close().require_success();
```

The third constructor argument is `allow_dynamic_components` (default false).
It controls the slot managers created by this builder. False allows
initial setup, normal lifecycle, and whole-tree shutdown, but rejects runtime
load/unload, replacement, and switching to another exclusive candidate.
All slots use internal managers in both modes; native use creates no RPC worker
or event loop. Single and `Option[T]` slots share exclusive activation, while
`List[T]` permits several active entries.
The builder creates and populates these managers, gives them their loading
factories, then moves the manager map into the context. Context construction
accepts assembled managers and does not handle factories or build policies.

Alternatively, pass a per-slot policy directly after the data manager:

```cpp
rpp::ComponentContextBuilder builder(data_manager,
  [](const std::string & slot_name, const rpp::ComponentSlot & slot_type) {
    return slot_name == "controllers" && slot_type.kind == rpp::SlotKind::OPTION;
  });
```

The boolean and callback constructors are alternatives. The callback receives
each slot's name and parsed type once during construction, including empty slots.
Its result stays fixed for that slot; newly loaded descendants use the same policy.
`context.allows_dynamic_components()` queries whether this instance can be replaced
in its owning slot; unmanaged roots return false. The overload taking `slot_name`
queries a specific child slot. Permissions are independent: direct child switching
requires that child slot's permission. Unloading an allowed parent tears down its
whole subtree even if child switching is disabled; ordinary parent activation
and deactivation leave child transitions to the plugin callbacks.
The decision is local to each manager, regardless of its components' source language.
Neither the callback nor the boolean is forwarded to external hosts by the builder.

`get_subcomponent_context(slot)` returns the selected context;
`get_subcomponent_contexts(slot)` returns a snapshot of shared const context
pointers. The singular accessor also accepts an entry name. Parameters and
component access stay on the context. Dynamic slot methods include
`load_component`, `configure_component`, `activate_component`,
`deactivate_component`, `cleanup_component`, `unload_component`,
`switch_component`, and `reload_component`.

Borrow with `with_active_component<T>(slot, name, callback)` when application
calls can overlap transitions. Do not retain unguarded component references
across unloading/replacement. Stale context references cannot operate on a
replacement generation under the same name.

Registration validators take the candidate context and a snapshot of existing
const contexts, including inactive entries, and return `CallbackResult`.
Validators inspect parameters/metadata and must not execute lifecycle operations.
Supply initial validators with `ComponentContextBuilder::RegistrationValidators`,
keyed by slot plugin type and then the name of the component being registered.
For example, `validators["controllers::Controller"]["surge_controller"]` applies
to that candidate in any slot of this type, including `List` and `Option` slots.
Initial names use the workspace component name; empty names are rejected on
registration. Dynamic loads use the supplied registration name. Missing rules
allow normal type checks.
Pass validators after the clock/permission arguments in the chosen constructor.
`context.set_registration_validator(slot, validator)` changes future admission
without evicting existing entries. Rejection leaves other entries untouched.
Adapted configure/cleanup is supported; adapted activation/deactivation and
typed state handover remain pending.

Plugins configure resources in `on_configure(const ComponentContext &)` and
release them in `on_cleanup()`. Both callbacks return `rpp::CallbackResult`;
the default implementations succeed without doing work. `context.configure()`
configures children before their parent and stops on failure. `context.cleanup()`
visits the parent before children in reverse order and continues after errors,
returning the first failure. Check results with `require_success()`; an optional
label can add caller-specific context.
Adapted instances run these callbacks in their hosting process. Cleanup must be
safe after partially completed configuration. The old `initialize()` API has
been removed; regenerate interfaces and rebuild plugins together.

`context.activate(state)` and `context.deactivate()` affect only this plugin.
Omit the state for cold activation. The manager marks the entry ACTIVE only after
`on_activate()` succeeds. It drains guarded calls before `on_deactivate()` and
marks the entry INACTIVE only after that callback succeeds. Normal parent
transitions do not change child states automatically.

Plugins may opt into child lifecycle from their callbacks. For example, a
`LifecyclePlugin<void>` that saved `context_` in `on_configure()` can use:

```cpp
rpp::CallbackResult on_activate() override
{
  return context_->activate_components();
}

rpp::CallbackResult on_deactivate() override
{
  return context_->deactivate_components();
}
```

The plugin chooses where to call these helpers relative to its own initialization
or shutdown. `activate_components()` activates selected exclusive candidates or
all list entries, without handover state. If this helper fails, it rolls back only
entries newly activated by that helper. A later failure in the parent's callback
does not automatically undo a successful helper call; the plugin controls recovery.
`deactivate_components()` visits active entries in reverse order, discards child
snapshots, continues after errors, and returns the first failure. Each child still
decides whether to manage its own descendants.

Script contexts have no plugin callbacks. Applications explicitly call
`activate_components()` and `deactivate_components()` on them. `close()` and unload
remain explicit full-tree teardown operations: they stop remaining active
descendants before cleanup and release, even if normal callbacks left them running.
Configure/cleanup propagation is unchanged. Adapted activation/deactivation remains
unsupported until runtime lifecycle control is implemented.
