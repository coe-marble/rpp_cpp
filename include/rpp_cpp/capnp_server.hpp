#pragma once
#include <memory>
#include <string>
#include <vector>
#include <kj/async-io.h>
#include <capnp/rpc-twoparty.h>
#include <capnp/rpc.h>


namespace rpp::runtime{

class CapnpServer
{
public:
    CapnpServer(
        kj::AsyncIoContext& io,
        capnp::Capability::Client capability,
        const std::string& host,
        uint16_t port)
        : io_(io),
          capability_(kj::mv(capability))
    {
        auto address = io_.provider->getNetwork()
            .parseAddress(host, port)
            .wait(io_.waitScope);

        listener_ = address->listen();

        accept_loop();
    }

private:
    struct Connection {
        kj::Own<kj::AsyncIoStream> stream;
        kj::Own<capnp::TwoPartyVatNetwork> network;
        kj::Own<capnp::RpcSystem<capnp::rpc::twoparty::VatId>> rpc;
    };
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
                    *connection->network,
                    capability_);


                connections_.add(kj::mv(connection));

                accept_loop();
                return kj::READY_NOW;
            });
    }


private:
    kj::AsyncIoContext& io_;
    capnp::Capability::Client capability_;
    kj::Own<kj::ConnectionReceiver> listener_;

    kj::Vector<kj::Own<Connection>> connections_;
    kj::Maybe<kj::Promise<void>> accept_promise_;
};

}  // namespace rpp::runtime
