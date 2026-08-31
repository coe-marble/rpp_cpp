#pragma once

#include <dlfcn.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "plugin_def.hpp"
#include "data_model.hpp"
#include "adapter_bases.hpp"
#include "rpp_paths.hpp"
#include "parameter_description.hpp"
#include "logger.hpp"


namespace rpp {

template <typename BaseType>
using RppPtr = std::unique_ptr<BaseType, std::function<void(BaseType*)>>;


RppPtr<params::RppParameters_T> load_cpp_plugin_parameters_description(
    const PluginInfo& info,
    const std::string& create_symbol = "get_plugin_parameters_description");

RppPtr<params::RppComponents_T> load_cpp_plugin_component_spec(
    const PluginInfo& info,
    const std::string& create_symbol = "get_plugin_components");

RppPtr<Plugin> load_cpp_plugin_from_shared_library(
    const PluginInfo& info,
    const std::string& create_symbol = "create_plugin");

RppPtr<ClientAdapter> load_plugin_adapter_client(
        const PluginInfo& info,
        const std::string& name = "",
        const std::string& connection_name = "",
        const std::string& create_symbol = "create_plugin_client",
        std::shared_ptr<RppLogger> logger = nullptr);

RppPtr<ServerAdapter> load_plugin_adapter_server(
        const PluginInfo& info,
        RppPtr<Plugin>&& plugin_ptr,
        const std::string& name = "",
        const std::string& connection_name = "",
        const std::string& create_symbol = "create_plugin_server",
        std::shared_ptr<RppLogger> logger = nullptr);

RppPtr<ServerAdapter> load_plugin_adapter_server(
        const PluginInfo& info,
        std::shared_ptr<Plugin> plugin_ptr,
        const std::string& name = "",
        const std::string& connection_name = "",
        const std::string& create_symbol = "create_plugin_server",
        std::shared_ptr<RppLogger> logger = nullptr);

template <typename BaseType, typename... FactoryArgs>
RppPtr<BaseType> load_from_shared_library__(
    const std::string& shared_library_path,
    const std::string& create_symbol = "create_plugin",
    bool delete_on_close = true,
    bool allow_nullptr = false,
    FactoryArgs&&... factory_args) {

    auto registry_dir = get_app_registry_dir();  // Ensure the registry directory is set up
    auto shared_library_path_abs = registry_dir + "/" + shared_library_path;

    void* handle = dlopen(shared_library_path_abs.c_str(), RTLD_NOW);
    if (handle == nullptr) {
        throw std::runtime_error(std::string("dlopen failed for '")
            + shared_library_path_abs + "': " + dlerror());
    }

    dlerror();
    using FactoryFn = BaseType* (*)(std::decay_t<FactoryArgs>...);
    void* symbol = dlsym(handle, create_symbol.c_str());
    const char* symbol_error = dlerror();
    if (symbol_error != nullptr || symbol == nullptr) {
        dlclose(handle);
        throw std::runtime_error(std::string("Failed to resolve symbol '")
            + create_symbol + "' in '" + shared_library_path_abs + "': "
            + (symbol_error != nullptr ? symbol_error : "unknown error"));
    }

    auto factory = reinterpret_cast<FactoryFn>(symbol);
    BaseType* raw_plugin = factory(std::forward<FactoryArgs>(factory_args)...);
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


template <typename PluginBase>
RppPtr<PluginBase> load_cpp_plugin_from_shared_library(
    const PluginInfo& info,
    const std::string& create_symbol = "create_plugin") {
    return load_from_shared_library__<PluginBase>(
            info.plugin_shared_library_path, create_symbol);
}

template <typename BaseType>
RppPtr<typename BaseType::AdapterClient> load_plugin_adapter_client(
    const std::string& name = "",
    const std::string& connection_name = "",
    std::shared_ptr<RppLogger> logger = nullptr)
{
    // Here you can use the plugin_ptr as needed, for example, store it in a registry or call its methods.

    auto client = std::make_unique<typename BaseType::AdapterClient>(
        std::move(logger));

    ClientAdapterParams info_adapter;
    if (name.empty()) {
        info_adapter.name =
            client->get_info_adapter_client__().plugin_name + "_adapter_client";
    } else {
        info_adapter.name = name;
    }
    if (connection_name.empty()) {
        info_adapter.connection_name =
            client->get_info_adapter_client__().plugin_name + "_connection";
    } else {
        info_adapter.connection_name = connection_name;
    }
    client->configure_adapter_client__(
        std::make_shared<ClientAdapterParams>(std::move(info_adapter)));

    return client;
}

template <typename PluginBase>
RppPtr<typename PluginBase::AdapterServer> load_plugin_adapter_server(
        RppPtr<PluginBase>&& plugin_ptr,
        const std::string& name = "",
        const std::string& connection_name = "",
        std::shared_ptr<RppLogger> logger = nullptr) {
    // Here you can use the plugin_ptr as needed, for example, store it in a registry or call its methods.
    auto server = std::make_unique<typename PluginBase::AdapterServer>(
        std::move(logger));

    ServerAdapterParams info;

    info.backend = std::shared_ptr<Plugin>(std::move(plugin_ptr));

    if (name.empty()) {
        info.name =
            server->get_info_adapter_server__().plugin_name + "_adapter_server";
    } else {
        info.name = name;
    }
    if (connection_name.empty()) {
        info.connection_name =
            server->get_info_adapter_server__().plugin_name + "_connection";
    } else {
        info.connection_name = connection_name;
    }

    server->configure_adapter_server__(
        std::make_shared<ServerAdapterParams>(std::move(info)));

    return server;
}

}  // namespace rpp
