#pragma once
#include <string>
#include <memory>
#include <chrono>
#include "plugin.hpp"
#include <kj/async-io.h>


namespace rpp {

struct ServerAdapterParams {
    std::string name;
    std::string host;
    std::string plugin_name;
    uint16_t port;
    std::shared_ptr<rpp::Plugin> backend;
};


struct ClientAdapterParams {
    std::string name;
    std::string plugin_name;
    std::string host;
    uint16_t port;
};

struct ServerAdapterInfo {
    std::string name;
    std::string plugin_name;
    std::string plugin_type;
    std::chrono::milliseconds created_at;
};

struct ClientAdapterInfo {
    std::string name;
    std::string plugin_name;
    std::string plugin_type;
    std::chrono::milliseconds created_at;
};



class ServerAdapter {
    public:
        virtual ~ServerAdapter() = default;
        virtual bool configure_adapter_server__(std::shared_ptr<ServerAdapterParams> info) = 0;
        virtual void start_adapter_server__(kj::AsyncIoContext& io) = 0;
        virtual void close_adapter_server__() = 0;
        virtual const ServerAdapterInfo& get_info_adapter_server__() const = 0;

    };

class ClientAdapter {
    public:
        virtual ~ClientAdapter() = default;
        virtual bool configure_adapter_client__(std::shared_ptr<ClientAdapterParams> info) = 0;
        virtual bool connect_adapter_client__() = 0;
        virtual const ClientAdapterInfo& get_info_adapter_client__() const = 0;
};



}  // namespace rpp