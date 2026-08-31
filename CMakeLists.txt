cmake_minimum_required(VERSION 3.12)
project(rpp_cpp)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# find dependencies
find_package(ament_cmake REQUIRED)
find_package(CapnProto REQUIRED)
find_package(pybind11 REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Development)
find_package(rclcpp QUIET)
# uncomment the following section in order to fill in
# further dependencies manually.
# find_package(<dependency> REQUIRED)

if(rclcpp_FOUND)
    message(STATUS "--> ROS 2 environment DETECTED. Using rclcpp logging backend.")
else()
    message(STATUS "--> ROS 2 NOT detected. Falling back to spdlog backend.")
    # Search for system-wide spdlog (installed via apt, vcpkg, conan, etc.)
    find_package(spdlog REQUIRED)
endif()

# ---------------------------------------------------------------------------
# STEP 1: CONFIGURE/BUILD CORE C++ TARGETS
# ---------------------------------------------------------------------------

add_library(rpp_cpp_core SHARED
  src/logger.cpp
  src/clock.cpp
)
target_include_directories(rpp_cpp_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
if(rclcpp_FOUND)
    target_compile_definitions(rpp_cpp_core PUBLIC USE_ROS2_LOGGING)
    target_link_libraries(rpp_cpp_core PUBLIC rclcpp::rclcpp)
else()
    target_link_libraries(rpp_cpp_core PUBLIC spdlog::spdlog)
endif()


add_library(rpp_cpp SHARED
  src/plugin_runtime.capnp.c++
  src/data_model.cpp
  src/parameter_handler.cpp
  src/python_helpers.cpp
  src/rpp_paths.cpp
  src/plugin_loader.cpp
  src/child_process.cpp
)
target_link_libraries(rpp_cpp
PUBLIC
  rpp_cpp_core
  CapnProto::capnp
  CapnProto::capnp-rpc
PRIVATE
  Python3::Python
  pybind11::embed
)
target_link_options(rpp_cpp PUBLIC "-rdynamic")

if(WIN32)
  target_link_libraries(rpp_cpp PRIVATE ws2_32)
endif()

set_target_properties(rpp_cpp PROPERTIES
  POSITION_INDEPENDENT_CODE TRUE
)
target_include_directories(rpp_cpp
PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

add_executable(component_server_cpp src/component_server.cpp)
target_include_directories(component_server_cpp PRIVATE include)
target_link_libraries(component_server_cpp rpp_cpp)

install(TARGETS component_server_cpp
  DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY include/
  DESTINATION include
)
install(DIRECTORY DESTINATION bin)
install(CODE "
  execute_process(
    COMMAND \${CMAKE_COMMAND} -E create_symlink
    \"../lib/${PROJECT_NAME}/component_server_cpp\"
    \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/rpp_component_server_cpp\"
  )
  ")

install(TARGETS rpp_cpp rpp_cpp_core
  EXPORT export_rpp_cpp
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

ament_export_targets(export_rpp_cpp HAS_LIBRARY_TARGET)
ament_export_include_directories(include)
if(rclcpp_FOUND)
    ament_export_dependencies(CapnProto rclcpp)
else()
    ament_export_dependencies(CapnProto)
endif()

# Add custom target that writes RPP_CPP_CORE_PATH to a global rpp configuration file
add_custom_target(write_rpp_cpp_core_path ALL
  COMMAND rpp registry config RPP_CPP_CORE_PATH="${CMAKE_CURRENT_BINARY_DIR}"
  COMMENT "Writing RPP_CPP_CORE_PATH to .rpp configuration file..."
)

add_dependencies(write_rpp_cpp_core_path rpp_cpp)


set(OVERRIDE_RPP_TEST false)
if(BUILD_TESTING)

  find_package(ament_lint_auto REQUIRED)
  find_package(ament_cmake_gtest REQUIRED)
  ament_lint_auto_find_test_dependencies()

  set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/rpp_autogen")
  set(INCLUDE_GENERATED_DIR "${GENERATED_DIR}/.rpp/interfaces/cpp")

  execute_process(
      COMMAND ${Python3_EXECUTABLE} -c
      "import os, rpp_testing; print(list(rpp_testing.__path__)[0])"
      OUTPUT_VARIABLE RPP_TESTING_PATH
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE RPP_TESTING_STATUS
      ERROR_VARIABLE PY_ERROR
  )

  if(NOT DEFINED RPP_TESTING_STATUS OR NOT RPP_TESTING_STATUS EQUAL 0)
      message(FATAL_ERROR
        "Could not find the rpp_testing package or there was an error while trying to import it.\n"
        "Please ensure that the rpp_testing package is installed and accessible in your Python environment.\n"
      )
  endif()
  set(RPP_ARGS test setup_tmp_rpp_with_test_plugins ${GENERATED_DIR})
  if(OVERRIDE_RPP_TEST)
    list(APPEND RPP_ARGS --override)
  endif()

  add_custom_command(
    OUTPUT "${GENERATED_DIR}/gen_checked.stamp"
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GENERATED_DIR}
    COMMAND rpp ${RPP_ARGS}
    COMMAND ${CMAKE_COMMAND} -E touch "${GENERATED_DIR}/gen_checked.stamp"
    COMMENT "Running test autogeneration linked to rpp_cpp_core..."
    VERBATIM
  )

  add_custom_target(run_rpp_autogen
    DEPENDS "${GENERATED_DIR}/gen_checked.stamp"
  )
  add_dependencies(run_rpp_autogen rpp_cpp_core rpp_cpp write_rpp_cpp_core_path)

  # ---------------------------------------------------------------------------
  # STEP 3: BUILD TEST TARGETS
  # ---------------------------------------------------------------------------



  ament_add_gtest(test_instance_plugin_cpp tests/test_instance_plugin_cpp.cpp)
  add_dependencies(test_instance_plugin_cpp run_rpp_autogen)

  file(GLOB_RECURSE GENERATED_SOURCES CONFIGURE_DEPENDS "${INCLUDE_GENERATED_DIR}/capnp_gen/**/*.c++")
  if(GENERATED_SOURCES)
    target_sources(test_instance_plugin_cpp PRIVATE
      ${GENERATED_SOURCES}
    )
  endif()

  target_include_directories(test_instance_plugin_cpp PRIVATE include ${INCLUDE_GENERATED_DIR})
  target_compile_definitions(test_instance_plugin_cpp PRIVATE
    AUTOGEN_RPP_HOME="${GENERATED_DIR}/.rpp"
    TEST_DATA_DIR="${RPP_TESTING_PATH}/data"
  )
  target_link_libraries(test_instance_plugin_cpp rpp_cpp)

  ament_add_gtest(test_rpp_cpp tests/test_parameters.cpp)

  add_dependencies(test_rpp_cpp rpp_cpp)
  target_include_directories(test_rpp_cpp PRIVATE include
    ${Python3_INCLUDE_DIRS}
    ${pybind11_INCLUDE_DIRS})
  target_link_libraries(test_rpp_cpp rpp_cpp)
  target_compile_definitions(test_rpp_cpp PRIVATE
    TEST_DATA_DIR="${RPP_TESTING_PATH}/data"
  )

  # the following line skips the linter which checks for copyrights
  # comment the line when a copyright and license is added to all source files
  set(ament_cmake_copyright_FOUND TRUE)
  # the following line skips cpplint (only works in a git repo)
  # comment the line when this package is in a git repo and when
  # a copyright and license is added to all source files
  set(ament_cmake_cpplint_FOUND TRUE)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
