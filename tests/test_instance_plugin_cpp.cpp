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
#include "rpp_cpp/child_process.hpp"
#include "rpp_cpp/component_call_executor.hpp"
#include "rpp_cpp/parameter_handler.hpp"

#include "rpp_plugin_types/rpp_testing/MotionController2D.hpp"
#include "rpp_plugin_types/rpp_testing/DisturbanceGenerator2D.hpp"
#include "rpp_plugin_types/rpp_testing/TestInterfaceAll.hpp"
#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <capnp/ez-rpc.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <atomic>


TEST(DataModelTest, ComponentRecordParsesMultipleSubcomponentsInOneSlot)
{
    const auto description = nlohmann::json::parse(R"({
        "Id": "parent-id",
        "Subcomponents": {
            "controllers": [
                {
                    "Id": "first-id",
                    "PluginName": "test::First"
                },
                {
                    "Id": "second-id",
                    "PluginName": "test::Second"
                }
            ]
        }
    })");

    const auto record =
        rpp::ComponentRecord::from_json(description, "/parts/parent-id");

    const auto& controllers = record.subcomponents.at("controllers");
    ASSERT_EQ(controllers.size(), 2U);
    EXPECT_EQ(controllers[0].id, "first-id");
    EXPECT_EQ(controllers[1].id, "second-id");
}

TEST(DataModelTest, ScriptDescriptionParsesConfigurations)
{
    const auto description_json = nlohmann::json::parse(R"({
        "Configurations": {
            "Default": {
                "Components": {
                    "controllers": [{
                        "Id": "controller-id",
                        "PluginName": "test::Controller"
                    }]
                }
            }
        },
        "ActiveConfiguration": "Default"
    })");

    const auto description = rpp::ScriptDescription::from_json(
        description_json, "/workspace/.rppws/script_descriptions/example.json");

    EXPECT_EQ(description.active_configuration, "Default");
    const auto& controllers = description.configurations.at("Default").at(
        "controllers");
    ASSERT_EQ(controllers.size(), 1U);
    EXPECT_EQ(controllers.front().id, "controller-id");
    EXPECT_EQ(controllers.front().plugin_name, "test::Controller");
}

TEST(ComponentCallExecutorTest, RunsCallsAndPropagatesErrors)
{
    rpp::ComponentCallExecutor executor;
    executor.start();

    const auto caller_thread = std::this_thread::get_id();
    EXPECT_NE(executor.call([](kj::AsyncIoContext&) {
        return std::this_thread::get_id();
    }), caller_thread);

    bool void_call_completed = false;
    executor.call([&void_call_completed](kj::AsyncIoContext&) {
        void_call_completed = true;
    });
    EXPECT_TRUE(void_call_completed);

    EXPECT_THROW(executor.call([](kj::AsyncIoContext&) -> int {
        throw std::logic_error("expected failure");
    }), std::logic_error);

    EXPECT_THROW(executor.call([&executor](kj::AsyncIoContext&) {
        executor.call([](kj::AsyncIoContext&) {});
    }), std::runtime_error);

    executor.stop();
    EXPECT_THROW(executor.call([](kj::AsyncIoContext&) {}), std::runtime_error);
}


class NativePluginTest : public ::testing::Test {

public:
    static std::string test_lib;
    static std::string rpp_home_dir;
    static std::string test_data_dir;
    static std::unique_ptr<rpp::RppDataManager> data_manager;
    static bool initialization_successful;

protected:
    static void SetUpTestSuite() {
        rpp_home_dir = std::getenv("RPP_HOME");
        rpp::RPP_HOME = rpp_home_dir;
        test_data_dir = std::getenv("TEST_DATA_DIR");
        test_lib = "test_lib";
        data_manager = std::make_unique<rpp::RppDataManager>(rpp_home_dir);
        initialization_successful = true;
    }

    static void TearDownTestSuite() {
        // std::filesystem::remove_all(tmp_dir);
    }

    void thread_run(rpp::ServerAdapter* server_adapter,
        std::atomic<bool>* server_ready, std::atomic<bool>* shutdown,
        const std::string& host, uint16_t port)
    {
        auto io = kj::setupAsyncIo();

        server_adapter->start_adapter_server__(io, host, port);

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

std::unique_ptr<rpp::RppDataManager> NativePluginTest::data_manager = nullptr;
std::string NativePluginTest::test_lib = "";
std::string NativePluginTest::test_data_dir = "";
std::string NativePluginTest::rpp_home_dir = "";
bool NativePluginTest::initialization_successful = false;

namespace rpp {

class ComponentContextAccess final {
public:
    static ComponentContext with_logger_and_parameters(
            std::shared_ptr<RppLogger> logger,
            std::map<std::string, params::ParameterValue> parameters)
    {
        ComponentContext context(std::move(logger));
        context.parameters_ = params::Parameters(std::move(parameters));
        return context;
    }
};

}  // namespace rpp

class PythonAdapterTest : public NativePluginTest {};


void check_all_interface_types_plugin_call(
    rpp_testing::TestInterfaceAll* plugin_client)
{
    ASSERT_TRUE(plugin_client != nullptr);

    plugin_client->funcEmpty();

    auto double_result = plugin_client->funcWithSimpleParams(2, true);

    ASSERT_DOUBLE_EQ(double_result, 4.0);

    auto test_struct1 = rpp_schema::rpp_testing::TestStruct1();
    test_struct1.x() = 1.0;
    test_struct1.y() = 2.0;
    test_struct1.theta() = 3.0;

    auto test_struct2 = rpp_schema::rpp_testing::TestStruct2();
    test_struct2.linear() = 4.0;
    test_struct2.angular() = 5.0;
    test_struct2.struct1().x() = 2.0;
    test_struct2.struct1().y() = 3.0;
    test_struct2.struct1().theta() = 4.0;

    ASSERT_DOUBLE_EQ(test_struct2.struct1().x(), 2.0);
    ASSERT_DOUBLE_EQ(test_struct2.struct1().y(), 3.0);
    ASSERT_DOUBLE_EQ(test_struct2.struct1().theta(), 4.0);

    auto test_struct_ret = plugin_client->funcWithStructParam(test_struct1, test_struct2);

    ASSERT_DOUBLE_EQ(test_struct_ret.x(), 5.0);
    ASSERT_DOUBLE_EQ(test_struct_ret.y(), 7.0);

    rpp::List<double> list_param;
    list_param.resize(3);
    list_param[0] = 1.0;
    list_param[1] = 2.0;

    rpp_schema::rpp_testing::TestStruct1::List list_struct_param;
    list_struct_param.resize(2);
    list_struct_param[0].x() = 1.0;
    list_struct_param[0].y() = 2.0;
    list_struct_param[1].x() = 3.0;
    list_struct_param[1].y() = 4.0;

    auto list_result = plugin_client->funcWithListParam(list_param, list_struct_param);

    ASSERT_EQ(list_result.size(), 5);
    ASSERT_DOUBLE_EQ(list_result[0], 1.0);
    ASSERT_DOUBLE_EQ(list_result[1], 2.0);
    ASSERT_DOUBLE_EQ(list_result[2], 0.0);
    ASSERT_DOUBLE_EQ(list_result[3], 3.0);
    ASSERT_DOUBLE_EQ(list_result[4], 7.0);


    auto list_struct_result = plugin_client->funcWithListOfStructParam(list_struct_param);

    ASSERT_EQ(list_struct_result.size(), 2);
    ASSERT_DOUBLE_EQ(list_struct_result[0].linear(), 3.0);
    ASSERT_DOUBLE_EQ(list_struct_result[0].angular(), 0.0);
    ASSERT_DOUBLE_EQ(list_struct_result[1].linear(), 7.0);
    ASSERT_DOUBLE_EQ(list_struct_result[1].angular(), 0.0);


    auto [double_val, bool_val] = plugin_client->funcWithMultipleSimpleReturns();

    ASSERT_DOUBLE_EQ(double_val, 3.14);
    ASSERT_TRUE(bool_val);

    auto [struct1_val, struct2_val] = plugin_client->funcWithMultipleStructReturns();

    ASSERT_DOUBLE_EQ(struct1_val.x(), 1.0);
    ASSERT_DOUBLE_EQ(struct1_val.y(), 2.0);
    ASSERT_DOUBLE_EQ(struct2_val.linear(), 4.0);
    ASSERT_DOUBLE_EQ(struct2_val.angular(), 5.0);
    ASSERT_DOUBLE_EQ(struct2_val.struct1().x(), 2.0);
    ASSERT_DOUBLE_EQ(struct2_val.struct1().y(), 3.0);

    auto [list_double_val, list_struct_val] = plugin_client->funcWithMultipleListReturns();

    ASSERT_EQ(list_double_val.size(), 3);
    ASSERT_EQ(list_struct_val.size(), 2);

    ASSERT_DOUBLE_EQ(list_double_val[0], 1.0);
    ASSERT_DOUBLE_EQ(list_double_val[1], 2.0);
    ASSERT_DOUBLE_EQ(list_double_val[2], 3.0);

    ASSERT_DOUBLE_EQ(list_struct_val[0].x(), 4.0);
    ASSERT_DOUBLE_EQ(list_struct_val[0].y(), 5.0);
    ASSERT_DOUBLE_EQ(list_struct_val[1].x(), 7.0);
    ASSERT_DOUBLE_EQ(list_struct_val[1].y(), 8.0);

}

TEST_F(NativePluginTest, TestDataInterface) {
    std::string plugin_name = "test_lib::DataInterfaceCpp";

    rpp::PluginInfo plugin_info = NativePluginTest::data_manager->get_plugin_info_from_lib(plugin_name);

    auto plugin =
        rpp::load_cpp_plugin_from_shared_library
            <rpp_testing::DisturbanceGenerator2D>(plugin_info);

    ASSERT_TRUE(plugin != nullptr);

    auto data = plugin->getData();

    ASSERT_EQ(data.size(), 5);
    ASSERT_EQ(data[0], 1);
    ASSERT_EQ(data[1], 2);
    ASSERT_EQ(data[2], 3);
    ASSERT_EQ(data[3], 4);
    ASSERT_EQ(data[4], 5);

    bool set_result = plugin->setData(data);

    ASSERT_TRUE(set_result);

    // Test with incorrect data
    rpp::Data incorrect_data;
    incorrect_data.resize(4); // Incorrect size
    set_result = plugin->setData(incorrect_data);
    ASSERT_FALSE(set_result);
}



TEST_F(NativePluginTest, TestMsgAsStruct) {
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


TEST_F(NativePluginTest, TestInstanceByGenericInterface) {

    std::string plugin_name = "test_lib::ComponentPluginSimpleCpp";

    rpp::PluginInfo plugin_info = NativePluginTest::data_manager->get_plugin_info_from_lib(plugin_name);

    auto instance = rpp::load_cpp_plugin_from_shared_library<rpp::Plugin>(plugin_info);
    ASSERT_TRUE(instance != nullptr);
    rpp_testing::MotionController2D* motion_controller =
        dynamic_cast<rpp_testing::MotionController2D*>(instance.get());

    ASSERT_TRUE(motion_controller != nullptr);

}

TEST_F(NativePluginTest, TestSimpleInstancePlugin) {
    // Create PluginInfo for the C++ plugin

    // assert initialization in NativePluginTest::SetUpTestSuite() was successful

    std::string plugin_name = "test_lib::ComponentPluginSimpleCpp";

    rpp::PluginInfo plugin_info = NativePluginTest::data_manager->get_plugin_info_from_lib(plugin_name);

    auto plugin =
        rpp::load_cpp_plugin_from_shared_library
            <rpp_testing::MotionController2D>(plugin_info);

    auto logger = std::make_shared<rpp::RppLogger>(
        rpp::LoggerOptions{rpp::LogLevel::DEBUG, "test_simple_instance_plugin"});
    auto context = rpp::ComponentContextAccess::with_logger_and_parameters(
        logger, {{"validate_threshold", 5.0}});
    plugin->initialize(context);

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

TEST_F(NativePluginTest, TestAllInterfaceTypesInstancePlugin) {
    std::string plugin_name = "test_lib::AllInterfaceTypesCpp";

    rpp::PluginInfo plugin_info = NativePluginTest::data_manager->get_plugin_info_from_lib(plugin_name);

    auto plugin =
        rpp::load_cpp_plugin_from_shared_library
            <rpp_testing::TestInterfaceAll>(plugin_info);

    return check_all_interface_types_plugin_call(plugin.get());
}



TEST_F(NativePluginTest, TestPluginAdapterLocal)
{
    std::string host = "127.0.0.1";
    uint16_t port = get_available_port();
    std::string plugin_name = "test_lib::ComponentPluginSimpleCpp";
    std::atomic<bool> server_ready(false);
    std::atomic<bool> shutdown(false);

    std::thread server_thread([&]() {
        // Kreiramo stvarni plugin i omotamo ga u server adapter
        rpp::PluginInfo plugin_info = NativePluginTest::data_manager->get_plugin_info_from_lib(plugin_name);
        ASSERT_TRUE(plugin_info.plugin_name == plugin_name);

        auto instance =
            rpp::load_cpp_plugin_from_shared_library
                <rpp_testing::MotionController2D>(plugin_info);

        auto odom_msg = rpp_testing::MotionController2D::Odometry2D();
        odom_msg.pose().position().x() = 6.0;

        auto is_valid = instance->validate(odom_msg);
        ASSERT_TRUE(is_valid);

        auto server_adapter =
            rpp::load_plugin_adapter_server<rpp_testing::MotionController2D>(
                std::move(instance));
        try{
            thread_run(server_adapter.get(), &server_ready, &shutdown, host, port);
        }
        catch (const std::exception& e) {
            FAIL() << "Exception in server thread: " << e.what();
        }
        catch (...) {
            FAIL() << "Unknown exception in server thread.";
        }
    });

    int max_wait_time_ms = 1000; // Maksimalno vrijeme čekanja u milisekundama
    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        max_wait_time_ms -= 10;
        if (max_wait_time_ms <= 0) {
            FAIL() << "Server not ready after waiting for 1 second.";
        }
    }

    auto plugin_client = \
        rpp::load_plugin_adapter_client<rpp_testing::MotionController2D>();

    ASSERT_TRUE(plugin_client != nullptr);

    rpp::ComponentCallExecutor client_executor;
    client_executor.start();
    auto context = client_executor.call([&](kj::AsyncIoContext& io) {
        return std::make_unique<rpp::RppRuntimeClientContext>(
            host, port, std::chrono::seconds(5), &io);
    });
    ASSERT_TRUE(client_executor.call([&](kj::AsyncIoContext&) {
        return plugin_client->connect_adapter_client__(
            *context, std::ref(client_executor));
    }));

    rpp_testing::MotionController2D::Odometry2D odom_msg;
    odom_msg.pose().position().x() = 1.0;
    odom_msg.pose().position().y() = 2.0;
    odom_msg.pose().yaw() = 4;

    bool is_valid = plugin_client->validate(odom_msg);

    ASSERT_FALSE(is_valid);

    odom_msg.pose().position().x() = 6.0;

    is_valid = plugin_client->validate(odom_msg);

    ASSERT_TRUE(is_valid);

    client_executor.call([&](kj::AsyncIoContext&) {
        plugin_client.reset();
        context.reset();
    });
    client_executor.stop();
    shutdown.store(true);
    server_thread.join();
}


TEST_F(NativePluginTest, TestPluginAdapterLocalWithAllInterfaceTypes)
{
    std::string host = "127.0.0.1";
    uint16_t port = get_available_port();
    std::string plugin_name = "test_lib::AllInterfaceTypesCpp";
    std::atomic<bool> server_ready(false);
    std::atomic<bool> shutdown(false);

    std::thread server_thread([&]() {
        // Kreiramo stvarni plugin i omotamo ga u server adapter
        rpp::PluginInfo plugin_info = NativePluginTest::data_manager->get_plugin_info_from_lib(plugin_name);
        ASSERT_TRUE(plugin_info.plugin_name == plugin_name);

        auto instance =
            rpp::load_cpp_plugin_from_shared_library
                <rpp_testing::TestInterfaceAll>(plugin_info);

        auto server_adapter =
            rpp::load_plugin_adapter_server<rpp_testing::TestInterfaceAll>(
                std::move(instance));
        try{
            thread_run(server_adapter.get(), &server_ready, &shutdown, host, port);
        }
        catch (const std::exception& e) {
            FAIL() << "Exception in server thread: " << e.what();
        }
        catch (...) {
            FAIL() << "Unknown exception in server thread.";
        }
    });

    int max_wait_time_ms = 1000; // Maksimalno vrijeme čekanja u milisekundama
    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        max_wait_time_ms -= 10;
        if (max_wait_time_ms <= 0) {
            FAIL() << "Server not ready after waiting for 1 second.";
        }
    }

    auto plugin_client = \
        rpp::load_plugin_adapter_client<rpp_testing::TestInterfaceAll>();

    ASSERT_TRUE(plugin_client != nullptr);

    rpp::ComponentCallExecutor client_executor;
    client_executor.start();
    auto context = client_executor.call([&](kj::AsyncIoContext& io) {
        return std::make_unique<rpp::RppRuntimeClientContext>(
            host, port, std::chrono::seconds(5), &io);
    });
    ASSERT_TRUE(client_executor.call([&](kj::AsyncIoContext&) {
        return plugin_client->connect_adapter_client__(
            *context, std::ref(client_executor));
    }));

    check_all_interface_types_plugin_call(plugin_client.get());

    client_executor.call([&](kj::AsyncIoContext&) {
        plugin_client.reset();
        context.reset();
    });
    client_executor.stop();
    shutdown.store(true);
    server_thread.join();
}


TEST_F(NativePluginTest, TestPluginAdapterLocalWithStringPluginName) {

    std::string plugin_name = "test_lib::ComponentPluginSimpleCpp";
    std::string host = "127.0.0.1";
    uint16_t port = get_available_port();

    rpp::PluginInfo plugin_info = NativePluginTest::data_manager->get_plugin_info_from_lib(plugin_name);

    std::atomic<bool> server_ready(false);
    std::atomic<bool> shutdown(false);
    std::thread server_thread([&]() {
        // Kreiramo stvarni plugin i omotamo ga u server adapter
        ASSERT_TRUE(plugin_info.plugin_name == plugin_name);
        auto instance = rpp::load_cpp_plugin_from_shared_library(plugin_info);
        auto server_adapter = rpp::load_plugin_adapter_server(plugin_info, std::move(instance));
        thread_run(server_adapter.get(), &server_ready, &shutdown, host, port);
    });

    int max_wait_time_ms = 5000;
    while (!server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        max_wait_time_ms -= 10;
        if (max_wait_time_ms <= 0) {
            FAIL() << "Server not ready after waiting for 5 seconds.";
        }
    }
    auto plugin_client_raw = \
    rpp::load_plugin_adapter_client(plugin_info);
    ASSERT_TRUE(plugin_client_raw != nullptr);
    auto plugin_client = dynamic_cast<rpp_testing::MotionController2D*>(plugin_client_raw.get());
    ASSERT_TRUE(plugin_client != nullptr);

    rpp::ComponentCallExecutor client_executor;
    client_executor.start();
    auto context = client_executor.call([&](kj::AsyncIoContext& io) {
        return std::make_unique<rpp::RppRuntimeClientContext>(
            host, port, std::chrono::seconds(5), &io);
    });
    ASSERT_TRUE(client_executor.call([&](kj::AsyncIoContext&) {
        return plugin_client_raw->connect_adapter_client__(
            *context, std::ref(client_executor));
    }));

    rpp_testing::MotionController2D::Odometry2D odom_msg;
    odom_msg.pose().position().x() = 1.0;
    odom_msg.pose().position().y() = 2.0;
    odom_msg.pose().yaw() = 4;

    bool is_valid = plugin_client->validate(odom_msg);

    ASSERT_FALSE(is_valid);

    odom_msg.pose().position().x() = 6.0;

    is_valid = plugin_client->validate(odom_msg);
    ASSERT_TRUE(is_valid);

    client_executor.call([&](kj::AsyncIoContext&) {
        plugin_client_raw.reset();
        context.reset();
    });
    client_executor.stop();
    shutdown.store(true);
    server_thread.join();
}

TEST_F(PythonAdapterTest, TestPluginAdapterWithPythonPlugin) {

    std::string plugin_name = "test_lib::ComponentPluginSimplePy";
    std::string host = "127.0.0.1";
    uint16_t port = get_available_port();

    rpp::PluginInfo plugin_info = NativePluginTest::data_manager->get_plugin_info_from_lib(plugin_name);

    std::vector<std::string> command{
        "rpp_component_server_python", "--host", host,
        "--port", std::to_string(port), "--home", NativePluginTest::rpp_home_dir,
        "--plugin", plugin_name, "--path",
        NativePluginTest::test_data_dir + "/test_component_simple_py",
        "--conn", "test_connection",
    };

    rpp::ChildProcess process(command);
    process.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    auto plugin_client_raw = \
        rpp::load_plugin_adapter_client(plugin_info, "test_name", "test_connection");
    auto plugin_client = dynamic_cast<rpp_testing::MotionController2D*>(plugin_client_raw.get());
    ASSERT_TRUE(plugin_client != nullptr);

    rpp::ComponentCallExecutor client_executor;
    client_executor.start();
    auto context = client_executor.call([&](kj::AsyncIoContext& io) {
        return std::make_unique<rpp::RppRuntimeClientContext>(
            host, port, std::chrono::seconds(5), &io);
    });
    ASSERT_TRUE(client_executor.call([&](kj::AsyncIoContext&) {
        return plugin_client_raw->connect_adapter_client__(
            *context, std::ref(client_executor));
    }));
    auto runtime_client = client_executor.call([&](kj::AsyncIoContext&) {
        return std::make_unique<rpp::PluginRuntimeClient>(*context);
    });


    rpp_testing::MotionController2D::Odometry2D odom_msg;
    odom_msg.pose().position().x() = 1.0;
    odom_msg.pose().position().y() = 2.0;
    odom_msg.pose().yaw() = 4;

    bool is_valid = plugin_client->validate(odom_msg);

    ASSERT_FALSE(is_valid);

    odom_msg.pose().position().x() = 6.0;

    is_valid = plugin_client->validate(odom_msg);

    client_executor.call([&](kj::AsyncIoContext&) {
        runtime_client->shutdown();
    });
    client_executor.call([&](kj::AsyncIoContext&) {
        runtime_client.reset();
        plugin_client_raw.reset();
        context.reset();
    });
    client_executor.stop();
    try {
        process.wait(std::chrono::seconds(5));
    }
    catch (const std::exception&) {
        process.terminate();
        process.wait();
        ADD_FAILURE() << "Python component server did not exit within 5 seconds.";
    }

    ASSERT_TRUE(is_valid);
}

TEST_F(NativePluginTest, TestPluginRuntimeAdapterLocal)
{
    std::string host = "127.0.0.1";
    uint16_t port = get_available_port();

    auto plugin_info = data_manager->get_plugin_info_from_lib("test_lib::ComponentPluginSimpleCpp");
    std::atomic<bool> server_ready(false);
    std::thread server_thread([&]() {

        rpp::RppServerHost server_host(host, port);
        auto instance = rpp::load_cpp_plugin_from_shared_library(plugin_info);
        auto server_adapter =
            rpp::load_plugin_adapter_server(
                plugin_info, std::move(instance),
                "test_name", "test_connection_name");

        server_host.add_server(std::move(server_adapter));
        server_ready.store(true);
        server_host.run();
    });

    int max_wait_time_ms = 3000;
    while (!server_ready.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        max_wait_time_ms -= 10;
        if (max_wait_time_ms <= 0) {
            FAIL() << "Server not ready after waiting for 1 second.";
        }
    }

    auto client_raw = rpp::load_plugin_adapter_client(plugin_info, "test_name", "test_connection_name");
    auto client = dynamic_cast<rpp_testing::MotionController2D*>(client_raw.get());
    ASSERT_TRUE(client != nullptr);

    rpp::ComponentCallExecutor client_executor;
    client_executor.start();
    auto context = client_executor.call([&](kj::AsyncIoContext& io) {
        return std::make_unique<rpp::RppRuntimeClientContext>(
            host, port, std::chrono::seconds(5), &io);
    });
    ASSERT_TRUE(client_executor.call([&](kj::AsyncIoContext&) {
        return client_raw->connect_adapter_client__(
            *context, std::ref(client_executor));
    }));
    auto runtime_client = client_executor.call([&](kj::AsyncIoContext&) {
        return std::make_unique<rpp::PluginRuntimeClient>(*context);
    });

    client_executor.call([&](kj::AsyncIoContext&) {
        runtime_client->ping();
    });

    auto adapter_list = client_executor.call([&](kj::AsyncIoContext&) {
        return runtime_client->listAdapters();
    });

    ASSERT_EQ(adapter_list.size(), 1);
    ASSERT_EQ(adapter_list[0].plugin_name, "test_lib::ComponentPluginSimpleCpp");
    ASSERT_EQ(adapter_list[0].name, "test_name");

    rpp_testing::MotionController2D::Odometry2D odom_msg;
    odom_msg.pose().position().x() = 1.0;
    odom_msg.pose().position().y() = 2.0;
    odom_msg.pose().yaw() = 4;

    bool is_valid = client->validate(odom_msg);

    ASSERT_FALSE(is_valid);

    client_executor.call([&](kj::AsyncIoContext&) {
        runtime_client->shutdown();
    });
    client_executor.call([&](kj::AsyncIoContext&) {
        runtime_client.reset();
        client_raw.reset();
        context.reset();
    });
    client_executor.stop();


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


TEST_F(NativePluginTest, TestComplexInstancePluginUsingContextBuilder)
{
    std::string component_folder = NativePluginTest::test_data_dir + "/test_component_cpp";

    auto context = rpp::ComponentContextBuilder(
        *NativePluginTest::data_manager)
        .build_component_from_path(component_folder);

    auto subcomponent_list = context.list_subcomponents();
    ASSERT_EQ(subcomponent_list.size(), 1);
    ASSERT_EQ(subcomponent_list[0], "ctl1");

    auto subcomponent = context.get_component<rpp_testing::MotionController2D>("ctl1");

    auto odom_msg = rpp_testing::MotionController2D::Odometry2D();
    odom_msg.pose().position().x() = 1.0;
    odom_msg.pose().position().y() = 2.0;
    odom_msg.pose().yaw() = 1.57;
    auto result = subcomponent->step(odom_msg, 0.1);
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


TEST_F(PythonAdapterTest, TestComponentCppWithPythonSubc) {

    std::string component_folder = NativePluginTest::test_data_dir + "/test_component_cpp_with_python_subc";

    auto context = rpp::ComponentContextBuilder(
        *NativePluginTest::data_manager)
        .build_component_from_path(component_folder);

    context.initialize();

    auto python_subcomponent =
        context.get_component<rpp_testing::MotionController2D>("ctl_1");
    auto odom_msg = rpp_testing::MotionController2D::Odometry2D();
    odom_msg.pose().position().x() = 1.0;
    odom_msg.pose().position().y() = 2.0;
    odom_msg.pose().yaw() = 1.57;

    // The Python component rejects this state.
    ASSERT_FALSE(python_subcomponent->validate(odom_msg));

    odom_msg.pose().position().x() = 6.0;
    // Python accepts it, but its C++ child rejects it at the higher threshold.
    ASSERT_FALSE(python_subcomponent->validate(odom_msg));

    odom_msg.pose().position().x() = 11.0;
    // Both the Python component and the C++ child now accept it.
    ASSERT_TRUE(python_subcomponent->validate(odom_msg));
}



int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    setenv("RPP_HOME", AUTOGEN_RPP_HOME, 1);
    setenv("TEST_DATA_DIR", TEST_DATA_DIR, 1);

    // Pokrećemo GoogleTest najnormalnije
    return RUN_ALL_TESTS();
}
