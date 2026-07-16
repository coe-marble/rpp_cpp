#pragma once

#include <dlfcn.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "plugin.hpp"
#include "plugin_info.hpp"
#include "adapter_info.hpp"
#include "rpp_paths.hpp"
#include "capnp/ez-rpc.h"


namespace rpp {

template <typename PluginBase>
using PluginPtr = std::shared_ptr<PluginBase>;

template <typename PluginBase>
PluginPtr<PluginBase> load_from_shared_library__(
    const std::string& shared_library_path,
    const std::string& create_symbol = "create_plugin") {

    auto registry_dir = get_registry_dir();  // Ensure the registry directory is set up
    auto shared_library_path_abs = registry_dir + "/" + shared_library_path;

    void* handle = dlopen(shared_library_path_abs.c_str(), RTLD_NOW);
    if (handle == nullptr) {
        throw std::runtime_error(std::string("dlopen failed for '")
            + shared_library_path_abs + "': " + dlerror());
    }

    dlerror();
    using FactoryFn = PluginBase* (*)();
    void* symbol = dlsym(handle, create_symbol.c_str());
    const char* symbol_error = dlerror();
    if (symbol_error != nullptr || symbol == nullptr) {
        dlclose(handle);
        throw std::runtime_error(std::string("Failed to resolve symbol '")
            + create_symbol + "' in '" + shared_library_path_abs + "': "
            + (symbol_error != nullptr ? symbol_error : "unknown error"));
    }

    auto factory = reinterpret_cast<FactoryFn>(symbol);
    PluginBase* raw_plugin = factory();
    if (raw_plugin == nullptr) {
        dlclose(handle);
        throw std::runtime_error(std::string("Factory symbol '")
            + create_symbol + "' returned a null plugin for '"
            + shared_library_path_abs + "'.");
    }

    return PluginPtr<PluginBase>(raw_plugin, [handle](PluginBase* plugin) {
        delete plugin;
        if (handle != nullptr) {
            dlclose(handle);
        }
    });
}



template <typename PluginBase>
PluginPtr<PluginBase> load_cpp_plugin_from_shared_library(
    const PluginInfo& info,
    const std::string& create_symbol = "create_plugin") {
    return load_from_shared_library__<PluginBase>(
            info.plugin_shared_library_path, create_symbol);
}


PluginPtr<Plugin> load_cpp_plugin_from_shared_library(
    const PluginInfo& info,
    const std::string& create_symbol = "create_plugin") {
    return load_from_shared_library__<Plugin>(
            info.plugin_shared_library_path, create_symbol);
}

PluginPtr<ClientAdapter> load_plugin_adapter_client(
        const PluginInfo& info,
        const std::string& host,
        uint16_t port,
        const std::string& name = "",
        const std::string& create_symbol = "create_plugin_client") {
    ClientAdapterParams info_adapter;
    if (name.empty()) {
        info_adapter.name = info.plugin_name + "_adapter_client";
    }
    else {
        info_adapter.name = name;
    }
    info_adapter.host = host;
    info_adapter.port = port;
    auto client = load_from_shared_library__<ClientAdapter>(
            info.plugin_type_shared_library_path, create_symbol);
    auto result = client->configure_adapter_client__(
            std::make_shared<ClientAdapterParams>(std::move(info_adapter)));
    if (!result) {
        throw std::runtime_error("Failed to configure client adapter for plugin: "
            + info.plugin_name);
    }
    return client;

}

PluginPtr<ServerAdapter> load_plugin_adapter_server(
        const PluginInfo& info,
        const PluginPtr<Plugin>& plugin_ptr,
        const std::string& host,
        uint16_t port,
        const std::string& name = "",
        const std::string& create_symbol = "create_plugin_server") {

    auto server_ptr = load_from_shared_library__<ServerAdapter>(
            info.plugin_type_shared_library_path, create_symbol);

    ServerAdapterParams info_adapter;
    info_adapter.backend = plugin_ptr;
    info_adapter.plugin_name = info.plugin_name;
    if (name.empty()) {
        info_adapter.name = info.plugin_name + "_adapter_server";
    }
    else {
        info_adapter.name = name;
    }
    info_adapter.host = host;
    info_adapter.port = port;
    bool result = server_ptr->configure_adapter_server__(
            std::make_shared<ServerAdapterParams>(std::move(info_adapter)));

    if (!result) {
        throw std::runtime_error("Failed to configure server adapter for plugin: " + info.plugin_name);
    }

    return server_ptr;
}



enum class AdapterMode {
    Client,
    Server
};

template <typename PluginBase>
PluginPtr<typename PluginBase::AdapterClient> load_plugin_adapter_client(
    const std::string& host, uint16_t port, const std::string& name = "") {
    // Here you can use the plugin_ptr as needed, for example, store it in a registry or call its methods.

    auto client = std::make_shared<typename PluginBase::AdapterClient>();

    ClientAdapterParams info_adapter;
    if (name.empty()) {
        info_adapter.name = client->get_info_adapter_client__().plugin_name + "_adapter_client";
    } else {
        info_adapter.name = name;
    }
    info_adapter.name = name;
    info_adapter.host = host;
    info_adapter.port = port;

    client->configure_adapter_client__(std::make_shared<ClientAdapterParams>(std::move(info_adapter)));

    return client;
}

template <typename PluginBase>
PluginPtr<typename PluginBase::AdapterServer> load_plugin_adapter_server(
        const PluginPtr<PluginBase>& plugin_ptr,
        const std::string& host,
        uint16_t port, const std::string& name = "") {
    // Here you can use the plugin_ptr as needed, for example, store it in a registry or call its methods.
    auto server = std::make_shared<typename PluginBase::AdapterServer>();

    ServerAdapterParams info;
    info.backend = plugin_ptr;
    if (name.empty()) {
        info.name = server->get_info_adapter_server__().plugin_name + "_adapter_server";
    } else {
        info.name = name;
    }

    info.host = host;
    info.port = port;
    server->configure_adapter_server__(std::make_shared<ServerAdapterParams>(std::move(info)));

    return server;
}


}  // namespace rpp