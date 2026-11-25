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
#include <set>
#include <stdexcept>
#include <utility>
#include "plugin_runtime.capnp.h"
#include "adapter_bases.hpp"
#include "logger.hpp"
#include "component_status.hpp"
#include "handover_state.hpp"

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
    std::function<CallbackResult(const std::string &)> deactivate;
    std::function<CallbackResult(const std::string &)> unload;
    std::function<CallbackResult(const std::string &, HandoverState)> reload;
    std::function<CallbackResult()> close;
    // Rebind the generated adapter to the selected runtime before handing out
    // its typed capability.
    std::function<CallbackResult()> refresh_adapter;
    bool allows_dynamic_operations = false;
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

  struct Instance
  {
    std::string id;
    SlotControl * control = nullptr;
  };

  struct Activation
  {
    Instance instance;
    HandoverState state;
  };

  CallbackResult resolve_instance(
    std::string id, std::set<std::string> & seen, Instance & output,
    std::optional<LifecycleState> expected)
  {
    if (id.empty()) {return CallbackResult::failure("Component instance ID is required");}
    if (!seen.insert(id).second) {
      return CallbackResult::failure("Repeated component instance: " + id);
    }
    for (auto & item : slots_) {
      auto & control = item.second;
      for (const auto & component : control.list()) {
        if (component.instance_id != id) {continue;}
        if (expected && (component.transition || component.state != *expected)) {
          return CallbackResult::failure(
            "Instance '" + id + "' must be " + state_name(*expected));
        }
        output = {std::move(id), &control};
        return {};
      }
    }
    return CallbackResult::failure("Unknown hosted component instance: " + id);
  }

  CallbackResult prepare_instances(
    capnp::List<capnp::Text>::Reader ids, std::set<std::string> & seen,
    std::vector<Instance> & instances,
    std::optional<LifecycleState> expected = {})
  {
    instances.reserve(ids.size());
    for (auto id : ids) {
      Instance instance;
      auto result = resolve_instance(id.cStr(), seen, instance, expected);
      if (!result.success) {return result;}
      instances.push_back(std::move(instance));
    }
    return {};
  }

  CallbackResult prepare_activations(
    capnp::List<capnp::Text>::Reader ids,
    capnp::List<runtime::HandoverState>::Reader handover_states, std::set<std::string> & seen,
    std::vector<Activation> & activations,
    std::optional<LifecycleState> expected = {})
  {
    if (handover_states.size() != 0 && handover_states.size() != ids.size()) {
      return CallbackResult::failure(
          "Handover states must follow activation IDs, one entry per ID");
    }
    std::vector<Instance> instances;
    auto resolved = prepare_instances(ids, seen, instances, expected);
    if (!resolved.success) {return resolved;}
    activations.reserve(instances.size());
    for (std::size_t i = 0; i < instances.size(); ++i) {
      auto & instance = instances[i];
      HandoverState state;
      if (handover_states.size() != 0) {
        const auto wire = handover_states[i];
        auto decoded = instance.control->adapter->decode_lifecycle_state__(
          wire.getSchemaId(), wire.getPayload(), state);
        if (!decoded.success) {return lifecycle_failure(instance, decoded, true);}
      }
      activations.push_back({std::move(instance), std::move(state)});
    }
    return {};
  }

  template<class Operate>
  CallbackResult operate_on_instances(capnp::List<capnp::Text>::Reader ids, const Operate & operate)
  {
    return invoke([&] {
               std::set<std::string> seen;
               std::vector<Instance> instances;
               auto prepared = prepare_instances(ids, seen, instances);
               if (!prepared.success) {return prepared;}
               for (const auto & instance : instances) {
                 auto result = operate(*instance.control, instance.id);
                 if (!result.success) {
                   return lifecycle_failure(instance, result, instances.size() > 1);
                 }
               }
               return CallbackResult{};
      });
  }

  static CallbackResult lifecycle_failure(
    const Instance & instance, const CallbackResult & result, bool report_id)
  {
    return report_id ?
           CallbackResult::failure("Instance '" + instance.id + "': " + result.message) : result;
  }

  static CallbackResult activate_instances(std::vector<Activation> & activations)
  {
    for (auto & activation : activations) {
      auto & instance = activation.instance;
      auto result = instance.control->activate(instance.id, std::move(activation.state));
      if (!result.success) {return lifecycle_failure(instance, result, activations.size() > 1);}
    }
    return {};
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
      !control.reload || !control.close ||
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
    auto result = operate_on_instances(context.getParams().getInstanceIds(),
        [](SlotControl & control, const std::string & id) {return control.configure(id);});
    write_result(context.getResults().initResult(), result);
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> cleanupComponent(CleanupComponentContext context) override
  {
    auto result = operate_on_instances(context.getParams().getInstanceIds(),
        [](SlotControl & control, const std::string & id) {return control.cleanup(id);});
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
    auto result = invoke([&] {
          std::set<std::string> seen;
          std::vector<Activation> activations;
          const auto params = context.getParams();
          auto prepared = prepare_activations(
          params.getInstanceIds(), params.getHandoverStates(), seen, activations);
          return prepared.success ? activate_instances(activations) : prepared;
      });
    write_result(context.getResults().initResult(), result);
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> deactivateComponent(DeactivateComponentContext context) override
  {
    auto result = operate_on_instances(context.getParams().getInstanceIds(),
        [](SlotControl & control, const std::string & id) {return control.deactivate(id);});
    write_result(context.getResults().initResult(), result);
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> loadComponent(LoadComponentContext context) override
  {
    const auto component = context.getParams().getComponent();
    const auto connection_name = std::string(component.getConnectionName().cStr());
    auto control = slot(connection_name);
    const auto result = control ? invoke([&] {
          return control->load(component.getComponentPath().cStr(),
          component.getPluginName().cStr());
    }) : CallbackResult::failure("Unknown hosted slot connection: " + connection_name);
    write_result(context.getResults().initResult(), result);
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> unloadComponent(UnloadComponentContext context) override
  {
    auto result = operate_on_instances(context.getParams().getInstanceIds(),
        [](SlotControl & control, const std::string & id) {return control.unload(id);});
    write_result(context.getResults().initResult(), result);
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> reloadComponent(ReloadComponentContext context) override
  {
    const auto params = context.getParams();
    auto result = invoke([&] {
          std::set<std::string> seen;
          std::vector<Activation> replacements;
          auto prepared = prepare_activations(
          params.getInstanceIds(), params.getHandoverStates(), seen, replacements);
          if (!prepared.success) {return prepared;}
          for (auto & replacement : replacements) {
            auto & instance = replacement.instance;
            auto reloaded = instance.control->reload(instance.id, std::move(replacement.state));
            if (!reloaded.success) {return lifecycle_failure(instance, reloaded, true);}
          }
          return CallbackResult{};
      });
    write_result(context.getResults().initResult(), result);
    return ::kj::READY_NOW;
  }

  ::kj::Promise<void> switchComponents(SwitchComponentsContext context) override
  {
    const auto params = context.getParams();
    auto result = invoke([&] {
          std::set<std::string> seen;
          std::vector<Instance> outgoing;
          auto prepared = prepare_instances(
          params.getDeactivate(), seen, outgoing, LifecycleState::ACTIVE);
          if (!prepared.success) {return prepared;}

          std::vector<Activation> incoming;
          prepared = prepare_activations(
          params.getActivate(), params.getHandoverStates(), seen, incoming,
          LifecycleState::INACTIVE);
          if (!prepared.success) {return prepared;}

          for (const auto & instance : outgoing) {
            auto stopped = instance.control->deactivate(instance.id);
            if (!stopped.success) {return lifecycle_failure(instance, stopped, true);}
          }
          auto started = activate_instances(incoming);
          if (!started.success) {return started;}
          std::set<SlotControl *> refreshed;
          for (const auto & activation : incoming) {
            auto control = activation.instance.control;
            if (!refreshed.insert(control).second) {continue;}
            auto result = control->refresh_adapter();
            if (!result.success) {return result;}
          }
          return CallbackResult{};
      });
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

  template<class Request>
  CallbackResult call_instances(Request && request, const std::vector<std::string> & instance_ids)
  {
    auto ids = request.initInstanceIds(instance_ids.size());
    for (std::size_t index = 0; index < instance_ids.size(); ++index) {
      ids.set(index, instance_ids[index]);
    }
    auto response = request.send().wait(io_context_.waitScope);
    auto result = response.getResult();
    return {result.getSuccess(), result.getMessage().cStr()};
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
    return configure_instances({component_id(name)});
  }

  /// Batch lifecycle helpers use immutable loaded instance IDs across hosted slots.
  CallbackResult configure_instances(const std::vector<std::string> & instance_ids)
  {
    return call_instances(backend_.configureComponentRequest(), instance_ids);
  }

  CallbackResult cleanup_component(const std::string & name)
  {
    return cleanup_instances({component_id(name)});
  }

  CallbackResult cleanup_instances(const std::vector<std::string> & instance_ids)
  {
    return call_instances(backend_.cleanupComponentRequest(), instance_ids);
  }

  CallbackResult activate_component(
    const std::string & name, const ClientAdapter & adapter, HandoverState state)
  {
    auto request = backend_.activateComponentRequest();
    auto wire = request.initHandoverStates(1)[0];
    uint64_t schema_id = 0;
    auto encoded = adapter.encode_lifecycle_state__(state, schema_id, wire.initPayload());
    if (!encoded.success) {return encoded;}
    wire.setSchemaId(schema_id);
    return call_instances(request, {component_id(name)});
  }

  CallbackResult activate_instances(
    const std::vector<std::string> & instance_ids,
    capnp::List<runtime::HandoverState>::Reader handover_states = {})
  {
    auto request = backend_.activateComponentRequest();
    request.setHandoverStates(handover_states);
    return call_instances(request, instance_ids);
  }

  CallbackResult deactivate_component(const std::string & name)
  {
    return deactivate_instances({component_id(name)});
  }

  CallbackResult deactivate_instances(const std::vector<std::string> & instance_ids)
  {
    return call_instances(backend_.deactivateComponentRequest(), instance_ids);
  }

  CallbackResult switch_components(
    const std::vector<std::string> & deactivate, const std::vector<std::string> & activate,
    capnp::List<runtime::HandoverState>::Reader handover_states = {})
  {
    auto request = backend_.switchComponentsRequest();
    auto outgoing = request.initDeactivate(deactivate.size());
    for (std::size_t i = 0; i < deactivate.size(); ++i) {outgoing.set(i, deactivate[i]);}
    auto incoming = request.initActivate(activate.size());
    for (std::size_t i = 0; i < activate.size(); ++i) {incoming.set(i, activate[i]);}
    request.setHandoverStates(handover_states);
    auto response = request.send().wait(io_context_.waitScope);
    auto result = response.getResult();
    return {result.getSuccess(), result.getMessage().cStr()};
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
    return unload_instances({instance_id});
  }

  CallbackResult unload_instances(const std::vector<std::string> & instance_ids)
  {
    // Earlier entries may succeed before a later entry fails. Rediscover IDs on retry.
    component_ids_by_connection_.clear();
    return call_instances(backend_.unloadComponentRequest(), instance_ids);
  }

  CallbackResult reload_component(
    const std::string & instance_id,
    capnp::List<runtime::HandoverState>::Reader handover_states = {})
  {
    return reload_instances({instance_id}, handover_states);
  }

  CallbackResult reload_instances(
    const std::vector<std::string> & instance_ids,
    capnp::List<runtime::HandoverState>::Reader handover_states = {})
  {
    component_ids_by_connection_.clear();
    auto request = backend_.reloadComponentRequest();
    request.setHandoverStates(handover_states);
    return call_instances(request, instance_ids);
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
