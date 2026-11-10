#include <gtest/gtest.h>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "rpp_cpp/rpc_control_service.hpp"
#include "support/remote_lifecycle_test_helpers.hpp"
#include "support/runtime_test_helpers.hpp"

namespace
{
using rpp::testing::RemoteClientAdapter;
using rpp::testing::RemoteServerAdapter;
using rpp::testing::RemoteState;
using rpp::testing::RemoteStatefulPlugin;

template<class Threading>
rpp::RpcControlService::SlotControl test_slot_control(
  const std::shared_ptr<rpp::BasicSlotManager<Threading>> & manager,
  const std::shared_ptr<rpp::ServerAdapter> & adapter,
  std::function<rpp::CallbackResult(const std::string &, const std::string &)> loader = {})
{
  const auto name_for = [manager](const std::string & instance_id) -> std::optional<std::string> {
      for (const auto & status : manager->list()) {
        if (status.instance_id == instance_id) {return status.name;}
      }
      return std::nullopt;
    };
  const auto missing = [](const std::string & instance_id) {
      return rpp::CallbackResult::failure("Unknown hosted component instance: " + instance_id);
    };
  return {
    adapter,
    [manager] {return manager->list();},
    [loader = std::move(loader)](const std::string & path, const std::string & plugin) {
      return loader ? loader(path, plugin) :
             rpp::CallbackResult::failure("Test slot has no component loader");
    },
    [manager, name_for, missing](const std::string & instance_id) {
      auto name = name_for(instance_id);
      return name ? manager->configure(*name, instance_id) : missing(instance_id);
    },
    [manager, name_for, missing](const std::string & instance_id) {
      auto name = name_for(instance_id);
      return name ? manager->cleanup(*name, instance_id) : missing(instance_id);
    },
    [manager, name_for, missing](const std::string & instance_id, rpp::HandoverState state) {
      auto name = name_for(instance_id);
      return name ? manager->activate(*name, std::move(state), instance_id) : missing(instance_id);
    },
    [manager, name_for,
      missing](const std::string & instance_id) -> rpp::DeactivationResult<rpp::HandoverState> {
      auto name = name_for(instance_id);
      return name ? manager->deactivate(*name, std::chrono::milliseconds::max(), instance_id) :
             rpp::DeactivationResult<rpp::HandoverState>{missing(instance_id), {}};
    },
    [manager, name_for, missing](const std::string & instance_id) {
      auto name = name_for(instance_id);
      return name ? manager->unload(*name, std::chrono::milliseconds::max(),
          instance_id) : missing(instance_id);
    },
    [manager, name_for, missing](const std::string & instance_id, bool transfer_state) {
      auto name = name_for(instance_id);
      return name ? manager->reload(*name, transfer_state) : missing(instance_id);
    },
    [manager, name_for, missing](
      const std::string & source_id, const std::string & target_id, bool transfer_state) {
      auto source = name_for(source_id);
      auto target = name_for(target_id);
      if (!source) {return missing(source_id);}
      if (!target) {return missing(target_id);}
      return manager->switch_components(*source, *target, transfer_state);
    },
    [manager] {return manager->close();},
    [] {return rpp::CallbackResult{};},
  };
}

template<class Threading>
void exercise_remote_lifecycle_handover()
{
  auto io = kj::setupAsyncIo();
  auto service = kj::heap<rpp::RpcControlService>();
  auto plugin = std::make_shared<RemoteStatefulPlugin>();
  auto runtime = rpp::ComponentRuntimeAccess::plugin<Threading>(plugin);
  auto server_adapter = std::make_shared<RemoteServerAdapter>();
  RemoteClientAdapter client_adapter;
  constexpr const char * connection_name = "remote";
  constexpr const char * component_name = "root";
  const auto instance_id = runtime.get_context().get_instance_id();
  auto manager = rpp::ComponentRuntimeAccess::owning_slot(runtime).lock();
  ASSERT_TRUE(manager);
  service->register_slot(connection_name, test_slot_control(manager, server_adapter));
  rpp::runtime::PluginRuntime::Client client(kj::mv(service));

  auto list_request = client.listComponentsRequest();
  auto listed = list_request.send().wait(io.waitScope);
  ASSERT_EQ(listed.getComponents().size(), 1u);
  EXPECT_STREQ(listed.getComponents()[0].getConnectionName().cStr(), connection_name);
  EXPECT_STREQ(listed.getComponents()[0].getName().cStr(), component_name);
  EXPECT_STREQ(listed.getComponents()[0].getInstanceId().cStr(), instance_id.c_str());

  auto configure = client.configureComponentRequest();
  configure.setInstanceId(instance_id);
  EXPECT_TRUE(configure.send().wait(io.waitScope).getResult().getSuccess());

  auto activate = [&](rpp::HandoverState state) {
      auto request = client.activateComponentRequest();
      request.setInstanceId(instance_id);
      auto wire = request.initState();
      uint64_t schema_id = 0;
      EXPECT_TRUE(client_adapter.encode_lifecycle_state__(
          state, schema_id, wire.initPayload()).success);
      wire.setSchemaId(schema_id);
      return request.send().wait(io.waitScope).getResult().getSuccess();
    };
  EXPECT_TRUE(activate(rpp::HandoverState(RemoteState::Const::named("initial-state"))));

  rpp::HandoverState returned_state;
  {
    auto deactivate = client.deactivateComponentRequest();
    deactivate.setInstanceId(instance_id);
    auto stopped_response = deactivate.send().wait(io.waitScope);
    auto stopped = stopped_response.getResult();
    ASSERT_TRUE(stopped.getResult().getSuccess());
    ASSERT_TRUE(stopped.hasState());
    EXPECT_TRUE(client_adapter.decode_lifecycle_state__(
        stopped.getState().getSchemaId(), stopped.getState().getPayload(), returned_state).success);
  }  // The move-only state must outlive this RPC response.
  ASSERT_TRUE(returned_state.holds<RemoteState::Const>());
  EXPECT_EQ(returned_state.get<RemoteState::Const>().name(), "returned-state");

  EXPECT_TRUE(activate(std::move(returned_state)));
  auto final_deactivate = client.deactivateComponentRequest();
  final_deactivate.setInstanceId(instance_id);
  EXPECT_TRUE(final_deactivate.send().wait(io.waitScope).getResult().getResult().getSuccess());

  auto cleanup = client.cleanupComponentRequest();
  cleanup.setInstanceId(instance_id);
  EXPECT_TRUE(cleanup.send().wait(io.waitScope).getResult().getSuccess());
  EXPECT_EQ(plugin->activated_with, (std::vector<std::string>{"initial-state", "returned-state"}));
  EXPECT_EQ(plugin->events, (std::vector<std::string>{
      "configure", "activate", "deactivate", "activate", "deactivate", "cleanup"}));
}

template<class Threading>
void exercise_remote_slot_management()
{
  using Runtime = rpp::ComponentRuntime<Threading::concurrent>;
  std::map<std::string, std::vector<std::shared_ptr<RemoteStatefulPlugin>>> plugins;
  typename rpp::BasicSlotManager<Threading>::RuntimeFactory factory =
    [&plugins](const std::string & name) -> Runtime {
      auto plugin = std::make_shared<RemoteStatefulPlugin>();
      plugins[name].push_back(plugin);
      return rpp::ComponentRuntimeAccess::named<Threading>(
        plugin, name, {}, "testing::Remote");
    };
  auto script = rpp::ComponentRuntimeAccess::policy_slots<Threading>(
    {{"remote", "Option[testing::Remote]"}},
    [](const std::string &, const rpp::ComponentSlot &) {return true;}, factory);
  auto manager = rpp::ComponentRuntimeAccess::slot_manager(script, "remote");
  auto adapter = std::make_shared<RemoteServerAdapter>();
  auto service = kj::heap<rpp::RpcControlService>();
  service->register_slot(
    "remote", test_slot_control(
      manager, adapter,
      [manager](const std::string & path, const std::string &) {
        return manager->load(path, path);
      }));
  auto io = kj::setupAsyncIo();
  rpp::runtime::PluginRuntime::Client client(kj::mv(service));

  const auto load = [&](const std::string & name) {
      auto request = client.loadComponentRequest();
      auto component = request.initComponent();
      component.setComponentPath(name);
      component.setPluginName("testing::Remote");
      component.setConnectionName("remote");
      auto response = request.send().wait(io.waitScope);
      auto result = response.getResult();
      return rpp::CallbackResult{
      result.getSuccess(), std::string(result.getMessage().cStr())};
    };
  const auto list = [&] {
      auto request = client.listComponentsRequest();
      return request.send().wait(io.waitScope);
    };
  const auto first_loaded = load("first");
  ASSERT_TRUE(first_loaded.success) << first_loaded.message;
  const auto second_loaded = load("second");
  ASSERT_TRUE(second_loaded.success) << second_loaded.message;
  auto listed = list();
  ASSERT_EQ(listed.getComponents().size(), 2u);
  const std::string first_id = listed.getComponents()[0].getInstanceId().cStr();
  const std::string second_id = listed.getComponents()[1].getInstanceId().cStr();

  const auto configure = [&](const std::string & instance_id) {
      auto request = client.configureComponentRequest();
      request.setInstanceId(instance_id);
      return request.send().wait(io.waitScope).getResult().getSuccess();
    };
  ASSERT_TRUE(configure(first_id));
  ASSERT_TRUE(configure(second_id));
  auto activate = client.activateComponentRequest();
  activate.setInstanceId(first_id);
  activate.initState().setSchemaId(0);
  ASSERT_TRUE(activate.send().wait(io.waitScope).getResult().getSuccess());

  auto switch_request = client.switchComponentsRequest();
  switch_request.setConnectionName("remote");
  switch_request.setSourceInstanceId(first_id);
  switch_request.setTargetInstanceId(second_id);
  switch_request.setTransferState(true);
  ASSERT_TRUE(switch_request.send().wait(io.waitScope).getResult().getSuccess());
  ASSERT_EQ(plugins["second"].front()->activated_with,
    (std::vector<std::string>{"returned-state"}));

  auto unload = client.unloadComponentRequest();
  unload.setInstanceId(first_id);
  ASSERT_TRUE(unload.send().wait(io.waitScope).getResult().getSuccess());
  auto reload = client.reloadComponentRequest();
  reload.setInstanceId(second_id);
  reload.setTransferState(true);
  ASSERT_TRUE(reload.send().wait(io.waitScope).getResult().getSuccess());

  listed = list();
  ASSERT_EQ(listed.getComponents().size(), 2u);
  EXPECT_EQ(listed.getComponents()[0].getInstanceId().size(), 0u);
  EXPECT_STRNE(listed.getComponents()[1].getInstanceId().cStr(), second_id.c_str());
  ASSERT_EQ(plugins["second"].size(), 2u);
  EXPECT_EQ(plugins["second"].back()->activated_with, (std::vector<std::string>{"returned-state"}));
}
}  // namespace

TEST(RpcControlService, AddressesComponentInstancesWithinHostedSlots)
{
  auto io = kj::setupAsyncIo();
  auto server = kj::heap<rpp::RpcControlService>();
  auto first_plugin = std::make_shared<RemoteStatefulPlugin>();
  auto second_plugin = std::make_shared<RemoteStatefulPlugin>();
  auto first = rpp::ComponentRuntimeAccess::plugin(first_plugin);
  auto second = rpp::ComponentRuntimeAccess::plugin(second_plugin);
  auto first_manager = rpp::ComponentRuntimeAccess::owning_slot(first).lock();
  auto second_manager = rpp::ComponentRuntimeAccess::owning_slot(second).lock();
  auto adapter = std::make_shared<RemoteServerAdapter>();
  ASSERT_TRUE(first_manager);
  ASSERT_TRUE(second_manager);
  server->register_slot("first", test_slot_control(first_manager, adapter));
  server->register_slot("second", test_slot_control(second_manager, adapter));
  rpp::runtime::PluginRuntime::Client client(kj::mv(server));
  auto version = client.getProtocolVersionRequest().send().wait(io.waitScope);
  EXPECT_EQ(version.getVersion(), rpp::runtime::PROTOCOL_VERSION);
  auto list = client.listComponentsRequest().send().wait(io.waitScope);
  ASSERT_EQ(list.getComponents().size(), 2u);
  EXPECT_STREQ(list.getComponents()[0].getConnectionName().cStr(), "first");
  EXPECT_STREQ(list.getComponents()[1].getConnectionName().cStr(), "second");
  auto missing = client.configureComponentRequest();
  missing.setInstanceId("missing");
  EXPECT_FALSE(missing.send().wait(io.waitScope).getResult().getSuccess());
  auto request = client.configureComponentRequest();
  request.setInstanceId(first.get_context().get_instance_id());
  EXPECT_TRUE(request.send().wait(io.waitScope).getResult().getSuccess());
  EXPECT_EQ(first_plugin->events, (std::vector<std::string>{"configure"}));
  EXPECT_TRUE(second_plugin->events.empty());
}

TEST(RpcControlService, CallbackExceptionsBecomeLifecycleFailures)
{
  auto io = kj::setupAsyncIo();
  auto server = kj::heap<rpp::RpcControlService>();
  int attempts = 0;
  auto adapter = std::make_shared<RemoteServerAdapter>();
  server->register_slot("root", {
    adapter,
    [] {
      return std::vector<rpp::ComponentStatus>{
      {"root", "", "root-instance", rpp::LifecycleState::UNCONFIGURED, {}, {}, false, 0}};
    },
    [](const std::string &, const std::string &) {return rpp::CallbackResult{};},
    [&](const std::string &) -> rpp::CallbackResult {
      ++attempts;
      throw std::runtime_error("configuration rejected");
    },
    [](const std::string &) {return rpp::CallbackResult{};},
    [](const std::string &, rpp::HandoverState) {return rpp::CallbackResult{};},
    [](const std::string &) {return rpp::DeactivationResult<rpp::HandoverState>{};},
    [](const std::string &) {return rpp::CallbackResult{};},
    [](const std::string &, bool) {return rpp::CallbackResult{};},
    [](const std::string &, const std::string &, bool) {return rpp::CallbackResult{};},
    [] {return rpp::CallbackResult{};},
    [] {return rpp::CallbackResult{};},
  });
  rpp::runtime::PluginRuntime::Client client(kj::mv(server));
  auto request = client.configureComponentRequest();
  request.setInstanceId("root-instance");
  auto response = request.send().wait(io.waitScope);
  auto result = response.getResult();
  EXPECT_FALSE(result.getSuccess());
  EXPECT_STREQ(result.getMessage().cStr(), "configuration rejected");
  EXPECT_EQ(attempts, 1);
}

TEST(RpcControlService, AdaptedLifecycleHandoverSingleThreadedHappyPath)
{
  exercise_remote_lifecycle_handover<rpp::SingleThreaded>();
}

TEST(RpcControlService, AdaptedLifecycleHandoverConcurrentHappyPath)
{
  exercise_remote_lifecycle_handover<rpp::Concurrent>();
}

TEST(RpcControlService, RemoteSlotManagementSingleThreadedHappyPath)
{
  exercise_remote_slot_management<rpp::SingleThreaded>();
}

TEST(RpcControlService, RemoteSlotManagementConcurrentHappyPath)
{
  exercise_remote_slot_management<rpp::Concurrent>();
}

TEST(RpcControlService, NativeLifecycleHappyPath)
{
  auto plugin = std::make_shared<RemoteStatefulPlugin>();
  auto runtime = rpp::ComponentRuntimeAccess::plugin<rpp::SingleThreaded>(plugin);

  EXPECT_FALSE(rpp::ComponentRuntimeAccess::has_external_component_process(runtime));
  ASSERT_TRUE(runtime.configure().success);
  ASSERT_TRUE(runtime.activate(rpp::HandoverState(RemoteState::Const::named(
    "native-state"))).success);
  ASSERT_TRUE(runtime.deactivate().result.success);
  ASSERT_TRUE(runtime.cleanup().success);
  EXPECT_EQ(plugin->activated_with, (std::vector<std::string>{"native-state"}));
}
