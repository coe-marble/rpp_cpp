#include "gtest/gtest.h"
#include "rpp_cpp/plugin.hpp"
#include "rpp_cpp/plugin_info.hpp"
#include "rpp_cpp/cli_utils.hpp"
#include "rpp_cpp/library_manager.hpp"
#include "rpp_cpp/plugin_loader.hpp"
#include "rpp_cpp/plugin_runtime.hpp"
#include "rpp_plugin_types/rpp_common/MotionController2D.hpp"
#include "rpp_cpp/network_utils.hpp"
#include "rpp_cpp/rpp_server_host.hpp"

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <capnp/ez-rpc.h>
#include <thread>
#include <atomic>


class TestSuite : public ::testing::Test {

public:
    static std::string test_lib;
    static std::string rpp_home_dir;
    static std::unique_ptr<rpp::LibraryManager> library_manager;
    static bool initialization_successful;

protected:
    static void SetUpTestSuite() {
        rpp_home_dir = std::getenv("RPP_HOME");
        rpp::RPP_HOME = rpp_home_dir;
        test_lib = "test_lib";
        library_manager = std::make_unique<rpp::LibraryManager>(rpp_home_dir);
        initialization_successful = true;
    }

    static void TearDownTestSuite() {
        // std::filesystem::remove_all(tmp_dir);
    }

    void thread_run(std::shared_ptr<rpp::ServerAdapter> server_adapter,
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

std::unique_ptr<rpp::LibraryManager> TestSuite::library_manager = nullptr;
std::string TestSuite::test_lib = "";
std::string TestSuite::rpp_home_dir = "";
bool TestSuite::initialization_successful = false;


TEST_F(TestSuite, TestInstanceByGenericInterface) {

    std::string plugin_name = "test_lib::ComponentPlugin";

    rpp::PluginInfo plugin_info = TestSuite::library_manager->get_plugin_info(plugin_name);

    auto instance = rpp::load_cpp_plugin_from_shared_library<rpp::Plugin>(plugin_info);
    ASSERT_TRUE(instance != nullptr);
    rpp_common::MotionController2D* motion_controller = dynamic_cast<rpp_common::MotionController2D*>(instance.get());

    ASSERT_TRUE(motion_controller != nullptr);

}

TEST_F(TestSuite, TestInstancePlugin) {
    // Create PluginInfo for the C++ plugin

    // assert initialization in TestSuite::SetUpTestSuite() was successful

    std::string plugin_name = "test_lib::ComponentPlugin";

    rpp::PluginInfo plugin_info = TestSuite::library_manager->get_plugin_info(plugin_name);

    auto loader = rpp::load_cpp_plugin_from_shared_library<rpp_common::MotionController2D>(plugin_info);

    rpp_common::MotionController2D::Pose2D pose_msg;

    pose_msg.position().x() = 1.0;
    pose_msg.position().y() = 2.0;
    pose_msg.yaw() = 1.57;

    auto vector = loader->step(pose_msg, 0.1);

    ASSERT_DOUBLE_EQ(vector.x(), 3.0);
    ASSERT_DOUBLE_EQ(vector.y(), 1.0);


    bool is_valid = loader->validate(pose_msg.as_const());

    ASSERT_FALSE(is_valid);
    pose_msg.position().x() = 6.0;

    // works with implicit conversion to Const
    ASSERT_TRUE(loader->validate(pose_msg));
}

TEST_F(TestSuite, TestInstancePluginWithAsStruct) {
    // Create PluginInfo for the C++ plugin

    // assert initialization in TestSuite::SetUpTestSuite() was successful

    std::string plugin_name = "test_lib::ComponentPluginWithUsingNamespace";

    rpp::PluginInfo plugin_info = TestSuite::library_manager->get_plugin_info(plugin_name);

    auto instance = rpp::load_cpp_plugin_from_shared_library<rpp_common::MotionController2D>(plugin_info);

    rpp_common::MotionController2D::Pose2D pose_msg;

    pose_msg.position().x() = 1.0;
    pose_msg.position().y() = 2.0;
    pose_msg.yaw() = 1.57;

    bool is_valid = instance->validate(pose_msg.as_const());

    ASSERT_FALSE(is_valid);
    pose_msg.position().x() = 6.0;

    // works with implicit conversion to Const
    ASSERT_TRUE(instance->validate(pose_msg));

}

TEST_F(TestSuite, TestPluginAdapterLocal)
{
    std::string host = "127.0.0.1";
    uint16_t port = get_available_port();
    std::string plugin_name = "test_lib::ComponentPlugin";
    std::atomic<bool> server_ready(false);
    std::atomic<bool> shutdown(false);

    std::thread server_thread([&]() {
        // Kreiramo stvarni plugin i omotamo ga u server adapter
        rpp::PluginInfo plugin_info = TestSuite::library_manager->get_plugin_info(plugin_name);
        ASSERT_TRUE(plugin_info.plugin_name == plugin_name);

        auto instance = rpp::load_cpp_plugin_from_shared_library<rpp_common::MotionController2D>(plugin_info);
        auto server_adapter = rpp::load_plugin_adapter_server<rpp_common::MotionController2D>(instance, host, port);
        try{
            thread_run(server_adapter, &server_ready, &shutdown);
        }
        catch (const std::exception& e) {
            FAIL() << "Exception in server thread: " << e.what();
        }
        catch (...) {
            FAIL() << "Unknown exception in server thread.";
        }
    });


    auto plugin_client = \
        rpp::load_plugin_adapter_client<rpp_common::MotionController2D>(host, port);

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

    rpp_common::MotionController2D::Pose2D pose_msg;
    pose_msg.position().x() = 1.0;
    pose_msg.position().y() = 2.0;
    pose_msg.yaw() = 4;

    bool is_valid = plugin_client->validate(pose_msg);

    ASSERT_FALSE(is_valid);

    pose_msg.position().x() = 6.0;

    is_valid = plugin_client->validate(pose_msg);

    ASSERT_TRUE(is_valid);

    shutdown.store(true);
    server_thread.join();

}



TEST_F(TestSuite, TestPluginAdapterLocalWithStringPluginName) {

    std::string plugin_name = "test_lib::ComponentPlugin";
    std::string host = "127.0.0.1";
    uint16_t port = get_available_port();

    rpp::PluginInfo plugin_info = TestSuite::library_manager->get_plugin_info(plugin_name);

    std::atomic<bool> server_ready(false);
    std::atomic<bool> shutdown(false);
    std::thread server_thread([&]() {
        // Kreiramo stvarni plugin i omotamo ga u server adapter
        ASSERT_TRUE(plugin_info.plugin_name == plugin_name);
        auto instance = rpp::load_cpp_plugin_from_shared_library(plugin_info);
        auto server_adapter = rpp::load_plugin_adapter_server(plugin_info, instance, host, port);
        thread_run(server_adapter, &server_ready, &shutdown);
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
    auto plugin_client = std::dynamic_pointer_cast<rpp_common::MotionController2D>(plugin_client_raw);

    rpp_common::MotionController2D::Pose2D pose_msg;
    pose_msg.position().x() = 1.0;
    pose_msg.position().y() = 2.0;
    pose_msg.yaw() = 4;

    bool is_valid = plugin_client->validate(pose_msg);

    ASSERT_FALSE(is_valid);

    pose_msg.position().x() = 6.0;

    is_valid = plugin_client->validate(pose_msg);
    ASSERT_TRUE(is_valid);

    shutdown.store(true);
    server_thread.join();
}

TEST_F(TestSuite, TestPluginAdapterWithPythonPlugin) {

    std::atomic<bool> server_ready(false);
    std::string plugin_name = "test_lib::ComponentPluginPy";
    std::string host = "127.0.0.1";
    uint16_t port = get_available_port();
    uint16_t runtime_port = get_available_port();

    rpp::PluginInfo plugin_info = TestSuite::library_manager->get_plugin_info(plugin_name);


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

    auto plugin_client = std::dynamic_pointer_cast<rpp_common::MotionController2D>(plugin_client_raw);
    auto runtime_client = std::make_unique<rpp::PluginRuntimeClient>(host, runtime_port);
    ASSERT_TRUE(plugin_client != nullptr);
    ASSERT_TRUE(runtime_client != nullptr);


    rpp_common::MotionController2D::Pose2D pose_msg;
    pose_msg.position().x() = 1.0;
    pose_msg.position().y() = 2.0;
    pose_msg.yaw() = 4;

    bool is_valid = plugin_client->validate(pose_msg);

    ASSERT_FALSE(is_valid);

    pose_msg.position().x() = 6.0;

    is_valid = plugin_client->validate(pose_msg);

    runtime_client->shutdown();
    server_thread.join();

    ASSERT_TRUE(is_valid);
}

TEST_F(TestSuite, TestPluginRuntimeAdapterLocal)
{
    std::string host = "127.0.0.1";
    uint16_t port = get_available_port();
    uint16_t port_runtime = get_available_port();

    auto plugin_info = library_manager->get_plugin_info("test_lib::ComponentPlugin");
    std::atomic<bool> server_ready(false);
    std::thread server_thread([&]() {

        rpp::RppServerHost server_host(host, port_runtime);
        auto instance = rpp::load_cpp_plugin_from_shared_library(plugin_info);
        auto server_adapter = rpp::load_plugin_adapter_server(plugin_info, instance, host, port);

        server_host.add_server(server_adapter);
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
    ASSERT_EQ(adapter_list[0].plugin_name, "test_lib::ComponentPlugin");
    ASSERT_EQ(adapter_list[0].name, "test_lib::ComponentPlugin_adapter_server");

    client->shutdown();


    server_thread.join();
}


int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    setenv("RPP_HOME", AUTOGEN_RPP_HOME, 1);

    // Pokrećemo GoogleTest najnormalnije
    return RUN_ALL_TESTS();
}
