#pragma once
#include <atomic>
#include <kj/async-io.h>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "logger.hpp"
#include "plugin_runtime.hpp"
#include "capnp_server.hpp"


namespace rpp {

class RppServerHost final {

    private:
        std::string host_;
        uint16_t port_;
        kj::AsyncIoContext io_;
        std::map<std::string, std::shared_ptr<ServerAdapter>> adapters_;
        kj::Own<PluginRuntimeServer> runtime_server_impl_;
        std::unique_ptr<runtime::CapnpServer> runtime_server_;
        std::atomic<bool> shutdown_requested_;
        std::shared_ptr<RppLogger> logger_;

    public:
        RppServerHost(const std::string& host, uint16_t port,
                      std::shared_ptr<RppLogger> logger = nullptr)
            : host_(host),
              port_(port),
              io_(kj::setupAsyncIo()),
              runtime_server_impl_(kj::heap<PluginRuntimeServer>(logger)),
              shutdown_requested_(false),
              logger_(logger ? std::move(logger)
                             : std::make_shared<RppLogger>("rpp_server_host"))
        {
        }

        void add_server(std::shared_ptr<ServerAdapter> adapter)
        {
            std::string name = adapter->get_info_adapter_server__().connection_name;
            adapters_[name] = adapter;
            RPP_LOG_DEBUG(*logger_, "Registered runtime adapter connection=%s.",
                          name.c_str());
        }

        kj::AsyncIoContext& get_io_context() { return io_; }
        const kj::AsyncIoContext& get_io_context() const { return io_; }

        void run()
        {
            RPP_LOG_INFO(*logger_, "Starting runtime host host=%s port=%d adapters=%zu.",
                         host_.c_str(), port_, adapters_.size());
            shutdown_requested_.store(false);
            runtime_server_impl_->set_adapters(adapters_);
            runtime_server_impl_->set_on_shutdown_callback([this]() {
                this->shutdown();
            });

            kj::Own<runtime::PluginRuntime::Server> owned_server(
                static_cast<runtime::PluginRuntime::Server*>(runtime_server_impl_.get()),
                kj::NullDisposer::instance
            );

            auto runtime_server_cap = capnp::Capability::Client(std::move(owned_server));
            runtime_server_ = std::make_unique<runtime::CapnpServer>(
                io_, host_, port_, runtime_server_cap, adapters_
            );
            RPP_LOG_INFO(*logger_, "Runtime host listening host=%s port=%d.",
                         host_.c_str(), port_);

            kj::Timer& timer = io_.provider->getTimer();
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

            for (auto& [name, adapter] : adapters_) {
                RPP_LOG_DEBUG(*logger_, "Stopping runtime adapter connection=%s.",
                              name.c_str());
                adapter->close_adapter_server__();
            }

            RPP_LOG_INFO(*logger_, "Runtime host stopped.");

        }

        void shutdown() {
            shutdown_requested_.store(true);
        }
};

} // namespace rpp
