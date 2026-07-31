#pragma once
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <kj/async-io.h>
#include <capnp/rpc-twoparty.h>
#include <capnp/rpc.h>


namespace rpp::runtime{

class CapnpServer
{
public:

    using ServerAdapterMap =
        std::map<std::string, std::shared_ptr<ServerAdapter>>;

    CapnpServer(
        kj::AsyncIoContext& io,
        const std::string& host,
        uint16_t port,
        capnp::Capability::Client runtime_capability,
        ServerAdapterMap& instances)
        : io_(io),
          capability_(runtime_capability),
          instances_(std::make_shared<ServerAdapterMap>(instances))
    {
        start(host, port);
    }

    CapnpServer(
        kj::AsyncIoContext& io,
        const std::string& host,
        uint16_t port,
        capnp::Capability::Client capability)
        : io_(io),
          capability_(capability),
          instances_(nullptr)
    {
        start(host, port);
    }

private:

    struct Connection {
        kj::Own<kj::AsyncIoStream> stream;
        kj::Own<capnp::TwoPartyVatNetwork> network;
        kj::Own<capnp::RpcSystem<capnp::rpc::twoparty::VatId>> rpc;
    };


    void start(const std::string& host, uint16_t port)
    {
        auto address = io_.provider->getNetwork()
            .parseAddress(host, port)
            .wait(io_.waitScope);

        listener_ = address->listen();
        accept_loop();
    }

    void accept_loop()
    {
        accept_promise_ = listener_->accept()
            .then([this](kj::Own<kj::AsyncIoStream> stream)
                -> kj::Promise<void>
            {
                auto connection = kj::heap<Connection>();
                connection->stream = kj::mv(stream);
                connection->network = kj::heap<capnp::TwoPartyVatNetwork>(
                    *connection->stream,
                    capnp::rpc::twoparty::Side::SERVER);


                connection->rpc = kj::heap<capnp::RpcSystem<capnp::rpc::twoparty::VatId>>(
                    *connection->network, capability_
                );
                connections_.add(kj::mv(connection));

                accept_loop();
                return kj::READY_NOW;
            });
    }


private:
    kj::AsyncIoContext& io_;
    capnp::Capability::Client capability_;
    std::shared_ptr<ServerAdapterMap> instances_;
    kj::Own<kj::ConnectionReceiver> listener_;

    kj::Vector<kj::Own<Connection>> connections_;
    kj::Maybe<kj::Promise<void>> accept_promise_;
};

}  // namespace rpp::runtime
