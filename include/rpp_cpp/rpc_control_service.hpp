#pragma once
#include <chrono>
#include <exception>
#include <string>
#include <kj/debug.h>
#include <vector>
#include <kj/async-io.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include "plugin_runtime.capnp.h"
#include "adapter_bases.hpp"
#include "logger.hpp"
#include "component_status.hpp"

namespace rpp
{


// RPC control endpoint. Each connection identifies one host-owned slot; the
// host supplies native-equivalent slot-manager operations through SlotControl.
class RpcControlService final : public runtime::PluginRuntime::Server
{
public:
  using ServerAdapterPtr = std::shared_ptr<ServerAdapter>;
  using ServerAdapterMap = std::map<std::string, ServerAdapterPtr>;

  struct SlotControl
  {
    ServerAdapterPtr adapter;
    std::function<std::vector<ComponentStatus>()> list;
    std::function<CallbackResult(const std::string &, const std::string &)> load;
    std::function<CallbackResult(const std::string &)> configure;
    std::function<CallbackResult(const std::string &)> cleanup;
    std::function<CallbackResult(const std::string &, HandoverState)> activate;
    std::function<DeactivationResult<HandoverState>(const std::string &)> deactivate;
    std::function<CallbackResult(const std::string &)> unload;
    std::function<CallbackResult(const std::string &, bool)> reload;
    std::function<CallbackResult(
      const std::string &, const std::string &, bool)> switch_components;
    std::function<CallbackResult()> close;
    // Rebind the generated adapter to the selected runtime before handing out
    // its typed capability.
    std::function<CallbackResult()> refresh_adapter;
  };

private:
  std::shared_ptr<ServerAdapterMap> adapters_;
  std::function<void()> on_shutdown_callback_;
  std::shared_ptr<RppLogger> logger_;
  std::map<std::string, SlotControl> slots_;

  static CallbackResult invoke(const std::function<CallbackResult()> & callback)
  {
    try {return callback();} catch (const std::exception & error) {
      return CallbackResult::failure(error.what());
    } catch (...) {return CallbackResult::failure("Unknown lifecycle callback exception");}
  }

  SlotControl * slot(const std::string & connection_name)
  {
    auto found = slots_.find(connection_name);
    return found == slots_.end() ? nullptr : &found->second;
  }

  SlotControl * slot_for_instance(const std::string & instance_id)
  {
    for (auto & [connection_name, control] : slots_) {
      (void)connection_name;
      for (const auto & component : control.list()) {
        if (component.instance_id == instance_id) {return &control;}
      }
    }
    return nullptr;
  }

  static const char * state_name(LifecycleState state)
  {
    switch (state) {
      case LifecycleState::UNLOADED: return "unloaded";
      case LifecycleState::UNCONFIGURED: return "unconfigured";
      case LifecycleState::INACTIVE: return "inactive";
      case LifecycleState::ACTIVE: return "active";
      case LifecycleState::ERROR: return "error";
    }
    return "unknown";
  }

  static void write_result(runtime::LifecycleResult::Builder output, const CallbackResult & result)
  {
    output.setSuccess(result.success);
    output.setMessage(result.message);
  }

public:
  explicit RpcControlService(std::shared_ptr<RppLogger> logger = nullptr)
  : adapters_(std::make_shared<ServerAdapterMap>()),
    logger_(logger ? std::move(logger) :
      std::make_shared<RppLogger>("rpp_rpc_control_service"))
  {}

  void set_adapters(const ServerAdapterMap & adapters)
  {
    adapters_ = std::make_shared<ServerAdapterMap>(adapters);
  }

  void set_on_shutdown_callback(std::function<void()> callback)
  {
    on_shutdown_callback_ = std::move(callback);
  }

  void register_slot(const std::string & connection_name, SlotControl control)
  {
    if (connection_name.empty()) {
      throw std::invalid_argument("Hosted slot connection name is required");
    }
    if (!control.adapter || !control.list || !control.load || !control.configure ||
      !control.cleanup || !control.activate || !control.deactivate || !control.unload ||
      !control.reload || !control.switch_components || !control.close ||
      !control.refresh_adapter)
    {
      throw std::invalid_argument("Hosted slot control is incomplete: " + connection_name);
    }
    if (!slots_.emplace(connection_name, std::move(control)).second) {
      throw std::invalid_argument("Duplicate hosted slot connection: " + connection_name);
    }
  }

  CallbackResult close_slot(const std::string & connection_name)
  {
    auto control = slot(connection_name);
    return control ? invoke(control->close) :
      CallbackResult::failure("Unknown hosted slot connection: " + connection_name);
  }

  ::kj::Promise<void> getProtocolVersion(GetProtocolVersionContext context) override
  {
    context.getResults().setVersion(runtime::PROTOCOL_VERSION);
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> configureComponent(ConfigureComponentContext context) override
  {
    const auto instance_id = std::string(context.getParams().getInstanceId().cStr());
    auto control = slot_for_instance(instance_id);
    const auto result = control ? invoke([&] {return control->configure(instance_id);}) :
      CallbackResult::failure("Unknown hosted component instance: " + instance_id);
    write_result(context.getResults().initResult(), result);
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> cleanupComponent(CleanupComponentContext context) override
  {
    const auto instance_id = std::string(context.getParams().getInstanceId().cStr());
    auto control = slot_for_instance(instance_id);
    const auto result = control ? invoke([&] {return control->cleanup(instance_id);}) :
      CallbackResult::failure("Unknown hosted component instance: " + instance_id);
    write_result(context.getResults().initResult(), result);
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> listComponents(ListComponentsContext context) override
  {
    struct ListedComponent
    {
      std::string connection_name;
      ComponentStatus status;
      ServerAdapterInfo adapter_info;
    };
    std::vector<ListedComponent> components;
    for (const auto & [connection_name, control] : slots_) {
      const auto adapter_info = control.adapter->get_info_adapter_server__();
      for (const auto & status : control.list()) {
        components.push_back({connection_name, status, adapter_info});
      }
    }
    auto output = context.getResults().initComponents(components.size());
    size_t index = 0;
    for (const auto & component : components) {
      auto info = output[index++];
      info.setName(component.status.name);
      info.setInstanceId(component.status.instance_id);
      info.setPluginName(component.adapter_info.plugin_name);
      info.setPluginType(component.adapter_info.plugin_type);
      info.setState(state_name(component.status.state));
      info.setConnectionName(component.connection_name);
    }
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> activateComponent(ActivateComponentContext context) override
  {
    const auto instance_id = std::string(context.getParams().getInstanceId().cStr());
    auto control = slot_for_instance(instance_id);
    CallbackResult result = control ? CallbackResult{} :
      CallbackResult::failure("Unknown hosted component instance: " + instance_id);
    HandoverState state;
    if (result.success) {
      auto wire = context.getParams().getState();
      result = control->adapter->decode_lifecycle_state__(wire.getSchemaId(), wire.getPayload(), state);
    }
    if (result.success) {result = invoke([&] {return control->activate(instance_id, std::move(state));});}
    write_result(context.getResults().initResult(), result);
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> deactivateComponent(DeactivateComponentContext context) override
  {
    auto output = context.getResults().initResult();
    const auto instance_id = std::string(context.getParams().getInstanceId().cStr());
    auto control = slot_for_instance(instance_id);
    CallbackResult result = control ? CallbackResult{} :
      CallbackResult::failure("Unknown hosted component instance: " + instance_id);
    DeactivationResult<HandoverState> stopped;
    if (result.success) {try {stopped = control->deactivate(instance_id); result = stopped.result;} catch (const std::exception & e) {result = CallbackResult::failure(e.what());}}
    auto lifecycle = output.initResult(); write_result(lifecycle, result);
    if (result.success && stopped.state) {
      auto wire = output.initState();
      uint64_t schema_id = 0;
      result = control->adapter->encode_lifecycle_state__(
        *stopped.state, schema_id, wire.initPayload());
      wire.setSchemaId(schema_id);
      write_result(lifecycle, result);
    }
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> loadComponent(LoadComponentContext context) override
  {
    const auto component = context.getParams().getComponent();
    const auto connection_name = std::string(component.getConnectionName().cStr());
    auto control = slot(connection_name);
    const auto result = control ? invoke([&] {
      return control->load(component.getComponentPath().cStr(), component.getPluginName().cStr());
    }) : CallbackResult::failure("Unknown hosted slot connection: " + connection_name);
    write_result(context.getResults().initResult(), result);
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> unloadComponent(UnloadComponentContext context) override
  {
    const auto instance_id = std::string(context.getParams().getInstanceId().cStr());
    auto control = slot_for_instance(instance_id);
    const auto result = control ? invoke([&] {return control->unload(instance_id);}) :
      CallbackResult::failure("Unknown hosted component instance: " + instance_id);
    write_result(context.getResults().initResult(), result);
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> reloadComponent(ReloadComponentContext context) override
  {
    const auto instance_id = std::string(context.getParams().getInstanceId().cStr());
    auto control = slot_for_instance(instance_id);
    const auto result = control ? invoke([&] {
      return control->reload(instance_id, context.getParams().getTransferState());
    }) : CallbackResult::failure("Unknown hosted component instance: " + instance_id);
    write_result(context.getResults().initResult(), result);
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> switchComponents(SwitchComponentsContext context) override
  {
    const auto params = context.getParams();
    const auto connection_name = std::string(params.getConnectionName().cStr());
    auto control = slot(connection_name);
    const auto result = control ? invoke([&] {
      return control->switch_components(
        params.getSourceInstanceId().cStr(), params.getTargetInstanceId().cStr(),
        params.getTransferState());
    }) : CallbackResult::failure("Unknown hosted slot connection: " + connection_name);
    write_result(context.getResults().initResult(), result);
    return ::kj::READY_NOW;
  }


  ::kj::Promise<void> ping(PingContext /* context*/) override
  {
    RPP_LOG_DEBUG(*logger_, "RPC control service received ping.");
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> shutdown(ShutdownContext /* context */) override
  {
    RPP_LOG_DEBUG(*logger_, "RPC control service received shutdown request.");
    if (on_shutdown_callback_) {
      on_shutdown_callback_();
    }
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> listAdapters(ListAdaptersContext context) override
  {
    RPP_LOG_DEBUG(*logger_, "RPC control service received listAdapters.");
    auto adapter_infos = context.getResults().initAdapters(adapters_->size());
    int index = 0;
    for (auto & [name, adapter] : *adapters_) {
      auto server_info = adapter->get_info_adapter_server__();
      auto adapter_info = adapter_infos[index++];
      (void)name;       // Suppress unused variable warning
      adapter_info.setName(server_info.name);
      adapter_info.setPluginName(server_info.plugin_name);
      adapter_info.setPluginType(server_info.plugin_type);
      adapter_info.setCreatedAt(server_info.created_at.count());
    }
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> getComponentCapability(GetComponentCapabilityContext context) override
  {
    const std::string connection_name = context.getParams().getName().cStr();
    RPP_LOG_DEBUG(
      *logger_,
      "RPC control service received getComponentCapability connection=%s.",
      connection_name.c_str());
    auto control = slot(connection_name);
    if (control && adapters_->find(connection_name) != adapters_->end()) {
      auto rebound = invoke(control->refresh_adapter);
      if (!rebound.success) {
        KJ_FAIL_REQUIRE(
          "No selected hosted component for connection '" + connection_name + "': " +
          rebound.message);
      }
      auto adapter = (*adapters_)[connection_name];
      auto capability = adapter->create_capability_adapter_server__();
      context.getResults().setPluginRef(capability);
    } else {
      KJ_FAIL_REQUIRE("Adapter with connection name '" + connection_name + "' not found.");
    }
    return ::kj::READY_NOW;
  }

};

class RpcControlClient final
{
public:
  struct RemoteComponentStatus
  {
    std::string connection_name;
    ComponentStatus status;
  };

private:
  const kj::AsyncIoContext & io_context_;
  runtime::PluginRuntime::Client backend_;
  std::shared_ptr<RppLogger> logger_;
  std::map<std::string, std::string> component_ids_by_connection_;

  const std::string & component_id(const std::string & connection_name)
  {
    auto cached = component_ids_by_connection_.find(connection_name);
    if (cached != component_ids_by_connection_.end()) {return cached->second;}
    auto response = backend_.listComponentsRequest().send().wait(io_context_.waitScope);
    std::string id;
    for (const auto component : response.getComponents()) {
      if (std::string(component.getConnectionName().cStr()) != connection_name ||
        component.getInstanceId().size() == 0)
      {
        continue;
      }
      if (!id.empty()) {
        throw LifecycleError(
          "Remote lifecycle-by-connection requires exactly one loaded component in slot: " +
          connection_name);
      }
      id = component.getInstanceId().cStr();
    }
    if (id.empty()) {
      throw LifecycleError(
        "Remote lifecycle-by-connection requires exactly one loaded component in slot: " +
        connection_name);
    }
    return component_ids_by_connection_.emplace(connection_name, std::move(id)).first->second;
  }

public:
  RpcControlClient(
    const RppRuntimeClientContext & context,
    std::shared_ptr<RppLogger> logger = nullptr)
  : io_context_(context.get_io_context()),
    backend_(context.get_client().castAs<rpp::runtime::PluginRuntime>()),
    logger_(logger ? std::move(logger) :
      std::make_shared<RppLogger>("rpp_rpc_control_client"))
  {
    auto version = backend_.getProtocolVersionRequest().send().wait(io_context_.waitScope);
    if (version.getVersion() != runtime::PROTOCOL_VERSION) {
      throw LifecycleError("Incompatible RPC control protocol version");
    }
    RPP_LOG_DEBUG(*logger_, "RPC control client connected.");
  }

  void ping()
  {
    RPP_LOG_DEBUG(*logger_, "RPC control client calling ping.");
    backend_.pingRequest().send().wait(io_context_.waitScope);
  }

  CallbackResult configure_component(const std::string & name)
  {
    auto request = backend_.configureComponentRequest();
    request.setInstanceId(component_id(name));
    auto response = request.send().wait(io_context_.waitScope);
    auto result = response.getResult();
    return {result.getSuccess(), result.getMessage().cStr()};
  }

  CallbackResult cleanup_component(const std::string & name)
  {
    auto request = backend_.cleanupComponentRequest();
    request.setInstanceId(component_id(name));
    auto response = request.send().wait(io_context_.waitScope);
    auto result = response.getResult();
    return {result.getSuccess(), result.getMessage().cStr()};
  }

  CallbackResult activate_component(
    const std::string & name, const ClientAdapter & adapter, HandoverState state)
  {
    auto request = backend_.activateComponentRequest();
    request.setInstanceId(component_id(name));
    auto wire = request.initState();
    uint64_t schema_id = 0;
    auto encoded = adapter.encode_lifecycle_state__(state, schema_id, wire.initPayload());
    if (!encoded.success) {return encoded;}
    wire.setSchemaId(schema_id);
    auto response = request.send().wait(io_context_.waitScope);
    auto result = response.getResult();
    return {result.getSuccess(), result.getMessage().cStr()};
  }

  DeactivationResult<HandoverState> deactivate_component(
    const std::string & name, const ClientAdapter & adapter)
  {
    auto request = backend_.deactivateComponentRequest();
    request.setInstanceId(component_id(name));
    auto response = request.send().wait(io_context_.waitScope);
    auto remote = response.getResult();
    auto lifecycle = remote.getResult();
    CallbackResult result{lifecycle.getSuccess(), lifecycle.getMessage().cStr()};
    if (!result.success) {return {std::move(result), {}};}
    if (!remote.hasState()) {return {std::move(result), {}};}
    HandoverState state;
    auto wire = remote.getState();
    auto decoded = adapter.decode_lifecycle_state__(wire.getSchemaId(), wire.getPayload(), state);
    if (!decoded.success) {return {std::move(decoded), {}};}
    return {std::move(result), std::move(state)};
  }

  CallbackResult load_component(
    const std::string & component_path, const std::string & plugin_name,
    const std::string & connection_name)
  {
    auto request = backend_.loadComponentRequest();
    auto component = request.initComponent();
    component.setComponentPath(component_path);
    component.setPluginName(plugin_name);
    component.setConnectionName(connection_name);
    auto response = request.send().wait(io_context_.waitScope);
    auto result = response.getResult();
    if (result.getSuccess()) {component_ids_by_connection_.erase(connection_name);}
    return {result.getSuccess(), result.getMessage().cStr()};
  }

  CallbackResult unload_component(const std::string & instance_id)
  {
    auto request = backend_.unloadComponentRequest();
    request.setInstanceId(instance_id);
    auto response = request.send().wait(io_context_.waitScope);
    auto result = response.getResult();
    if (result.getSuccess()) {component_ids_by_connection_.clear();}
    return {result.getSuccess(), result.getMessage().cStr()};
  }

  CallbackResult reload_component(const std::string & instance_id, bool transfer_state = false)
  {
    auto request = backend_.reloadComponentRequest();
    request.setInstanceId(instance_id);
    request.setTransferState(transfer_state);
    auto response = request.send().wait(io_context_.waitScope);
    auto result = response.getResult();
    if (result.getSuccess()) {component_ids_by_connection_.clear();}
    return {result.getSuccess(), result.getMessage().cStr()};
  }

  CallbackResult switch_components(
    const std::string & connection_name, const std::string & source_instance_id,
    const std::string & target_instance_id, bool transfer_state = true)
  {
    auto request = backend_.switchComponentsRequest();
    request.setConnectionName(connection_name);
    request.setSourceInstanceId(source_instance_id);
    request.setTargetInstanceId(target_instance_id);
    request.setTransferState(transfer_state);
    auto response = request.send().wait(io_context_.waitScope);
    auto result = response.getResult();
    if (result.getSuccess()) {component_ids_by_connection_.erase(connection_name);}
    return {result.getSuccess(), result.getMessage().cStr()};
  }

  std::vector<RemoteComponentStatus> list_components()
  {
    auto response = backend_.listComponentsRequest().send().wait(io_context_.waitScope);
    std::vector<RemoteComponentStatus> components;
    for (const auto info : response.getComponents()) {
      ComponentStatus component;
      component.name = info.getName().cStr();
      component.instance_id = info.getInstanceId().cStr();
      component.component = info.getPluginName().cStr();
      components.push_back({info.getConnectionName().cStr(), std::move(component)});
    }
    return components;
  }

  std::vector<ComponentStatus> list_components(const std::string & connection_name)
  {
    std::vector<ComponentStatus> components;
    for (auto remote : list_components()) {
      if (remote.connection_name == connection_name) {
        components.push_back(std::move(remote.status));
      }
    }
    return components;
  }

  void shutdown()
  {
    RPP_LOG_DEBUG(*logger_, "RPC control client calling shutdown.");
    backend_.shutdownRequest().send().wait(io_context_.waitScope);
  }

  std::vector<rpp::ServerAdapterInfo> listAdapters()
  {
    RPP_LOG_DEBUG(*logger_, "RPC control client calling listAdapters.");
    auto request = backend_.listAdaptersRequest();
    auto response = request.send().wait(io_context_.waitScope);
    auto adapters = response.getAdapters();
    std::vector<rpp::ServerAdapterInfo> adapter_infos;
    for (auto adapter : adapters) {
      rpp::ServerAdapterInfo adapter_info;
      adapter_info.plugin_name = adapter.getPluginName();
      adapter_info.name = adapter.getName();
      adapter_info.plugin_type = adapter.getPluginType();
      adapter_info.created_at = std::chrono::milliseconds(adapter.getCreatedAt());
      adapter_infos.push_back(adapter_info);
    }
    return adapter_infos;
  }

};

}  // namespace rpp
