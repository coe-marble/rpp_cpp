#pragma once

#include <capnp/message.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "rpp_cpp/rpc_control_service.hpp"

namespace rpp::testing
{
constexpr uint64_t kRemoteStateSchemaId = 0xa52ea179cbcf660aULL;

// This mirrors a generated lifecycle state: it is a move-only wrapper around
// a Cap'n Proto reader, rather than a native data-model copy.
class RemoteState
{
public:
  class Const
  {
public:
    explicit Const(runtime::AdapterInfo::Reader reader)
    : reader_(reader) {}

    Const(Const &&) noexcept = default;
    Const & operator=(Const &&) noexcept = default;
    Const(const Const &) = delete;
    Const & operator=(const Const &) = delete;

    static Const named(std::string name)
    {
      auto message = std::make_unique<capnp::MallocMessageBuilder>();
      auto state = message->initRoot<runtime::AdapterInfo>();
      state.setName(name);
      return Const(std::move(message));
    }

    static Const from_wire(runtime::AdapterInfo::Reader reader)
    {
      auto message = std::make_unique<capnp::MallocMessageBuilder>();
      message->setRoot(reader);
      return Const(std::move(message));
    }

    std::string name() const {return reader_.getName().cStr();}
    operator runtime::AdapterInfo::Reader() const {return reader_;}

private:
    explicit Const(std::unique_ptr<capnp::MallocMessageBuilder> message)
    : message_(std::move(message)),
      reader_(message_->getRoot<runtime::AdapterInfo>()) {}

    std::unique_ptr<capnp::MallocMessageBuilder> message_;
    runtime::AdapterInfo::Reader reader_;
  };
};

class RemoteStateCodec
{
protected:
  static CallbackResult decode(
    uint64_t schema_id, capnp::AnyPointer::Reader payload, HandoverState & state)
  {
    if (schema_id == 0 && payload.isNull()) {
      state = HandoverState{};
      return {};
    }
    if (schema_id != kRemoteStateSchemaId) {
      return CallbackResult::failure("Unexpected lifecycle state schema ID");
    }
    state = HandoverState(RemoteState::Const::from_wire(payload.getAs<runtime::AdapterInfo>()));
    return {};
  }

  static CallbackResult encode(
    const HandoverState & state, uint64_t & schema_id, capnp::AnyPointer::Builder payload)
  {
    if (!state.has_value()) {
      schema_id = 0;
      return {};
    }
    if (!state.holds<RemoteState::Const>()) {
      return CallbackResult::failure("Unexpected lifecycle state type");
    }
    schema_id = kRemoteStateSchemaId;
    payload.setAs<runtime::AdapterInfo>(state.get<RemoteState::Const>());
    return {};
  }
};

class RemoteServerAdapter final : public ServerAdapter, private RemoteStateCodec
{
public:
  RemoteServerAdapter()
  {
    info_.name = "remote";
    info_.connection_name = "remote";
  }

  bool configure_adapter_server__(std::shared_ptr<ServerAdapterParams>) override {return true;}
  void start_adapter_server__(kj::AsyncIoContext &, std::string, uint16_t) override {}
  capnp::Capability::Client create_capability_adapter_server__() override
  {return capnp::Capability::Client(nullptr);}
  void close_adapter_server__() override {}
  const ServerAdapterInfo & get_info_adapter_server__() const override {return info_;}
  CallbackResult decode_lifecycle_state__(
    uint64_t schema_id, capnp::AnyPointer::Reader payload, HandoverState & state) const override
  {return decode(schema_id, payload, state);}
  CallbackResult encode_lifecycle_state__(
    const HandoverState & state, uint64_t & schema_id,
    capnp::AnyPointer::Builder payload) const override
  {return encode(state, schema_id, payload);}

private:
  ServerAdapterInfo info_;
};

class RemoteClientAdapter final : public ClientAdapter, private RemoteStateCodec
{
public:
  bool configure_adapter_client__(std::shared_ptr<ClientAdapterParams>) override {return true;}
  bool connect_adapter_client__(
    const RppRuntimeClientContext &,
    std::optional<std::reference_wrapper<ComponentCallExecutor>>) override {return true;}
  const ClientAdapterInfo & get_info_adapter_client__() const override {return info_;}
  CallbackResult decode_lifecycle_state__(
    uint64_t schema_id, capnp::AnyPointer::Reader payload, HandoverState & state) const override
  {return decode(schema_id, payload, state);}
  CallbackResult encode_lifecycle_state__(
    const HandoverState & state, uint64_t & schema_id,
    capnp::AnyPointer::Builder payload) const override
  {return encode(state, schema_id, payload);}

private:
  ClientAdapterInfo info_;
};

class RemoteStatefulPlugin final : public LifecyclePlugin<RemoteState::Const>
{
public:
  std::vector<std::string> events;
  std::vector<std::string> activated_with;

  CallbackResult on_configure(const ComponentContext &) override
  {
    events.push_back("configure");
    return {};
  }

  CallbackResult on_activate(const std::optional<RemoteState::Const> & state) override
  {
    events.push_back("activate");
    activated_with.push_back(state ? state->name() : "");
    return {};
  }

  DeactivationResult<RemoteState::Const> on_deactivate() override
  {
    events.push_back("deactivate");
    return {{}, RemoteState::Const::named("returned-state")};
  }

  CallbackResult on_cleanup() override
  {
    events.push_back("cleanup");
    return {};
  }
};
}  // namespace rpp::testing
