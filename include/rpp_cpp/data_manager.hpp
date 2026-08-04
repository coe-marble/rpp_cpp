#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <fstream>
#include "data_model.hpp"
#include "rpp_paths.hpp"
#include "variant"


namespace rpp {


class RppDataManager {

    std::string rpp_home_dir;

    using json = nlohmann::json;

    std::string to_snake_case(const std::string& name) const
    {
        auto is_special_char = [](char c) {
            return c == '-' || c == ':' || c == '_';
        };

        std::string result;
        for (size_t i = 0; i < name.size(); ++i)
        {
            char c = name[i];
            if (is_special_char(c))
                result += '_';
            else if (std::isupper(c))
            {
                if (i > 0 && !is_special_char(name[i - 1]))
                    result += '_';
                result += std::tolower(c);
            }
            else
                result += c;
        }
        return result;
    }

    std::string get_plugin_id_from_name(const std::string& plugin_name)
    {
        return to_snake_case(plugin_name);
    }


    json load_json_file(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open JSON file at path: " + path);
        }
        return json::parse(file, nullptr, false);
    }

    json load_library_manifest(const std::string& library_name) {
        std::string manifest_path = get_app_library_manifest_path_json(library_name);
        return load_json_file(manifest_path);
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


    explicit RppDataManager(std::string rpp_home_dir) : rpp_home_dir(std::move(rpp_home_dir)) {}
    explicit RppDataManager() : rpp_home_dir(RPP_HOME) {}
    ~RppDataManager() = default;


    std::variant<ComponentRecord, LinkedComponentRecord>
        load_component_info(const std::string& component_path)
    {
        std::string description_path = get_component_description_path(component_path);
        std::ifstream description_file(description_path);
        if (!description_file.is_open()) {
            throw std::runtime_error("Failed to open component description file at path: " + description_path);
        }
        auto description_json = json::parse(description_file, nullptr, false);
        if (description_json.contains("LinkedComponentId")) {
            return LinkedComponentRecord::from_json(description_json, component_path);
        }
        else {
            return ComponentRecord::from_json(description_json, component_path);
        }
    }

    PluginInfo get_plugin_info_from_lib(const std::string& plugin_name,
            const std::string& lib_name = "")
    {
        auto library_name = lib_name;
        auto plugin_full_name = plugin_name;
        if (lib_name == "") {
            auto [library_name, _] = parse_plugin_name(plugin_name);
            (void)_; // Suppress unused variable warning
        }
        else
        {
            plugin_full_name = library_name + "::" + plugin_name;
        }
        auto path = get_app_registry_plugin_json_path(plugin_full_name);
        if (!std::filesystem::exists(path)) {
            throw std::runtime_error("Plugin JSON file not found for plugin: " + plugin_full_name);
        }

        auto as_json = load_json_file(path);
        return PluginInfo::from_json(as_json);
    }

    std::string get_subcomponent_folder_path(
        const std::string& parent_component_path, const std::string& subcomponent_id)
    {
        return parent_component_path + "/subcomponents/" + subcomponent_id;
    }

    std::string get_linked_component_folder_path(
        const std::string& parent_component_path,
        const std::string& plugin_name,
        const std::string& linked_component_id)
    {
        // From parent folder, go 2 levels up, then search from plugin_id
        // Inside, there should be linked_component_id foder
        std::string plugin_id = get_plugin_id_from_name(plugin_name);

        std::string full_path = parent_component_path
            + "/../../" + plugin_id + "/" + linked_component_id;

        if (!std::filesystem::exists(full_path)) {
            throw std::runtime_error("Linked component folder not found at path: " + full_path);
        }
        return full_path;
    }
};



} // namespace rpp
