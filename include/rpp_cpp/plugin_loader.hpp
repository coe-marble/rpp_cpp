#pragma once

#include <dlfcn.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "plugin_def.hpp"
#include "data_model.hpp"
#include "adapter_info.hpp"
#include "rpp_paths.hpp"
#include "capnp/ez-rpc.h"
#include "parameter_description.hpp"


namespace rpp {

template <typename BaseType>
using RppPtr = std::unique_ptr<BaseType, std::function<void(BaseType*)>>;

template <typename BaseType>
RppPtr<BaseType> load_from_shared_library__(
    const std::string& shared_library_path,
    const std::string& create_symbol = "create_plugin",
    bool delete_on_close = true,
    bool allow_nullptr = false) {

    auto registry_dir = get_app_registry_dir();  // Ensure the registry directory is set up
    auto shared_library_path_abs = registry_dir + "/" + shared_library_path;

    void* handle = dlopen(shared_library_path_abs.c_str(), RTLD_NOW);
    if (handle == nullptr) {
        throw std::runtime_error(std::string("dlopen failed for '")
            + shared_library_path_abs + "': " + dlerror());
    }

    dlerror();
    using FactoryFn = BaseType* (*)();
    void* symbol = dlsym(handle, create_symbol.c_str());
    const char* symbol_error = dlerror();
    if (symbol_error != nullptr || symbol == nullptr) {
        dlclose(handle);
        throw std::runtime_error(std::string("Failed to resolve symbol '")
            + create_symbol + "' in '" + shared_library_path_abs + "': "
            + (symbol_error != nullptr ? symbol_error : "unknown error"));
    }

    auto factory = reinterpret_cast<FactoryFn>(symbol);
    BaseType* raw_plugin = factory();
    if (raw_plugin == nullptr) {
        dlclose(handle);
        if (allow_nullptr) {
            return nullptr;
        }
        else {
            throw std::runtime_error(std::string("Factory symbol '")
                + create_symbol + "' returned a null plugin for '"
                + shared_library_path_abs + "'.");
        }
    }

    if (delete_on_close)
    {
        return RppPtr<BaseType>(raw_plugin, [handle](BaseType* plugin) {
            delete plugin;
            if (handle != nullptr) {
                dlclose(handle);
            }
        });
    }
    else
    {
        return RppPtr<BaseType>(raw_plugin, [handle](BaseType* /* plugin*/) {
            if (handle != nullptr) {
                dlclose(handle);
            }
        });
    }
}

RppPtr<params::RppParameters_T> load_cpp_plugin_parameters_description(
    const PluginInfo& info,
    const std::string& create_symbol = "get_plugin_parameters_description") {
    auto plugin_ptr = load_from_shared_library__<params::RppParameters_T>(
        info.plugin_shared_library_path, create_symbol, false, true);
    return plugin_ptr;
}

RppPtr<params::RppComponents_T> load_cpp_plugin_component_spec(
    const PluginInfo& info,
    const std::string& create_symbol = "get_plugin_components") {
    auto plugin_ptr = load_from_shared_library__<params::RppComponents_T>(
        info.plugin_shared_library_path, create_symbol, false, true);
    return plugin_ptr;
}

template <typename PluginBase>
RppPtr<PluginBase> load_cpp_plugin_from_shared_library(
    const PluginInfo& info,
    const std::string& create_symbol = "create_plugin") {
    return load_from_shared_library__<PluginBase>(
            info.plugin_shared_library_path, create_symbol);
}


RppPtr<Plugin> load_cpp_plugin_from_shared_library(
    const PluginInfo& info,
    const std::string& create_symbol = "create_plugin") {
    return load_from_shared_library__<Plugin>(
            info.plugin_shared_library_path, create_symbol);
}

RppPtr<ClientAdapter> load_plugin_adapter_client(
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

RppPtr<ServerAdapter> load_plugin_adapter_server(
        const PluginInfo& info,
        RppPtr<Plugin>&& plugin_ptr,
        const std::string& host,
        uint16_t port,
        const std::string& name = "",
        const std::string& create_symbol = "create_plugin_server") {

    auto server_ptr = load_from_shared_library__<ServerAdapter>(
            info.plugin_type_shared_library_path, create_symbol);

    ServerAdapterParams info_adapter;
    info_adapter.backend = std::move(plugin_ptr);
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

template <typename BaseType>
RppPtr<typename BaseType::AdapterClient> load_plugin_adapter_client(
    const std::string& host, uint16_t port, const std::string& name = "") {
    // Here you can use the plugin_ptr as needed, for example, store it in a registry or call its methods.

    auto client = std::make_unique<typename BaseType::AdapterClient>();

    ClientAdapterParams info_adapter;
    if (name.empty()) {
        info_adapter.name =
            client->get_info_adapter_client__().plugin_name + "_adapter_client";
    } else {
        info_adapter.name = name;
    }
    info_adapter.name = name;
    info_adapter.host = host;
    info_adapter.port = port;

    client->configure_adapter_client__(
        std::make_shared<ClientAdapterParams>(std::move(info_adapter)));

    return client;
}

template <typename PluginBase>
RppPtr<typename PluginBase::AdapterServer> load_plugin_adapter_server(
        RppPtr<PluginBase>&& plugin_ptr,
        const std::string& host,
        uint16_t port, const std::string& name = "") {
    // Here you can use the plugin_ptr as needed, for example, store it in a registry or call its methods.
    auto server = std::make_unique<typename PluginBase::AdapterServer>();

    ServerAdapterParams info;

    auto old_deleter = std::move(plugin_ptr.get_deleter());
    auto* raw_derived = plugin_ptr.release();

    // Set up the backend with a custom deleter that calls the old deleter
    info.backend = RppPtr<Plugin>(raw_derived, [old_deleter](Plugin* p) {
        old_deleter(static_cast<PluginBase*>(p));
    });

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