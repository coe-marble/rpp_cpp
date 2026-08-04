#pragma once
#include <string>
#include <memory>
#include <chrono>
#include <functional>
#include "plugin_def.hpp"

namespace rpp {


struct ServerAdapterParams {
    using PluginPtr = std::unique_ptr<Plugin, std::function<void(Plugin*)>>;
    std::string name;
    std::string connection_name;
    std::string plugin_name;
    PluginPtr backend;
};


struct ClientAdapterParams {
    std::string name;
    std::string connection_name;
    std::string plugin_name;
};

struct ServerAdapterInfo {
    std::string name;
    std::string plugin_name;
    std::string plugin_type;
    std::string connection_name;
    std::chrono::milliseconds created_at;
};

struct ClientAdapterInfo {
    std::string name;
    std::string plugin_name;
    std::string plugin_type;
    std::string connection_name;
    std::chrono::milliseconds created_at;
};
}  // namespace rpp