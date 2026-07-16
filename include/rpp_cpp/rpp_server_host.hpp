#include <atomic>
#include <kj/async-io.h>
#include <memory>
#include <string>
#include <vector>
#include "plugin_runtime.hpp"
#include "capnp_server.hpp"


namespace rpp {

class RppServerHost final {

    private:
        std::string host_;
        uint16_t runtime_port_;
        kj::AsyncIoContext io_;
        std::vector<std::shared_ptr<ServerAdapter>> adapters_;
        kj::Own<PluginRuntimeServer> runtime_server_impl_;
        std::unique_ptr<runtime::CapnpServer> runtime_server_;
        std::atomic<bool> shutdown_requested_;

    public:
        RppServerHost(const std::string& host, uint16_t runtime_port)
            : host_(host),
              runtime_port_(runtime_port),
              io_(kj::setupAsyncIo()),
              runtime_server_impl_(kj::heap<PluginRuntimeServer>()),
              shutdown_requested_(false)
        {
        }

        void add_server(std::shared_ptr<ServerAdapter> adapter) {
            adapters_.push_back(adapter);
        }

        void run()
        {
            shutdown_requested_.store(false);
            runtime_server_impl_->set_adapters(adapters_);
            runtime_server_impl_->set_on_shutdown_callback([this]() {
                this->shutdown();
            });

            kj::Own<runtime::PluginRuntime::Server> owned_server(
                static_cast<runtime::PluginRuntime::Server*>(runtime_server_impl_.get()),
                kj::NullDisposer::instance
            );

            auto server_cap = capnp::Capability::Client(std::move(owned_server));
            runtime_server_ = std::make_unique<runtime::CapnpServer>(
                io_,
                server_cap,
                host_,
                runtime_port_
            );



            // Start all adapter servers
            for (auto& adapter : adapters_) {
                adapter->start_adapter_server__(io_);
            }

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

            for (auto& adapter : adapters_) {
                adapter->close_adapter_server__();
            }

        }

        void shutdown() {
            shutdown_requested_.store(true);
        }
};

} // namespace rpp