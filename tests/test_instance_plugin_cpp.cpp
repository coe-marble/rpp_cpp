#include "gtest/gtest.h"
#include "rpp_cpp/plugin.hpp"
#include "rpp_cpp/data_model.hpp"
#include "rpp_cpp/cli_utils.hpp"
#include "rpp_cpp/data_manager.hpp"
#include "rpp_cpp/plugin_loader.hpp"
#include "rpp_cpp/plugin_runtime.hpp"
#include "rpp_cpp/network_utils.hpp"
#include "rpp_cpp/rpp_server_host.hpp"
#include "rpp_cpp/context.hpp"
#include "rpp_cpp/context_builder.hpp"
#include "rpp_cpp/parameter_handler.hpp"

#include "rpp_plugin_types/rpp_testing/MotionController2D.hpp"
#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <capnp/ez-rpc.h>
#include <thread>
#include <atomic>


class TestSuite : public ::testing::Test {

public:
    static std::string test_lib;
    static std::string rpp_home_dir;
    static std::string test_data_dir;
    static std::unique_ptr<rpp::RppDataManager> data_manager;
    static bool initialization_successful;
    static std::unique_ptr<pybind11::scoped_interpreter> python_interpreter;

protected:
    static void SetUpTestSuite() {
        rpp_home_dir = std::getenv("RPP_HOME");
        rpp::RPP_HOME = rpp_home_dir;
        test_data_dir = std::getenv("TEST_DATA_DIR");
        test_lib = "test_lib";
        data_manager = std::make_unique<rpp::RppDataManager>(rpp_home_dir);
        python_interpreter =
            std::make_unique<pybind11::scoped_interpreter>();
        initialization_successful = true;
    }

    static void TearDownTestSuite() {
        // std::filesystem::remove_all(tmp_dir);
    }

    void thread_run(rpp::ServerAdapter* server_adapter,
        std::atomic<bool>* server_ready, std::atomic<bool>* shutdown)
    {
        auto io = kj::setupAsyncIo();

        server_adapter->start_adapter_server__(io);

        server_ready->store(true);

        auto& timer = io.provider->getTimer();
        std::function<kj::Promise<void>()> checkShutdown;
        checkShutdown = [&]() -> kj::Promise<void> {
            if (shutdown->load()) {
                return kj::READY_NOW;
            }
            return timer.afterDelay(10 * kj::MILLISECONDS)
                .then(checkShutdown);
        };
        checkShutdown().wait(io.waitScope);
        server_adapter->close_adapter_server__();
    }
};

std::unique_ptr<rpp::RppDataManager> TestSuite::data_manager = nullptr;
std::string TestSuite::test_lib = "";
std::string TestSuite::test_data_dir = "";
std::string TestSuite::rpp_home_dir = "";
std::unique_ptr<pybind11::scoped_interpreter> TestSuite::python_interpreter = nullptr;
bool TestSuite::initialization_successful = false;



TEST_F(TestSuite, TestMsgAsStruct) {
    // Create PluginInfo for the C++ plugin

    rpp_testing::MotionController2D::Odometry2D odom_msg;

    odom_msg.pose().position().x() = 6.0;
    odom_msg.pose().position().y() = 2.0;
    odom_msg.pose().yaw() = 1.57;

    auto native_struct = odom_msg.pose().as_struct();
    ASSERT_DOUBLE_EQ(native_struct.position.x, 6.0);
    ASSERT_DOUBLE_EQ(native_struct.position.y, 2.0);
    ASSERT_DOUBLE_EQ(native_struct.yaw, 1.57);

    native_struct.position.x = 10.0;
    native_struct.position.y = 5.0;
    native_struct.yaw = 3.14;
}


TEST_F(TestSuite, TestInstanceByGenericInterface) {

    std::string plugin_name = "test_lib::ComponentPluginSimpleCpp";

    rpp::PluginInfo plugin_info = TestSuite::data_manager->get_plugin_info_from_lib(plugin_name);

    auto instance = rpp::load_cpp_plugin_from_shared_library<rpp::Plugin>(plugin_info);
    ASSERT_TRUE(instance != nullptr);
    rpp_testing::MotionController2D* motion_controller =
        dynamic_cast<rpp_testing::MotionController2D*>(instance.get());

    ASSERT_TRUE(motion_controller != nullptr);

}

TEST_F(TestSuite, TestSimpleInstancePlugin) {
    // Create PluginInfo for the C++ plugin

    // assert initialization in TestSuite::SetUpTestSuite() was successful

    std::string plugin_name = "test_lib::ComponentPluginSimpleCpp";

    rpp::PluginInfo plugin_info = TestSuite::data_manager->get_plugin_info_from_lib(plugin_name);

    auto plugin = rpp::load_cpp_plugin_from_shared_library<rpp_testing::MotionController2D>(plugin_info);

    auto parameter_desc = rpp::load_cpp_plugin_parameters_description(plugin_info);

    ASSERT_TRUE(parameter_desc != nullptr);

    auto components_desc = rpp::load_cpp_plugin_component_spec(plugin_info);

    ASSERT_TRUE(components_desc == nullptr);

    rpp_testing::MotionController2D::Odometry2D odom_msg;

    odom_msg.pose().position().x() = 1.0;
    odom_msg.pose().position().y() = 2.0;
    odom_msg.pose().yaw() = 1.57;

    auto vector = plugin->step(odom_msg, 0.1);

    ASSERT_DOUBLE_EQ(vector.x(), 3.0);
    ASSERT_DOUBLE_EQ(vector.y(), 1.0);


    bool is_valid = plugin->validate(odom_msg.as_const());

    ASSERT_FALSE(is_valid);
    odom_msg.pose().position().x() = 6.0;

    // works with implicit conversion to Const
    ASSERT_TRUE(plugin->validate(odom_msg));
}



TEST_F(TestSuite, TestPluginAdapterLocal)
{
    std::string host = "127.0.0.1";
    uint16_t port = get_available_port();
    std::string plugin_name = "test_lib::ComponentPluginSimpleCpp";
    std::atomic<bool> server_ready(false);
    std::atomic<bool> shutdown(false);

    std::thread server_thread([&]() {
        // Kreiramo stvarni plugin i omotamo ga u server adapter
        rpp::PluginInfo plugin_info = TestSuite::data_manager->get_plugin_info_from_lib(plugin_name);
        ASSERT_TRUE(plugin_info.plugin_name == plugin_name);

        auto instance =
            rpp::load_cpp_plugin_from_shared_library<rpp_testing::MotionController2D>(
                plugin_info);

        auto odom_msg = rpp_testing::MotionController2D::Odometry2D();
        odom_msg.pose().position().x() = 6.0;

        auto is_valid = instance->validate(odom_msg);
        ASSERT_TRUE(is_valid);

        auto server_adapter =
            rpp::load_plugin_adapter_server<rpp_testing::MotionController2D>(
                std::move(instance), host, port);
        try{
            thread_run(server_adapter.get(), &server_ready, &shutdown);
        }
        catch (const std::exception& e) {
            FAIL() << "Exception in server thread: " << e.what();
        }
        catch (...) {
            FAIL() << "Unknown exception in server thread.";
        }
    });


    auto plugin_client = \
        rpp::load_plugin_adapter_client<rpp_testing::MotionController2D>(host, port);

    ASSERT_TRUE(plugin_client != nullptr);

    int max_wait_time_ms = 1000; // Maksimalno vrijeme čekanja u milisekundama
    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        max_wait_time_ms -= 10;
        if (max_wait_time_ms <= 0) {
            FAIL() << "Server not ready after waiting for 1 second.";
        }
    }

    plugin_client->connect_adapter_client__();

    rpp_testing::MotionController2D::Odometry2D odom_msg;
    odom_msg.pose().position().x() = 1.0;
    odom_msg.pose().position().y() = 2.0;
    odom_msg.pose().yaw() = 4;

    bool is_valid = plugin_client->validate(odom_msg);

    ASSERT_FALSE(is_valid);

    odom_msg.pose().position().x() = 6.0;

    is_valid = plugin_client->validate(odom_msg);

    ASSERT_TRUE(is_valid);

    shutdown.store(true);
    server_thread.join();

}



TEST_F(TestSuite, TestPluginAdapterLocalWithStringPluginName) {

    std::string plugin_name = "test_lib::ComponentPluginSimpleCpp";
    std::string host = "127.0.0.1";
    uint16_t port = get_available_port();

    rpp::PluginInfo plugin_info = TestSuite::data_manager->get_plugin_info_from_lib(plugin_name);

    std::atomic<bool> server_ready(false);
    std::atomic<bool> shutdown(false);
    std::thread server_thread([&]() {
        // Kreiramo stvarni plugin i omotamo ga u server adapter
        ASSERT_TRUE(plugin_info.plugin_name == plugin_name);
        auto instance = rpp::load_cpp_plugin_from_shared_library(plugin_info);
        auto server_adapter = rpp::load_plugin_adapter_server(plugin_info, std::move(instance), host, port);
        thread_run(server_adapter.get(), &server_ready, &shutdown);
    });


    auto plugin_client_raw = \
        rpp::load_plugin_adapter_client(plugin_info, host, port);


    ASSERT_TRUE(plugin_client_raw != nullptr);
    int max_wait_time_ms = 1000; // Maksimalno vrijeme čekanja u milisekundama
    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        max_wait_time_ms -= 10;
        if (max_wait_time_ms <= 0) {
            FAIL() << "Server not ready after waiting for 1 second.";
        }
    }
    plugin_client_raw->connect_adapter_client__();
    auto plugin_client = dynamic_cast<rpp_testing::MotionController2D*>(plugin_client_raw.get());

    rpp_testing::MotionController2D::Odometry2D odom_msg;
    odom_msg.pose().position().x() = 1.0;
    odom_msg.pose().position().y() = 2.0;
    odom_msg.pose().yaw() = 4;

    bool is_valid = plugin_client->validate(odom_msg);

    ASSERT_FALSE(is_valid);

    odom_msg.pose().position().x() = 6.0;

    is_valid = plugin_client->validate(odom_msg);
    ASSERT_TRUE(is_valid);

    shutdown.store(true);
    server_thread.join();
}

TEST_F(TestSuite, TestPluginAdapterWithPythonPlugin) {

    std::atomic<bool> server_ready(false);
    std::string plugin_name = "test_lib::ComponentPluginSimplePy";
    std::string host = "127.0.0.1";
    uint16_t port = get_available_port();
    uint16_t runtime_port = get_available_port();

    rpp::PluginInfo plugin_info = TestSuite::data_manager->get_plugin_info_from_lib(plugin_name);


    auto plugin_client_raw = \
        rpp::load_plugin_adapter_client(plugin_info, host, port);


    std::string command = "rpp_component_server_python"
        " --host " + host +
        " --plugin-port " + std::to_string(port) +
        " --runtime-port " + std::to_string(runtime_port) +
        " --plugin " + plugin_name +
        " --home " + TestSuite::rpp_home_dir +
        " --component-path " + TestSuite::rpp_home_dir + "/components";


    std::thread server_thread([command]() {
        std::system(command.c_str());
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    plugin_client_raw->connect_adapter_client__();

    auto plugin_client = dynamic_cast<rpp_testing::MotionController2D*>(plugin_client_raw.get());
    auto runtime_client = std::make_unique<rpp::PluginRuntimeClient>(host, runtime_port);
    ASSERT_TRUE(plugin_client != nullptr);
    ASSERT_TRUE(runtime_client != nullptr);


    rpp_testing::MotionController2D::Odometry2D odom_msg;
    odom_msg.pose().position().x() = 1.0;
    odom_msg.pose().position().y() = 2.0;
    odom_msg.pose().yaw() = 4;

    bool is_valid = plugin_client->validate(odom_msg);

    ASSERT_FALSE(is_valid);

    odom_msg.pose().position().x() = 6.0;

    is_valid = plugin_client->validate(odom_msg);

    runtime_client->shutdown();
    server_thread.join();

    ASSERT_TRUE(is_valid);
}

TEST_F(TestSuite, TestPluginRuntimeAdapterLocal)
{
    std::string host = "127.0.0.1";
    uint16_t port = get_available_port();
    uint16_t port_runtime = get_available_port();

    auto plugin_info = data_manager->get_plugin_info_from_lib("test_lib::ComponentPluginSimpleCpp");
    std::atomic<bool> server_ready(false);
    std::thread server_thread([&]() {

        rpp::RppServerHost server_host(host, port_runtime);
        auto instance = rpp::load_cpp_plugin_from_shared_library(plugin_info);
        auto server_adapter =
            rpp::load_plugin_adapter_server(plugin_info, std::move(instance), host, port);

        server_host.add_server(std::move(server_adapter));
        server_ready.store(true);
        server_host.run();
    });

    while (!server_ready.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto client = std::make_unique<rpp::PluginRuntimeClient>(host, port_runtime);

    client->ping();

    auto adapter_list = client->listAdapters();

    ASSERT_EQ(adapter_list.size(), 1);
    ASSERT_EQ(adapter_list[0].plugin_name, "test_lib::ComponentPluginSimpleCpp");
    ASSERT_EQ(adapter_list[0].name, "test_lib::ComponentPluginSimpleCpp_adapter_server");

    client->shutdown();


    server_thread.join();
}


RPP_PARAM_STRUCT(TestStruct1,
    RPP_MEMBER(int, width, 641),
    RPP_MEMBER(std::string, height, "481"),
    RPP_MEMBER(double, fps, 31.0)
)

RPP_PARAM_STRUCT(TestStruct2,
    RPP_MEMBER(TestStruct1, struct1, TestStruct1()),
    RPP_MEMBER(std::vector<int>, values, std::vector<int>{2, 3, 4})
)


TEST_F(TestSuite, TestComplexInstancePluginUsingContextBuilder)
{
    std::string component_folder = TestSuite::test_data_dir + "/test_component";

    auto context = rpp::ComponentContextBuilder(
        *TestSuite::data_manager,
        component_folder,
        *TestSuite::python_interpreter).build();

    auto subcomponent_list = context.list_subcomponents();
    ASSERT_EQ(subcomponent_list.size(), 1);
    ASSERT_EQ(subcomponent_list[0], "ctl1");

    auto& subcomponent = context.get_component<rpp_testing::MotionController2D>("ctl1");

    auto odom_msg = rpp_testing::MotionController2D::Odometry2D();
    odom_msg.pose().position().x() = 1.0;
    odom_msg.pose().position().y() = 2.0;
    odom_msg.pose().yaw() = 1.57;
    auto result = subcomponent.step(odom_msg, 0.1);
    ASSERT_DOUBLE_EQ(result.x(), 3.0);
    ASSERT_DOUBLE_EQ(result.y(), 1.0);


    // TEST DEFAULT PARAMETERS LOADED FROM JSON FILE
    ASSERT_EQ(context.get_parameter<int>("int_var"), 1);
    ASSERT_EQ(context.get_parameter<std::string>("str_var"), "test");
    auto struct1 = context.get_parameter<TestStruct1>("struct1_var");
    ASSERT_EQ(struct1.width, 640);
    ASSERT_EQ(struct1.height, "480");
    ASSERT_DOUBLE_EQ(struct1.fps, 30.0);

    // TEST PARAMETERS OVERRIDDEN BY PYTHON MODULE
    ASSERT_FLOAT_EQ(context.get_parameter<float>("float_var"), 10.0f);
    auto struct2 = context.get_parameter<TestStruct2>("struct2_var");
    ASSERT_EQ(struct2.struct1.width, 800);
    ASSERT_EQ(struct2.struct1.height, "500");
    ASSERT_DOUBLE_EQ(struct2.struct1.fps, 35.0);
    ASSERT_EQ(struct2.values.size(), 5);
    ASSERT_EQ(struct2.values[0], 1);
    ASSERT_EQ(struct2.values[1], 2);
    ASSERT_EQ(struct2.values[2], 3);
    ASSERT_EQ(struct2.values[3], 4);
    ASSERT_EQ(struct2.values[4], 5);

    const auto& subcomponent_context = context.get_subcomponent_context("ctl1");
    ASSERT_EQ(subcomponent_context.list_subcomponents().size(), 0);
    // TEST PARAMETERS OF SUBCOMPONENT DEFAULT
    ASSERT_EQ(subcomponent_context.get_parameter<float>("float_var"), 6.0f);
    struct2 = subcomponent_context.get_parameter<TestStruct2>("struct2_var");
    ASSERT_EQ(struct2.struct1.width, 641);
    ASSERT_EQ(struct2.struct1.height, "481");
    ASSERT_DOUBLE_EQ(struct2.struct1.fps, 31.0);
    ASSERT_EQ(struct2.values.size(), 3);
    ASSERT_EQ(struct2.values[0], 2);
    ASSERT_EQ(struct2.values[1], 3);
    ASSERT_EQ(struct2.values[2], 4);

    // TEST PARAMETERS OF SUBCOMPONENT OVERRIDDEN BY PYTHON MODULE
    ASSERT_EQ(subcomponent_context.get_parameter<int>("int_var"), 10);
    ASSERT_EQ(subcomponent_context.get_parameter<std::string>("str_var"), "test10");
    struct1 = subcomponent_context.get_parameter<TestStruct1>("struct1_var");
    ASSERT_EQ(struct1.width, 800);
    ASSERT_EQ(struct1.height, "500");
    ASSERT_DOUBLE_EQ(struct1.fps, 35.0);
}


TEST_F(TestSuite, TestComplexInstancePluginUsingContextBuilderWithPythonSubc) {

    std::string plugin_name = "test_lib::ComponentPluginSimplePy";
    std::string host = "127.0.0.1";
    uint16_t plugin_port = get_available_port();
    uint16_t runtime_port = get_available_port();

    std::string component_folder = TestSuite::test_data_dir + "/test_component_cpp_with_python_subc";


    auto context = rpp::ComponentContextBuilder(
        *TestSuite::data_manager,
        component_folder,
        *TestSuite::python_interpreter,
        host, plugin_port, runtime_port).build();

    

}



int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    setenv("RPP_HOME", AUTOGEN_RPP_HOME, 1);
    setenv("TEST_DATA_DIR", TEST_DATA_DIR, 1);

    // Pokrećemo GoogleTest najnormalnije
    return RUN_ALL_TESTS();
}
