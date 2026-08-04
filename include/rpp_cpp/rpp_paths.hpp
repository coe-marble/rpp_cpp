#pragma once
#include <string>
#include <filesystem>


namespace rpp {

extern std::string RPP_HOME;
const std::string LIBRARY_PACKAGE_FILENAME = "package.json";
const std::string LIBRARY_PLUGINS_FILENAME = "plugins.json";
const std::string LIBRARY_MANIFEST_FILENAME = "manifest.json";

std::tuple<std::string, std::string> parse_plugin_name(
    const std::string& plugin_name);

std::string get_component_description_path(const std::string& component_path);

std::string get_home_dir();

std::string get_app_registry_dir();

std::string get_app_libraries_dir();

std::string get_app_interfaces_dir();

std::string get_app_registry_json_path();

std::string get_app_registry_plugin_type_json_path(std::string plugin_type_name);

std::string get_app_registry_plugin_json_path(std::string plugin_name);

std::string get_app_plugin_types_dir();

std::string get_app_library_manifest_path_json(const std::string& library_name);

}