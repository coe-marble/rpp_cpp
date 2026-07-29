#pragma once

#include <string>
#include <map>
#include <optional>
#include <functional>
#include <nlohmann/json.hpp>
#include "parameter_description.hpp"

class RppDataManager;

namespace rpp {

std::string get_safe_string_from_json(const nlohmann::json& j,
    const std::string& key, const std::string& default_val = "")
    {
        return (j.contains(key) && !j[key].is_null()) ? j[key].get<std::string>() : default_val;
    };

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
        std::string plugin_path;
        std::string source_file;
        std::string plugin_shared_library_path;
        std::string plugin_type_shared_library_path;
        PluginMetadata plugin_metadata;


    friend class RppDataManager;

    static PluginInfo from_json(const nlohmann::json& j) {
        PluginInfo info;
        info.plugin_name = get_safe_string_from_json(j, "PluginName", "");
        info.source_language = get_safe_string_from_json(j, "SourceLanguage", "");
        info.library = get_safe_string_from_json(j, "Library", "");
        info.plugin_type_name = get_safe_string_from_json(j, "PluginType", "");
        info.class_name = get_safe_string_from_json(j, "ClassName", "");
        info.plugin_path = get_safe_string_from_json(j, "PluginPath", "");
        info.source_file = get_safe_string_from_json(j, "SourceFile", "");
        info.plugin_shared_library_path = get_safe_string_from_json(j, "PluginSharedLibraryPath", "");
        info.plugin_type_shared_library_path = get_safe_string_from_json(j, "PluginTypeSharedLibraryPath", "");


        std::function<params::ParameterValue(const nlohmann::json&)>
        parse_parameter_value =
            [&parse_parameter_value](const nlohmann::json& param_value) -> params::ParameterValue
        {
            auto type = param_value.value("type", "");
            if (type == "int") {
                return params::ParameterValue(param_value.value("default_value", 0));
            }
            else if (type == "float") {
                return params::ParameterValue(param_value.value("default_value", 0.0));
            }
            else if (type == "bool") {
                return params::ParameterValue(param_value.value("default_value", false));
            }
            else if (type == "string") {
                return params::ParameterValue(param_value.value("default_value", std::string()));
            }
            else if (type == "array") {
                std::vector<params::ParameterValue> list_values;
                if (param_value.contains("elements") && param_value["elements"].is_array()) {
                    for (const auto& item : param_value["elements"]) {
                        list_values.push_back(parse_parameter_value(item));
                    }
                }
                return params::ParameterValue(list_values);
            }
            else if (type == "object") {
                std::map<std::string, params::ParameterValue> dict_values;
                if (param_value.contains("fields") && param_value["fields"].is_object()) {
                    for (auto& [key, value] : param_value["fields"].items()) {
                        dict_values[key] = parse_parameter_value(value);
                    }
                }
                return params::ParameterValue(dict_values);
            }
            else {
                throw std::runtime_error("Unsupported parameter value type.");
            }
        };


        if (j.contains("PluginMetadata") && j["PluginMetadata"].is_object())
        {
            const auto& metadata_json = j["PluginMetadata"];
            if (metadata_json.contains("Components") && metadata_json["Components"].is_object())
            {
                for (auto& [comp_id, comp_name] : metadata_json["Components"].items())
                    info.plugin_metadata.components[comp_id] = comp_name.get<std::string>();
            }
            if (metadata_json.contains("Parameters") && metadata_json["Parameters"].is_object())
            {
                for (auto& [param_name, param_value] : metadata_json["Parameters"].items())
                    info.plugin_metadata.parameters[param_name] = parse_parameter_value(param_value);
            }
        }

        return info;
    }

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
        std::map<std::string, SubcomponentInfo> subcomponents;
        std::optional<ParentComponentInfo> parent_component_info;

    static ComponentRecord from_json(
        const nlohmann::json& j, const std::string& component_path)
    {
        ComponentRecord record;
        record.id = get_safe_string_from_json(j, "Id", "");
        record.name = get_safe_string_from_json(j, "Name", "");
        record.plugin_type = get_safe_string_from_json(j, "PluginType", "");
        record.plugin_name = get_safe_string_from_json(j, "PluginName", "");
        record.folder = component_path;
        record.library = get_safe_string_from_json(j, "Library", "");

        if (j.contains("SubcomponentSpec") && j["SubcomponentSpec"].is_object()) {
            for (auto& [slot_name, sub_name] : j["SubcomponentSpec"].items()) {
                record.subcomponent_spec[slot_name] = sub_name.get<std::string>();
            }
        }

        if (j.contains("ParentComponentInfo") && j["ParentComponentInfo"].is_object()) {
            const auto& parent_json = j["ParentComponentInfo"];
            ParentComponentInfo parent_info;
            parent_info.id = get_safe_string_from_json(parent_json, "ComponentID", "");
            parent_info.plugin_type = get_safe_string_from_json(parent_json, "PluginType", "");
            parent_info.plugin_name = get_safe_string_from_json(parent_json, "PluginName", "");
            parent_info.slot_name = get_safe_string_from_json(parent_json, "SlotName", "");
            parent_info.library = get_safe_string_from_json(parent_json, "Library", "");
            parent_info.is_linked = parent_json.value("IsLinked", false);
            record.parent_component_info = parent_info;
        }

        if (j.contains("Subcomponents") && j["Subcomponents"].is_object()) {
            for (auto& [slot_name, sub_info] : j["Subcomponents"].items()) {
                SubcomponentInfo subcomponent;
                subcomponent.id = get_safe_string_from_json(sub_info, "Id", "");
                subcomponent.name = get_safe_string_from_json(sub_info, "Name", "");
                subcomponent.plugin_type = get_safe_string_from_json(sub_info, "PluginType", "");
                subcomponent.plugin_name = get_safe_string_from_json(sub_info, "PluginName", "");
                subcomponent.library = get_safe_string_from_json(sub_info, "Library", "");
                subcomponent.folder = get_safe_string_from_json(sub_info, "Folder", "");
                subcomponent.slot_name = get_safe_string_from_json(sub_info, "SlotName", "");
                subcomponent.is_linked = sub_info.value("IsLinked", false);
                record.subcomponents[slot_name] = subcomponent;
            }
        }
        return record;
    }
};


class LinkedComponentRecord {
    private:
        LinkedComponentRecord() = default;
    public:
        std::string id;
        std::string name;
        std::string folder;
        std::string linked_component_id;
        ParentComponentInfo parent_component_info;

    static LinkedComponentRecord from_json(
        const nlohmann::json& j, const std::string& component_path)
    {
        LinkedComponentRecord record;
        record.id = get_safe_string_from_json(j, "ComponentID", "");
        record.name = get_safe_string_from_json(j, "ComponentName", "");
        record.folder = component_path;
        record.linked_component_id = get_safe_string_from_json(j, "LinkedComponentID", "");
        if (j.contains("ParentComponent") && j["ParentComponent"].is_object()) {
            const auto& parent_json = j["ParentComponent"];
            ParentComponentInfo parent_info;
            parent_info.id = get_safe_string_from_json(parent_json, "ComponentID", "");
            parent_info.plugin_type = get_safe_string_from_json(parent_json, "PluginType", "");
            parent_info.plugin_name = get_safe_string_from_json(parent_json, "PluginName", "");
            parent_info.slot_name = get_safe_string_from_json(parent_json, "SlotName", "");
            parent_info.library = get_safe_string_from_json(parent_json, "Library", "");
            parent_info.is_linked = parent_json.value("IsLinked", false);
            record.parent_component_info = parent_info;
        }
        return record;
    }
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