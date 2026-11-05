# C++ context construction and lifecycle

This describes the current implementation. The slot refactor is not yet
validated by a build or test run. The native C++ path below is implemented;
adapted activation and state handover remain unfinished.

## Application entry points

```cpp
rpp::RppDataManager data_manager(rpp_home, workspace_path);
rpp::ComponentContextBuilder builder(data_manager, rpp::ClockOptions{}, true);
auto context = builder.build_script_from_path(script_path);

context.configure().require_success();
context.activate_components().require_success();
auto controller = context.get_component<Controller>("controller");
// The application calls controller-specific methods from its own loop.
context.close().require_success();
```

The builder's third argument is `allow_dynamic_components`, defaulting to false.
Alternatively, use `ComponentContextBuilder(data_manager, policy, clock_options,
validators)`, where `policy(slot_name, ComponentSlot)` returns the permission.
These are separate constructors; the builder resolves the selected permission policy.
It is evaluated once for each slot, including declared empty slots. `ComponentSlot`
contains the base plugin type and `SINGLE`, `OPTION`, or `LIST` kind. Descendant
slots use the same policy, including descendants loaded later. Capture values
or keep referenced data alive for those later builds. The decision sets each
locally constructed manager's permission, including managers holding adapted
components. Neither permission mode is forwarded to external hosts by the builder.
The application does not create a slot manager. Building a context constructs
objects and prepares transports; configuration and activation are separate calls.

## Who does what

| Class/function | Responsibility |
| --- | --- |
| `RppDataManager` | Resolve workspace paths and read component descriptions, script configurations, and registered plugin metadata on demand. |
| `ComponentContextBuilder` | Assemble the tree: resolve records, load instances, load parameters, recursively build children, and create contexts. |
| `load_cpp_plugin_from_shared_library` | Load the registered native library and call its factory to construct the plugin instance. |
| `params::ParameterHandler` | Evaluate the component parameter file and combine its values with parameter declarations/defaults. |
| `ComponentContext` | Expose parameters, components, child contexts, lifecycle, and slot operations to application/plugin code. |
| `ComponentManager` | Own entries in one context slot and enforce admission, activation policy, transition state, generation checks, and guarded-call draining. |

`RppDataManager` construction stores locations; it does not create plugin
instances. Builder construction stores the data-manager reference and build
options; it does not start the component tree either. The data manager must
outlive the build. Dynamic-loading callbacks retain their own data-manager copy
so they do not depend on the original builder remaining alive.
Each later load creates a builder from the saved settings. Capturing the original
builder's pointer would be unsafe: it may have been a temporary destroyed as soon
as the initial build returned. The builder gives the factory directly to each slot
manager; the manager calls it when loading or replacing a component. Context
constructors receive assembled managers and have no factory or policy arguments.

## Building a component root

The call starts at `ComponentContextBuilder::build_component_from_path(path)`:

1. `make_component_node()` calls `resolve_component()`, which asks the data
   manager to load the component record. Linked records are resolved to their
   target. `get_plugin_info_from_lib()` reads its registered plugin metadata.
2. `prepare_component_processes()` inspects the described tree. Purely native
   C++ trees need no adapter process or RPC executor. Adapted nodes are prepared
   through the external-process path described below.
3. `build_for_component()` chooses native construction or an adapted proxy.
4. For native C++, `build_native_component()` calls
   `load_cpp_plugin_from_shared_library()`. The plugin constructor runs here.
5. `ParameterHandler` loads and resolves that component's parameters. Its C++
   implementation may evaluate a Python parameter file using an embedded
   interpreter; that is separate from async/RPC hosting.
6. The builder recursively calls `build_for_component()` for the described
   subcomponents.
7. `ComponentContextBuilder::make_slot_managers()` creates one `ComponentManager`
   for each slot, including declared empty slots. The builder gives each manager
   its factory, activation policy, dynamic permission, and admission rules, then
   calls `adopt()` for each initial child context.
8. The private `ComponentManager::adopt()` is called by the builder for initial
   insertion. It checks the declared plugin type, runs the optional
   registration validator, and commits the child as `UNCONFIGURED`. It sets the
   child's non-owning link back to this slot manager.
9. The builder constructs the `ComponentContext`, moving the resolved parameters
   and assembled manager map into it alongside its plugin and optional host handle.
   The context exposes these resources and delegates slot operations to managers.
10. The root context is returned. The local map of prepared components is destroyed
    automatically, including when a build throws. Returned contexts keep shared
    ownership of the instances and host processes they need. The builder stores
    no prepared components and needs no cleanup guard.

The native parent plugin constructor runs before its children are built.
Configuration visits children before their parent. Activation and deactivation
are local to the plugin unless its callbacks explicitly manage child components.
A constructor must not expect `on_configure(context)` to have run or the component
tree to be available yet.

```mermaid
sequenceDiagram
    participant App as Application
    participant Builder as ComponentContextBuilder
    participant Data as RppDataManager
    participant Loader as Native loader
    participant Context as ComponentContext
    participant Slot as ComponentManager per slot
    App->>Builder: build_component_from_path(path)
    Builder->>Data: load records and plugin metadata
    Builder->>Loader: load_cpp_plugin_from_shared_library(info)
    Loader-->>Builder: constructed plugin instance
    Note over Builder: Resolve parameters and recursively build children
    Builder->>Slot: create with factory, slot policy, permission, and validators
    Builder->>Slot: adopt each initial child context
    Note over Slot: Validate admission; state = UNCONFIGURED
    Builder->>Context: construct with instance, parameters, and assembled managers
    Builder-->>App: root context
```

## Building a script root

`build_script_from_path()` resolves the script-description JSON through the
data manager and delegates to `build_script_from_description_path()`.
The builder reads the chosen configuration's component assignments, resolves
their paths, builds those components, then constructs a script context with
their slot managers.

The script context has no plugin instance of its own. Building it does not run
the script's C++ application code; it builds the components described for it.

## Ownership and slot selection

```text
root ComponentContext
  ├── its plugin instance and parameters (component roots only)
  └── slots
      └── ComponentManager
          ├── entry A → child ComponentContext → plugin instance and nested slots
          └── entry B → child ComponentContext → plugin instance and nested slots
```

Managers own child contexts through shared pointers. Each child keeps:

- `owner_`: a weak pointer back to its owning manager, avoiding an ownership cycle.
- `owner_name_`: its entry identifier in that slot, the workspace
  component name for initial entries and the caller-supplied name for runtime loads.

The identifier is not the plugin class name. Multiple component instances can
use the same plugin implementation.

Single `T` and `Option[T]` use the same exclusive manager policy. Their initial
construction differs: one selected component versus a candidate collection.
The current implementation initially selects the first admitted candidate.
`List[T]` permits multiple active entries. Admission does not activate anything.

Both static and dynamic trees use managers. With dynamic changes disabled,
initial construction and ordinary lifecycle remain available; runtime membership
changes, replacement, and selecting another exclusive candidate are rejected.
The permission applies to managers created by the builder. External hosts are
launched independently of this local decision.

`allows_dynamic_components()` reports this instance's owning-slot permission;
it does not inspect child slots. Unmanaged roots return false. The overload taking
a slot name reports that child slot's permission. These permissions are independent:
unloading a permitted parent may tear down its entire subtree, even when child
slots forbid direct replacement. Ordinary activation/deactivation changes only
the parent unless its callbacks explicitly manage children. A direct request to
switch a child still checks that child's slot, and parent replacement does not
change any child permissions.

## Configuration and activation

For a script root, `context.configure()` calls `configure_impl()` directly.
For a child context, the public method uses `owner_` and `owner_name_` to call
the owning manager instead. The manager checks the entry state, calls the
child's private `configure_impl()`, and records `INACTIVE` on success.

`configure_impl()` visits child contexts through their slot managers before
calling its own instance's `Plugin::configure(context)`, which dispatches
`on_configure(context)`. A script root has no own callback. All registered
candidates are configured, including inactive option candidates.

Activation/deactivation are local to the current plugin:

```text
context.activate(state)
  → owning ComponentManager::activate(entry), if managed
  → ComponentContext::activate_impl(state)
  → Plugin::activate_state(state)
  → plugin.on_activate(optional_state)
  → manager marks ACTIVE on success
```

`on_activate()` runs before the manager opens the active-call gate. For
`deactivate()`, the manager first drains guarded calls, runs `on_deactivate()`, and
marks the entry INACTIVE on success. The plugin chooses whether and when to invoke
`context.activate_components()` or `context.deactivate_components()` from those
callbacks. Script roots have no plugin, so applications call the helpers directly;
`activate()`/`deactivate()` on a script do not transition its assignments.

The activation helper asks each manager to activate selected exclusive candidates
or all loaded list entries, without handover state. Only a failed helper call
rolls back entries that it newly activated; previously active entries are preserved.
If the helper succeeds but the parent callback later fails, child states remain
under plugin control. Deactivation of contained components visits active entries
in reverse order, continues after failures, and returns the first error without
child handover snapshots. Each child's own callbacks independently decide whether
to transition its descendants. Neither helper changes dynamic permissions.

No generic update loop starts. The application retains its timer/update loop.

## Reading and managing components

`get_parameter<T>()` reads the context's resolved parameters.
`get_logger()` and `get_clock()` return shared pointers to const objects. Logging
and reading timestamps remain available; callers cannot mutate the clock through
the context getter.
`get_component<T>(slot)` asks the slot manager for its selected context and
returns that context's plugin instance. `get_components<T>(slot)` returns the
loaded instances in that slot, including inactive candidates.

`get_subcomponent_context(slot)` returns the selected context. Its two-name
overload looks up a named entry. The plural method returns a snapshot of shared
const context pointers. `list_subcomponents()` lists slot names, while
`list_components(slot)` returns entry state/generation information.

Calling lifecycle methods on a returned child context still routes through its
manager. The child passes its own address as an expected-generation check, so
an old retained context cannot operate on a replacement under the same name.
This delegation is why the back-reference exists.

Friendship is one-way: `ComponentManager` can access context internals to bind
ownership and run lifecycle implementations without delegating back to itself.
`ComponentContext` uses manager methods for permission queries, guarded-call
checks, and activation; it cannot access manager fields or locks. The manager's
`activate_selected()` holds the slot mutation lock while selecting and activating
entries, and returns newly activated contexts through a shared-pointer collection
so the explicit activation helper can roll back only work it started.

For concurrent calls and transitions, use
`with_active_component<T>(slot, entry, callback)`. It borrows the instance for
the callback, tracks the call, and releases it afterward. Ordinary getter
references must not be retained across unloading/replacement.

For runtime loading, the call chain is:

```text
context.load_component(slot, entry, component_path)
  → slot manager.load(...)
  → permission and entry-state checks
  → builder-provided factory creates a fresh context tree
  → type/admission validation against registered entries
  → commit as UNCONFIGURED
```

Configure and activate the new entry explicitly. Switching is also a context
operation: it delegates to the slot manager, stops the source, transfers the
optional owned state, and activates/selects the target on success.

Validators receive the prepared candidate context and existing context
snapshots, including inactive candidates. They inspect metadata/parameters and
return `CallbackResult`; they must not perform lifecycle operations. Initial
validators are supplied to the builder, keyed by base slot plugin type and then
the name of the candidate being registered. Initial names come from the workspace
component name; empty names are rejected on registration. Dynamic registrations use
the caller's name. Rules apply throughout the tree wherever the type and candidate
name match. `List[T]` and `Option[T]` both use `T` as the key. Missing rules permit
normal type validation. The context setter changes future
admission without evicting or revalidating current entries.

## Shutdown and failures

Root `close()` deactivates its own plugin and explicitly stops remaining active
descendants, including descendants under inactive parents. It then performs
cleanup if configuration was attempted and releases runtime resources. Managed
unload performs the same descendant shutdown before cleanup and release. This
explicit teardown does not depend on plugin callbacks opting into child lifecycle.
Teardown deactivation and cleanup visit parents before children and continue through
independent children and reports failures. A failed lifecycle phase prevents
destruction of the affected instance. Closing an individual borrowed child is
an unload request to its manager and obeys the dynamic-modification permission.

Explicitly check `close()` results; destructor fallback does not replace
deterministic lifecycle shutdown. Managers retain instances whose shutdown
cannot safely complete rather than treating failure as permission to unload
active code.

## Adapted components

For an adapted node, the builder prepares `ExternalComponentProcess`, loads an
adapter client, starts/connects the hosting process, and builds a local context
around that proxy and its process owner. The host's builder constructs the real
component tree. Configure/cleanup travel through `PluginRuntimeClient` and
`plugin_runtime.capnp` to the real context in that process.

Transport preparation during building is not plugin activation. Adapted
activation/deactivation and state handover are still explicitly unsupported in
this increment. Native C++ trees create no RPC executor or external process.

## Source map

- [RppDataManager](../include/rpp_cpp/data_manager.hpp)
- [ComponentContextBuilder](../include/rpp_cpp/context_builder.hpp)
- [ComponentContext interface](../include/rpp_cpp/context.hpp) and [implementation](../src/context.cpp)
- [ComponentManager interface](../include/rpp_cpp/component_manager.hpp) and [implementation](../src/component_manager.cpp)
- [Native plugin loader](../src/plugin_loader.cpp)
