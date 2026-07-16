#pragma once
#include <vector>
#include <kj/async-io.h>
#include <functional>
#include <capnp/ez-rpc.h>
#include "plugin_runtime.capnp.h"
#include "adapter_info.hpp"

namespace rpp {


class PluginRuntimeServer final : public runtime::PluginRuntime::Server
{

    std::shared_ptr<std::vector<std::shared_ptr<ServerAdapter>>> adapters_;
    std::function<void()> on_shutdown_callback_;

public:
    PluginRuntimeServer() = default;

    void set_adapters(const std::vector<std::shared_ptr<ServerAdapter>>& adapters) {
        adapters_ = std::make_shared<std::vector<std::shared_ptr<ServerAdapter>>>(adapters);
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
        for (size_t i = 0; i < adapters_->size(); ++i) {
            auto adapter_info = adapter_infos[i];
            auto server_info = adapters_->at(i)->get_info_adapter_server__();
            adapter_info.setName(server_info.name);
            adapter_info.setPluginName(server_info.plugin_name);
            adapter_info.setPluginType(server_info.plugin_type);
            adapter_info.setCreatedAt(server_info.created_at.count());
        }
        return ::kj::READY_NOW;
    }
};

class PluginRuntimeClient final
{
private:
    std::string host_;
    uint16_t port_;
    std::unique_ptr<capnp::EzRpcClient> client_;
    runtime::PluginRuntime::Client backend_;
public:
    PluginRuntimeClient(std::string host, uint16_t port)
        : host_(std::move(host)),
          port_(port),
          client_(nullptr),
          backend_(nullptr)
    {
        client_ = std::make_unique<capnp::EzRpcClient>(host_, port_);
        backend_ = std::move(client_->getMain<runtime::PluginRuntime>());
    }

    void ping () {
        backend_.pingRequest().send().wait(client_->getWaitScope());
    }

    void shutdown () {
        backend_.shutdownRequest().send().wait(client_->getWaitScope());
    }

    std::vector<rpp::ServerAdapterInfo> listAdapters () {
        auto request = backend_.listAdaptersRequest();
        auto response = request.send().wait(client_->getWaitScope());
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