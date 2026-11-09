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


// RPC control endpoint. ComponentRuntime owns the hosted component lifecycle;
// the generated runtime::PluginRuntime name belongs to the shared wire schema.
class RpcControlService final : public runtime::PluginRuntime::Server
{
  using ServerAdapterPtr = std::shared_ptr<ServerAdapter>;
  using ServerAdapterMap = std::map<std::string, ServerAdapterPtr>;
  std::shared_ptr<ServerAdapterMap> adapters_;
  std::function<void()> on_shutdown_callback_;
  std::shared_ptr<RppLogger> logger_;
  struct ComponentControl
  {
    std::function<CallbackResult()> configure;
    std::function<CallbackResult()> cleanup;
    std::optional<CallbackResult> configure_result;
  };
  std::map<std::string, ComponentControl> components_;

  static CallbackResult invoke(const std::function<CallbackResult()> & callback)
  {
    try {return callback();} catch (const std::exception & error) {
      return CallbackResult::failure(error.what());
    } catch (...) {return CallbackResult::failure("Unknown lifecycle callback exception");}
  }

public:
  explicit RpcControlService(std::shared_ptr<RppLogger> logger = nullptr)
  : logger_(logger ? std::move(logger) :
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

  void register_component(
    const std::string & name,
    std::function<CallbackResult()> configure, std::function<CallbackResult()> cleanup)
  {
    if (!components_.emplace(
        name, ComponentControl{
        std::move(configure), std::move(cleanup), std::nullopt}).second)
    {
      throw std::invalid_argument("Duplicate hosted component: " + name);
    }
  }

  CallbackResult configure_component(const std::string & name)
  {
    auto found = components_.find(name);
    if (found == components_.end()) {
      return CallbackResult::failure("Unknown hosted component: " + name);
    }
    auto & component = found->second;
    if (component.configure_result) {return *component.configure_result;}
    component.configure_result = CallbackResult::failure("Configuration is in progress");
    component.configure_result = invoke(component.configure);
    return *component.configure_result;
  }

  CallbackResult cleanup_component(const std::string & name)
  {
    auto found = components_.find(name);
    if (found == components_.end()) {
      return CallbackResult::failure("Unknown hosted component: " + name);
    }
    auto & component = found->second;
    if (!component.configure_result) {return {};}
    auto result = invoke(component.cleanup);
    if (result.success) {component.configure_result.reset();} else {
      component.configure_result = CallbackResult::failure(
        "Cleanup must succeed before configuration");
    }
    return result;
  }

  ::kj::Promise<void> getProtocolVersion(GetProtocolVersionContext context) override
  {
    context.getResults().setVersion(runtime::PROTOCOL_VERSION);
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> configureComponent(ConfigureComponentContext context) override
  {
    auto result = configure_component(context.getParams().getName().cStr());
    auto output = context.getResults().initResult();
    output.setSuccess(result.success);
    output.setMessage(result.message);
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> cleanupComponent(CleanupComponentContext context) override
  {
    auto result = cleanup_component(context.getParams().getName().cStr());
    auto output = context.getResults().initResult();
    output.setSuccess(result.success);
    output.setMessage(result.message);
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
    std::string connection_name = context.getParams().getName();
    RPP_LOG_DEBUG(
      *logger_,
      "RPC control service received getComponentCapability connection=%s.",
      connection_name.c_str());
    if (adapters_->find(connection_name) != adapters_->end()) {
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
private:
  const kj::AsyncIoContext & io_context_;
  runtime::PluginRuntime::Client backend_;
  std::shared_ptr<RppLogger> logger_;

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
    request.setName(name);
    auto response = request.send().wait(io_context_.waitScope);
    auto result = response.getResult();
    return {result.getSuccess(), result.getMessage().cStr()};
  }

  CallbackResult cleanup_component(const std::string & name)
  {
    auto request = backend_.cleanupComponentRequest();
    request.setName(name);
    auto response = request.send().wait(io_context_.waitScope);
    auto result = response.getResult();
    return {result.getSuccess(), result.getMessage().cStr()};
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
