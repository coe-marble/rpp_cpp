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
