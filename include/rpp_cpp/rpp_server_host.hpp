#pragma once
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <atomic>
#include <kj/async-io.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "logger.hpp"
#include "runtime.hpp"
#include "rpc_control_service.hpp"
#include "capnp_server.hpp"


namespace rpp
{

template<class Threading>
class BasicRppServerHost final
{
  using SlotManager = BasicSlotManager<Threading>;
  using Timeout = typename SlotManager::Timeout;
  using SlotLoader = std::function<CallbackResult(const std::string &, const std::string &)>;

private:
  friend class BasicComponentRuntimeBuilder<Threading>;
  std::string host_;
  uint16_t port_;
  kj::AsyncIoContext io_;
  std::map<std::string, std::shared_ptr<ServerAdapter>> adapters_;
  std::map<std::string, std::shared_ptr<SlotManager>> slots_;
  std::vector<std::string> slot_order_;
  kj::Own<RpcControlService> rpc_server_impl_;
  std::unique_ptr<runtime::CapnpServer> runtime_server_;
  std::atomic<bool> shutdown_requested_;
  std::shared_ptr<RppLogger> logger_;

public:
  BasicRppServerHost(
    const std::string & host, uint16_t port,
    std::shared_ptr<RppLogger> logger = nullptr)
  : host_(host),
    port_(port),
    io_(kj::setupAsyncIo()),
    rpc_server_impl_(kj::heap<RpcControlService>(logger)),
    shutdown_requested_(false),
    logger_(logger ? std::move(logger) :
      std::make_shared<RppLogger>("rpp_server_host"))
  {
  }

  ~BasicRppServerHost()
  {
    (void)close();
  }

private:
  void add_slot(
    std::shared_ptr<ServerAdapter> adapter,
    std::shared_ptr<SlotManager> manager, SlotLoader load_component,
    bool allows_dynamic_operations)
  {
    if (!adapter || !manager || !load_component) {
      throw std::invalid_argument("Server adapter, slot manager, and loader are required");
    }
    std::string name = adapter->get_info_adapter_server__().connection_name;
    if (name.empty() || adapters_.count(name) || slots_.count(name)) {
      throw std::invalid_argument("Duplicate or empty hosted slot connection name: " + name);
    }
    auto entry_name = [manager](const std::string & instance_id) -> std::optional<std::string> {
        for (const auto & status : manager->list()) {
          if (status.instance_id == instance_id) {return status.name;}
        }
        return std::nullopt;
      };
    auto missing = [](const std::string & instance_id) {
        return CallbackResult::failure("Unknown hosted component instance: " + instance_id);
      };
    auto dynamic_operations_disabled = [] {
        return CallbackResult::failure(
          "This hosted slot is lifecycle-only; load, unload, reload, and switch "
          "are available only for dynamic top-level components");
      };
    auto refresh_adapter = [adapter, manager]() -> CallbackResult {
        auto runtime = manager->selected_runtime();
        if (!runtime) {
          return CallbackResult::failure("Hosted slot has no selected loaded component");
        }
        const auto & info = adapter->get_info_adapter_server__();
        auto params = std::make_shared<ServerAdapterParams>();
        params->name = info.name;
        params->connection_name = info.connection_name;
        params->plugin_name = info.plugin_name;
        params->backend = runtime->template get_instance<Plugin>();
        return adapter->configure_adapter_server__(std::move(params)) ? CallbackResult{} :
               CallbackResult::failure("Failed to bind adapter to selected hosted component");
      };
    rpc_server_impl_->register_slot(name, RpcControlService::SlotControl{
        adapter,
        [manager] {return manager->list();},
        [load_component = std::move(load_component), refresh_adapter, allows_dynamic_operations,
        dynamic_operations_disabled](
          const std::string & path, const std::string & plugin) {
          if (!allows_dynamic_operations) {return dynamic_operations_disabled();}
          auto result = load_component(path, plugin);
          return result.success ? refresh_adapter() : result;
        },
        [manager, entry_name, missing](const std::string & instance_id) {
          auto component = entry_name(instance_id);
          return component ? manager->configure(*component, instance_id) : missing(instance_id);
        },
        [manager, entry_name, missing](const std::string & instance_id) {
          auto component = entry_name(instance_id);
          return component ? manager->cleanup(*component, instance_id) : missing(instance_id);
        },
        [manager, entry_name, missing](const std::string & instance_id, HandoverState state) {
          auto component = entry_name(instance_id);
          return component ? manager->activate(*component, std::move(state),
            instance_id) : missing(instance_id);
        },
        [manager, entry_name,
        missing](const std::string & instance_id) -> DeactivationResult<HandoverState> {
          auto component = entry_name(instance_id);
          return component ? manager->deactivate(*component, Timeout::max(), instance_id) :
                 DeactivationResult<HandoverState>{missing(instance_id), {}};
        },
        [manager, entry_name, missing, refresh_adapter, allows_dynamic_operations,
        dynamic_operations_disabled](const std::string & instance_id) {
          if (!allows_dynamic_operations) {return dynamic_operations_disabled();}
          auto component = entry_name(instance_id);
          if (!component) {return missing(instance_id);}
          auto result = manager->unload(*component, Timeout::max(), instance_id);
          return result.success && manager->selected_runtime() ? refresh_adapter() : result;
        },
        [manager, entry_name, missing, refresh_adapter, allows_dynamic_operations,
        dynamic_operations_disabled](const std::string & instance_id, bool transfer_state) {
          if (!allows_dynamic_operations) {return dynamic_operations_disabled();}
          auto component = entry_name(instance_id);
          if (!component) {return missing(instance_id);}
          auto result = manager->reload(*component, transfer_state);
          return result.success ? refresh_adapter() : result;
        },
        [manager, entry_name, missing, refresh_adapter, allows_dynamic_operations,
        dynamic_operations_disabled](
          const std::string & source_id, const std::string & target_id, bool transfer_state) {
          if (!allows_dynamic_operations) {return dynamic_operations_disabled();}
          auto source = entry_name(source_id);
          auto target = entry_name(target_id);
          if (!source) {return missing(source_id);}
          if (!target) {return missing(target_id);}
          auto result = manager->switch_components(*source, *target, transfer_state);
          return result.success ? refresh_adapter() : result;
        },
        [manager] {return manager->close();},
        std::move(refresh_adapter),
    });
    adapters_[name] = adapter;
    slots_[name] = std::move(manager);
    slot_order_.push_back(name);
    RPP_LOG_DEBUG(
      *logger_, "Registered hosted slot connection=%s.",
      name.c_str());
  }

public:
  /// Shut down every hosted slot while retaining enough state to report failures.
  CallbackResult close()
  {
    CallbackResult first_error;
    for (auto slot = slot_order_.rbegin(); slot != slot_order_.rend(); ++slot) {
      auto result = rpc_server_impl_->close_slot(*slot);
      if (!result.success && first_error.success) {first_error = std::move(result);}
    }
    return first_error;
  }

  kj::AsyncIoContext & get_io_context() {return io_;}
  const kj::AsyncIoContext & get_io_context() const {return io_;}

  void run()
  {
    RPP_LOG_INFO(
      *logger_, "Starting runtime host host=%s port=%d adapters=%zu.",
      host_.c_str(), port_, adapters_.size());
    shutdown_requested_.store(false);
    rpc_server_impl_->set_adapters(adapters_);
    rpc_server_impl_->set_on_shutdown_callback(
      [this]() {
        this->shutdown();
      });

    kj::Own<runtime::PluginRuntime::Server> owned_server(
      static_cast<runtime::PluginRuntime::Server *>(rpc_server_impl_.get()),
      kj::NullDisposer::instance
    );

    auto runtime_server_cap = capnp::Capability::Client(std::move(owned_server));
    runtime_server_ = std::make_unique<runtime::CapnpServer>(
      io_, host_, port_, runtime_server_cap, adapters_
    );
    RPP_LOG_INFO(
      *logger_, "Runtime host listening host=%s port=%d.",
      host_.c_str(), port_);

    kj::Timer & timer = io_.provider->getTimer();
    std::function<kj::Promise<void>()> checkShutdown;
    checkShutdown = [&]() -> kj::Promise<void> {
        if (shutdown_requested_.load()) {
          return kj::READY_NOW;
        }
        return timer.afterDelay(10 * kj::MILLISECONDS)
               .then(checkShutdown);
      };
    checkShutdown().wait(io_.waitScope);
    RPP_LOG_INFO(*logger_, "Stopping runtime host.");

    for (auto & [name, adapter] : adapters_) {
      RPP_LOG_DEBUG(
        *logger_, "Stopping runtime adapter connection=%s.",
        name.c_str());
      adapter->close_adapter_server__();
    }

    RPP_LOG_INFO(*logger_, "Runtime host stopped.");

  }

  void shutdown()
  {
    shutdown_requested_.store(true);
  }
};

} // namespace rpp
