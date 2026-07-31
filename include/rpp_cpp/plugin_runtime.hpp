#pragma once
#include <vector>
#include <kj/async-io.h>
#include <functional>
#include <capnp/ez-rpc.h>
#include "plugin_runtime.capnp.h"
#include "adapter_bases.hpp"

namespace rpp {


class PluginRuntimeServer final : public runtime::PluginRuntime::Server
{
    using ServerAdapterPtr = std::shared_ptr<ServerAdapter>;
    using ServerAdapterMap = std::map<std::string, ServerAdapterPtr>;
    std::shared_ptr<ServerAdapterMap> adapters_;
    std::function<void()> on_shutdown_callback_;

public:
    PluginRuntimeServer() = default;

    void set_adapters(const ServerAdapterMap& adapters) {
        adapters_ = std::make_shared<ServerAdapterMap>(adapters);
    }

    void set_on_shutdown_callback(std::function<void()> callback) {
        on_shutdown_callback_ = std::move(callback);
    }


    ::kj::Promise<void> ping(PingContext /* context*/ ) override {
        // Implement the ping method logic here
        return ::kj::READY_NOW;
    }

    ::kj::Promise<void> shutdown(ShutdownContext /* context */ ) override {
        // Implement the shutdown method logic here
        if (on_shutdown_callback_) {
            on_shutdown_callback_();
        }
        return ::kj::READY_NOW;
    }

    ::kj::Promise<void> listAdapters(ListAdaptersContext context) override {
        // Implement the listAdapters method logic here
        auto adapter_infos = context.getResults().initAdapters(adapters_->size());
        int index = 0;
        for (auto& [name, adapter] : *adapters_) {
            auto server_info = adapter->get_info_adapter_server__();
            auto adapter_info = adapter_infos[index++];
            (void)name; // Suppress unused variable warning
            adapter_info.setName(server_info.name);
            adapter_info.setPluginName(server_info.plugin_name);
            adapter_info.setPluginType(server_info.plugin_type);
            adapter_info.setCreatedAt(server_info.created_at.count());
        }
        return ::kj::READY_NOW;
    }

    ::kj::Promise<void> getComponentCapability(GetComponentCapabilityContext context) override {
        std::string connection_name = context.getParams().getName();
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

class PluginRuntimeClient final
{
private:
    const kj::AsyncIoContext& io_context_;
    runtime::PluginRuntime::Client backend_;

public:

    PluginRuntimeClient(const ClientContext& context)
        : io_context_(context.get_io_context()),
          backend_(context.get_client().castAs<rpp::runtime::PluginRuntime>())
    {
    }

    void ping () {
        backend_.pingRequest().send().wait(io_context_.waitScope);
    }

    void shutdown () {
        backend_.shutdownRequest().send().wait(io_context_.waitScope);
    }

    std::vector<rpp::ServerAdapterInfo> listAdapters () {
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