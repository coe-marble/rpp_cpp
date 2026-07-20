cmake_minimum_required(VERSION 3.8)
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
find_package(pybind11 REQUIRED)
# uncomment the following section in order to fill in
# further dependencies manually.
# find_package(<dependency> REQUIRED)

# header only library
add_library(rpp_cpp src/plugin_runtime.capnp.c++)
target_link_libraries(rpp_cpp
  CapnProto::capnp
  CapnProto::capnp-rpc
  ${Python3_LIBRARIES}
  pybind11::embed
)
target_include_directories(rpp_cpp PUBLIC include
  ${CapnProto_INCLUDE_DIRS}
  ${Python3_INCLUDE_DIRS}
  ${pybind11_INCLUDE_DIRS}
)

add_executable(component_server_cpp src/component_server.cpp src/plugin_runtime.capnp.c++)
target_include_directories(component_server_cpp PRIVATE include)
target_link_libraries(component_server_cpp PRIVATE rpp_cpp)



install(TARGETS component_server_cpp
  DESTINATION lib/${PROJECT_NAME}
)
install(DIRECTORY DESTINATION bin)


install(CODE "
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E create_symlink
    \"../lib/${PROJECT_NAME}/component_server_cpp\"
    \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/rpp_component_server_cpp\"
  )
")


set(OVERRIDE_RPP_TEST true)
if(BUILD_TESTING)

  find_package(ament_lint_auto REQUIRED)
  find_package(ament_cmake_gtest REQUIRED)
  ament_lint_auto_find_test_dependencies()

  set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/rpp_autogen")
  set(INCLUDE_GENERATED_DIR "${GENERATED_DIR}/.rpp/interfaces/cpp")
  file(MAKE_DIRECTORY ${GENERATED_DIR})
  if (OVERRIDE_RPP_TEST)
    add_custom_target(GenerateTestRpp ALL
      COMMAND rpp test setup_tmp_rpp_with_test_plugins ${GENERATED_DIR} --override
      COMMENT "Generating C++ code from test_instance_plugin.capnp"
    )
  else()
    add_custom_target(GenerateTestRpp ALL
      COMMAND rpp test setup_tmp_rpp_with_test_plugins ${GENERATED_DIR}
      COMMENT "Generating C++ code from test_instance_plugin.capnp"
    )
  endif()


  include_directories(tests)
  add_custom_target(copy_test_data ALL
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/data
              ${CMAKE_CURRENT_BINARY_DIR}/tests/data
  )

  ament_add_gtest(test_instance_plugin_cpp tests/test_instance_plugin_cpp.cpp)

  file(GLOB_RECURSE GENERATED_SOURCES "${INCLUDE_GENERATED_DIR}/capnp_gen/**/*.c++")
  target_sources(test_instance_plugin_cpp PRIVATE
    ${GENERATED_SOURCES}
  )

  add_dependencies(test_instance_plugin_cpp GenerateTestRpp rpp_cpp)
  target_include_directories(test_instance_plugin_cpp PRIVATE include ${INCLUDE_GENERATED_DIR})
  target_compile_definitions(test_instance_plugin_cpp PRIVATE
    AUTOGEN_RPP_HOME="${GENERATED_DIR}/.rpp"
    TEST_DATA_DIR="${CMAKE_CURRENT_BINARY_DIR}/data"
  )
  target_link_libraries(test_instance_plugin_cpp
    CapnProto::capnp CapnProto::kj CapnProto::capnp-rpc CapnProto::kj-async rpp_cpp)


  ament_add_gtest(test_rpp_cpp tests/test_parameters.cpp)
  target_link_libraries(test_rpp_cpp rpp_cpp)
  target_compile_definitions(test_rpp_cpp PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_BINARY_DIR}/data"
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
