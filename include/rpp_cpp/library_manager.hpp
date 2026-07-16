# pragma once
# include <string>
# include <vector>

#include <nlohmann/json.hpp>
#include <fstream>
# include "plugin_info.hpp"
#include "rpp_paths.hpp"





namespace rpp {


class LibraryManager {

    using json = nlohmann::json;

    std::string rpp_home_dir;

    std::vector<std::string> split_plugin_name(const std::string& name) {
        std::vector<std::string> result;
        size_t start = 0;
        size_t end = name.find("::");
        while (end != std::string::npos) {
            result.push_back(name.substr(start, end - start));
            start = end + 2; // Move past the "::"
            end = name.find("::", start);
        }
        result.push_back(name.substr(start)); // Add the last part
        return result;
    }

    json load_library_manifest(const std::string& library_name) {
        std::string manifest_path = get_library_manifest_path(library_name);

        std::ifstream manifest_file(manifest_path);
        if (!manifest_file.is_open()) {
            throw std::runtime_error("Failed to open manifest file for library "
                + library_name + " at path: " + manifest_path);
        }

        json manifest_json = json::parse(manifest_file, nullptr, false);
        return manifest_json;
    }

    json get_library_plugins(const std::string& library_name) {
        json manifest_json = load_library_manifest(library_name);
        if (!manifest_json.contains("Plugins")) {
            throw std::runtime_error("Manifest file for library " + library_name + " does not contain 'Plugins' key.");
        }
        return manifest_json["Plugins"];
    }

    json get_library_plugin_types(const std::string& library_name) {
        json manifest_json = load_library_manifest(library_name);
        if (!manifest_json.contains("plugin_types")) {
            throw std::runtime_error("Manifest file for library " + library_name + " does not contain 'plugin_types' key.");
        }
        return manifest_json["plugin_types"];
    }


public:


    explicit LibraryManager(std::string rpp_home_dir) : rpp_home_dir(std::move(rpp_home_dir)) {}
    ~LibraryManager() = default;

    PluginInfo get_plugin_info(const std::string& plugin_name, const std::string& lib_name = "") {
        auto library_name = lib_name;
        auto plugin_full_name = plugin_name;
        if (lib_name == "") {
            auto splits = split_plugin_name(plugin_name);
            if (splits.size() != 2) {
                throw std::invalid_argument("Invalid plugin name format. Expected 'library::plugin_name'.");
            }
            library_name = splits[0];
        }
        else
        {
            plugin_full_name = library_name + "::" + plugin_name;
        }

        auto get_safe_string = [](const nlohmann::json& j, const std::string& key, const std::string& default_val = "") -> std::string {
            return (j.contains(key) && !j[key].is_null()) ? j[key].get<std::string>() : default_val;
        };

        auto plugins_json = get_library_plugins(library_name);
        // iterate over plugins_json dict to find the plugin_in_library
        for (const auto& [key, value] : plugins_json.items()) {
            if (key == plugin_full_name) {
                PluginInfo info
                {
                    get_safe_string(value, "PluginName", ""),
                    get_safe_string(value, "SourceLanguage", ""),
                    get_safe_string(value, "Library", ""),
                    get_safe_string(value, "PluginType", ""),
                    get_safe_string(value, "ClassName", ""),
                    get_safe_string(value, "PluginPath", ""),
                    get_safe_string(value, "SourceFile", ""),
                    get_safe_string(value, "PluginSharedLibraryPath", ""),
                    get_safe_string(value, "PluginTypeSharedLibraryPath", ""),
                    value.value("Components", std::map<std::string, std::string>{})
                };
                return info;
            }
        }
        throw std::runtime_error("Plugin not found: " + plugin_full_name);
    }



};


} // namespace rpp
