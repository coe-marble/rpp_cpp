# rpp_cpp

`rpp_cpp` provides the C++ plugin runtime and the C++ component server.

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

Plugins configure resources in `on_configure(const ComponentContext &)` and
release them in `on_cleanup()`. Both callbacks return `rpp::CallbackResult`;
the default implementations succeed without doing work. `context.configure()`
configures children before their parent and stops on failure. `context.cleanup()`
visits the parent before children in reverse order and continues after errors,
returning the first failure. Check results with `require_success(operation)`.
Adapted instances run these callbacks in their hosting process. Cleanup must be
safe after partially completed configuration. The old `initialize()` API has
been removed; regenerate interfaces and rebuild plugins together.

For native trees, `context.activate(state)` activates children first, then the
root. Omit the argument for cold activation. Handover state applies only to the
root; children activate cold. A failure rolls back successfully activated child
subtrees in reverse order and preserves the activation failure. Rollback errors
are logged. `context.deactivate()` visits the parent before children and
continues after errors. It returns `DeactivationResult<std::any>` and suppresses
the root snapshot if any node fails to stop. Adapted activation/deactivation
returns failure until runtime lifecycle control is implemented. Transition
validation and application call gates remain manager responsibilities.
