#pragma once
#include <algorithm>
#include <filesystem>
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
    std::filesystem::path workspace_path_;

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

    json load_json_file(const std::string& path) const {
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


    explicit RppDataManager(
        std::string rpp_home_dir, std::string workspace_path = "")
        : rpp_home_dir(std::move(rpp_home_dir)),
          workspace_path_(normalize_workspace_path(workspace_path)) {}
    explicit RppDataManager() : rpp_home_dir(RPP_HOME) {}
    ~RppDataManager() = default;


    ScriptDescription load_script_description(const std::string& script_path) {
        std::ifstream description_file(script_path);
        if (!description_file.is_open()) {
            throw std::runtime_error("Failed to open script description file at path: " + script_path);
        }
        auto description_json = json::parse(description_file, nullptr, false);
        return ScriptDescription::from_json(description_json, script_path);
    }

    std::string get_default_script_description_path(
        const std::string& script_path) const
    {
        auto current_path = std::filesystem::path(script_path).parent_path();
        while (!current_path.empty()) {
            const auto workspace_path = current_path / ".rppws";
            if (std::filesystem::is_directory(workspace_path)) {
                return (workspace_path / "script_descriptions" /
                    (std::filesystem::path(script_path).stem().string() +
                     ".json")).string();
            }
            const auto parent_path = current_path.parent_path();
            if (parent_path == current_path) {
                break;
            }
            current_path = parent_path;
        }
        throw std::runtime_error(
            "Could not find a .rppws folder above script '" + script_path + "'.");
    }

    std::string get_script_description_path_from_library(
        const std::string& library_name, const std::string& script_name) const
    {
        if (workspace_path_.empty()) {
            throw std::runtime_error(
                "A workspace path is required to resolve a script by library and name.");
        }

        const auto separator = script_name.find("::");
        const auto unqualified_name = separator == std::string::npos
            ? script_name : script_name.substr(separator + 2);
        if (separator != std::string::npos &&
            script_name.substr(0, separator) != library_name) {
            throw std::invalid_argument(
                "Script '" + script_name + "' does not belong to library '" +
                library_name + "'.");
        }

        const auto descriptions_path = workspace_path_ / "script_descriptions";
        if (!std::filesystem::is_directory(descriptions_path)) {
            throw std::runtime_error(
                "Script descriptions folder not found: " +
                descriptions_path.string());
        }

        std::vector<std::filesystem::path> descriptions;
        for (const auto& entry :
             std::filesystem::directory_iterator(descriptions_path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                descriptions.push_back(entry.path());
            }
        }
        std::sort(descriptions.begin(), descriptions.end());

        const auto qualified_name = library_name + "::" + unqualified_name;
        std::vector<std::filesystem::path> matches;
        for (const auto& description_path : descriptions) {
            const auto description = load_json_file(description_path.string());
            const auto stored_name = get_safe_string_from_json(
                description, "ScriptName", "");
            if (get_safe_string_from_json(description, "ScriptLibrary", "") ==
                    library_name &&
                (stored_name == unqualified_name || stored_name == qualified_name)) {
                matches.push_back(description_path);
            }
        }

        if (matches.empty()) {
            throw std::runtime_error(
                "Script '" + qualified_name + "' is not present in workspace '" +
                workspace_path_.parent_path().string() + "'.");
        }
        if (matches.size() > 1) {
            throw std::runtime_error(
                "Multiple descriptions found for script '" + qualified_name + "'.");
        }
        return matches.front().string();
    }

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

    std::string get_default_script_parts_folder_path_from_description(
        const std::string& script_description_path)
    {
        // Go up two levels to reach the script's root folder
        std::filesystem::path script_dir =
            std::filesystem::path(script_description_path).parent_path().parent_path();
        std::filesystem::path parts_dir = script_dir / "parts";
        return parts_dir.string();
    }



    std::string get_component_path_in_parts_folder(
        const std::string& parts_folder,
        const std::string& plugin_name,
        const std::string& component_id)
    {
        std::string plugin_id = get_plugin_id_from_name(plugin_name);
        std::string full_path = parts_folder + "/" + plugin_id + "/" + component_id;
        if (!std::filesystem::exists(full_path)) {
            throw std::runtime_error("Component folder not found at path: " + full_path);
        }
        return full_path;
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

private:
    static std::filesystem::path normalize_workspace_path(
        const std::string& workspace_path)
    {
        if (workspace_path.empty()) {
            return {};
        }
        const auto normalized_path = std::filesystem::absolute(workspace_path);
        return normalized_path.filename() == ".rppws"
            ? normalized_path : normalized_path / ".rppws";
    }
};



} // namespace rpp
