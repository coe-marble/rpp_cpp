#include "rpp_cpp/rpp_paths.hpp"

namespace rpp {

std::string RPP_HOME = std::getenv("RPP_HOME") ?
    std::string(std::getenv("RPP_HOME"))
    : std::getenv("HOME") ? std::string(std::getenv("HOME")) + "/.rpp" : "/tmp/.rpp";


std::tuple<std::string, std::string> parse_plugin_name(const std::string& plugin_name) {
    auto pos = plugin_name.find("::");
    if (pos == std::string::npos) {
        throw std::invalid_argument(
            "Invalid plugin name format. Expected 'library_name::plugin_name'");
    }
    std::string lib_name = plugin_name.substr(0, pos);
    std::string plugin_type = plugin_name.substr(pos + 2);
    return {lib_name, plugin_type};
}

std::string get_component_description_path(const std::string& component_path) {
    return component_path + "/description.json";
}

std::string get_home_dir() {
    return RPP_HOME;
}

std::string get_app_registry_dir() {
    return RPP_HOME + "/registry";
}

std::string get_app_libraries_dir() {
    return RPP_HOME + "/libraries";
}

std::string get_app_interfaces_dir() {
    return RPP_HOME + "/interfaces";
}

std::string get_app_registry_json_path() {
    return RPP_HOME + "/registry/rpp_plugin_types.json";
}

std::string get_app_registry_plugin_type_json_path(std::string plugin_type_name) {
    auto [lib_name, plugin_type] = parse_plugin_name(plugin_type_name);
    return RPP_HOME + "/registry/libraries/"
        + lib_name + "/rpp_plugin_types/"
        + plugin_type + ".json";
}

std::string get_app_registry_plugin_json_path(std::string plugin_name) {
    auto [lib_name, plugin_type] = parse_plugin_name(plugin_name);
    return RPP_HOME + "/registry/libraries/"
        + lib_name + "/rpp_plugins/"
        + plugin_type + ".json";
}

std::string get_app_plugin_types_dir() {
    return RPP_HOME + "/interfaces";
}

std::string get_app_library_manifest_path_json(const std::string& library_name) {
    return get_app_registry_dir() + "/" + library_name + "/manifest.json";
}

} // namespace rpp