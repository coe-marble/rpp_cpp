#pragma once
#include <string>
#include <memory>
#include <chrono>
#include <kj/async-io.h>
#include "plugin_def.hpp"
#include "adapter_info.hpp"
#include <capnp/rpc-twoparty.h>


namespace rpp {

class ClientContext {

    using VatId = capnp::rpc::twoparty::VatId;
    public:
        ClientContext(const std::string& host, uint16_t port)
            : host_(host), port_(port), io_(kj::setupAsyncIo())
        {
            auto& network = io_.provider->getNetwork();
            auto address = network.parseAddress(host_, port_).wait(io_.waitScope);
            stream_ = address->connect().wait(io_.waitScope);

            vat_network_ = kj::heap<capnp::TwoPartyVatNetwork>(
                *stream_, capnp::rpc::twoparty::Side::CLIENT);

            client_ = kj::heap<capnp::RpcSystem<VatId>>(*vat_network_, nullptr);
        }

        capnp::Capability::Client get_client() const {
            VatId::Reader vat_id_msg;
            return client_->bootstrap(vat_id_msg);
        }

        const std::string& get_host() const { return host_; }
        uint16_t get_port() const { return port_; }
        const kj::AsyncIoContext& get_io_context() const { return io_; }
    private:
        std::string host_;
        uint16_t port_;
        kj::AsyncIoContext io_;
        kj::Own<kj::AsyncIoStream> stream_;
        kj::Own<capnp::TwoPartyVatNetwork> vat_network_;
        mutable kj::Own<capnp::RpcSystem<capnp::rpc::twoparty::VatId>> client_;
};


class ServerAdapter {
    public:
        virtual ~ServerAdapter() = default;
        virtual bool configure_adapter_server__(
            std::shared_ptr<ServerAdapterParams> info) = 0;
        virtual void start_adapter_server__(
            kj::AsyncIoContext& io, std::string host, uint16_t port) = 0;
        virtual capnp::Capability::Client create_capability_adapter_server__() = 0;
        virtual void close_adapter_server__() = 0;
        virtual const ServerAdapterInfo& get_info_adapter_server__() const = 0;

    };

class ClientAdapter {
    public:
        virtual ~ClientAdapter() = default;
        virtual bool configure_adapter_client__(std::shared_ptr<ClientAdapterParams> info) = 0;
        virtual bool connect_adapter_client__(const ClientContext& info) = 0;
        virtual const ClientAdapterInfo& get_info_adapter_client__() const = 0;
};



}  // namespace rpp