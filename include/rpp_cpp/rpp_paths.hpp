#pragma once
#include <string>

namespace rpp {

std::string RPP_HOME = "";

std::string LIBRARY_PACKAGE_FILENAME = "package.json";
std::string LIBRARY_PLUGINS_FILENAME = "plugins.json";
std::string LIBRARY_MANIFEST_FILENAME = "manifest.json";

std::string get_home_dir() {
    return RPP_HOME;
}

std::string get_registry_dir() {
    return RPP_HOME + "/registry";
}

std::string get_libraries_dir() {
    return RPP_HOME + "/libraries";
}

std::string get_interfaces_dir() {
    return RPP_HOME + "/interfaces";
}

std::string get_plugin_types_registry_path() {
    return RPP_HOME + "/registry/rpp_plugin_types.registry.json";
}

std::string get_plugin_types_dir() {
    return RPP_HOME + "/interfaces";
}

std::string get_library_manifest_path(const std::string& library_name) {
    return get_libraries_dir() + "/" + library_name + "/autogen/" + LIBRARY_MANIFEST_FILENAME;
}

}