#include "rpp_cpp/plugin_loader.hpp"

namespace rpp {

RppPtr<params::RppParameters_T> load_cpp_plugin_parameters_description(
    const PluginInfo& info,
    const std::string& create_symbol) {
    auto plugin_ptr = load_from_shared_library__<params::RppParameters_T>(
        info.plugin_shared_library_path, create_symbol, false, true);
    return plugin_ptr;
}

RppPtr<params::RppComponents_T> load_cpp_plugin_component_spec(
    const PluginInfo& info,
    const std::string& create_symbol) {
    auto plugin_ptr = load_from_shared_library__<params::RppComponents_T>(
        info.plugin_shared_library_path, create_symbol, false, true);
    return plugin_ptr;
}

RppPtr<Plugin> load_cpp_plugin_from_shared_library(
    const PluginInfo& info,
    const std::string& create_symbol) {
    return load_from_shared_library__<Plugin>(
            info.plugin_shared_library_path, create_symbol);
}

RppPtr<ClientAdapter> load_plugin_adapter_client(
        const PluginInfo& info,
        const std::string& name,
        const std::string& connection_name,
        const std::string& create_symbol,
        std::shared_ptr<RppLogger> logger
    ) {
    ClientAdapterParams info_adapter;
    if (name.empty()) {
        info_adapter.name = info.plugin_name + "_adapter_client";
    }
    else {
        info_adapter.name = name;
    }
    if (connection_name.empty()) {
        info_adapter.connection_name = info.plugin_name + "_connection";
    }
    else {
        info_adapter.connection_name = connection_name;
    }

    auto client = load_from_shared_library__<ClientAdapter>(
            info.plugin_type_shared_library_path, create_symbol,
            true, false, std::move(logger));
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
        const std::string& name,
        const std::string& connection_name,
        const std::string& create_symbol,
        std::shared_ptr<RppLogger> logger) {

    return load_plugin_adapter_server(
        info, std::shared_ptr<Plugin>(std::move(plugin_ptr)),
        name, connection_name, create_symbol, std::move(logger));
}

RppPtr<ServerAdapter> load_plugin_adapter_server(
        const PluginInfo& info,
        std::shared_ptr<Plugin> plugin_ptr,
        const std::string& name,
        const std::string& connection_name,
        const std::string& create_symbol,
        std::shared_ptr<RppLogger> logger) {

    auto server_ptr = load_from_shared_library__<ServerAdapter>(
            info.plugin_type_shared_library_path, create_symbol,
            true, false, std::move(logger));

    ServerAdapterParams info_adapter;
    info_adapter.backend = plugin_ptr;
    info_adapter.plugin_name = info.plugin_name;
    if (name.empty()) {
        info_adapter.name = info.plugin_name + "_adapter_server";
    }
    else {
        info_adapter.name = name;
    }
    if (connection_name.empty()) {
        info_adapter.connection_name = info.plugin_name + "_connection";
    }
    else {
        info_adapter.connection_name = connection_name;
    }

    bool result = server_ptr->configure_adapter_server__(
            std::make_shared<ServerAdapterParams>(std::move(info_adapter)));

    if (!result) {
        throw std::runtime_error("Failed to configure server adapter for plugin: " + info.plugin_name);
    }

    return server_ptr;
}
}  // namespace rpp
