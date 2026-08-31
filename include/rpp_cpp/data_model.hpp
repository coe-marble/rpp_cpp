#pragma once

#include <string>
#include <map>
#include <optional>
#include <functional>
#include <nlohmann/json_fwd.hpp>
#include "parameter_description.hpp"


// Forward declarations
class RppDataManager;

namespace rpp {

std::string get_safe_string_from_json(const nlohmann::json& j,
    const std::string& key, const std::string& default_val = "");

class ParentComponentInfo {
    public:
        std::string id;
        std::string plugin_type;
        std::string plugin_name;
        std::string slot_name;
        std::string library;
        bool is_linked = false;
};

class SubcomponentInfo {
    public:
        std::string id;
        std::string name;
        std::string plugin_type;
        std::string plugin_name;
        std::string library;
        std::string folder;
        std::string slot_name;
        bool is_linked = false;
};

class PluginInfo final {

    private:
        PluginInfo() = default;

    public:
        class PluginMetadata {
        public:
            std::map<std::string, std::string> components;
            std::map<std::string, params::ParameterValue> parameters;
        };
        std::string plugin_name;
        std::string source_language;
        std::string library;
        std::string plugin_type_name;
        std::string class_name;
        std::string source_file;
        std::string plugin_shared_library_path;
        std::string plugin_type_shared_library_path;
        PluginMetadata plugin_metadata;


    friend class RppDataManager;
    static PluginInfo from_json(const nlohmann::json& j);
};

class ComponentRecord {
    private:
        ComponentRecord() = default;
    public:
        std::string id;
        std::string name;
        std::string plugin_type;
        std::string plugin_name;
        std::string library;
        std::string folder;
        std::map<std::string, std::string> subcomponent_spec;
        std::map<std::string, std::vector<SubcomponentInfo>> subcomponents;
        std::optional<ParentComponentInfo> parent_component_info;

    static ComponentRecord from_json(
        const nlohmann::json& j, const std::string& component_path);
};


class LinkedComponentRecord {
    private:
        LinkedComponentRecord() = default;
    public:
        std::string id;
        std::string name;
        std::string folder;
        std::string linked_component_id;
        std::string linked_component_workspace;
        ParentComponentInfo parent_component_info;

    static LinkedComponentRecord from_json(
        const nlohmann::json& j, const std::string& component_path);
};

class ScriptDescription {
    public:

        struct ScriptComponent {
            std::string id;
            std::string plugin_name;
        };

        using ComponentAssignments =
            std::map<std::string, std::vector<ScriptComponent>>;

        std::string script_path;
        std::string language;
        std::map<std::string, ComponentAssignments> configurations;
        std::string active_configuration;
        std::map<std::string, std::string> spec;

    static ScriptDescription from_json(
        const nlohmann::json& j, const std::string& script_path);
};


inline std::string to_lower_copy(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

inline bool is_cpp_source_language(const PluginInfo& info) {
    return to_lower_copy(info.source_language) == "cpp";
}


}  // namespace rpp
