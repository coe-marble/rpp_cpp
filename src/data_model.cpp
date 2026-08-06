#include "rpp_cpp/data_model.hpp"
#include <nlohmann/json.hpp>

namespace rpp {

std::string get_safe_string_from_json(const nlohmann::json& j,
    const std::string& key, const std::string& default_val)
{
    return (j.contains(key) && !j[key].is_null()) ? j[key].get<std::string>() : default_val;
}


PluginInfo PluginInfo::from_json(const nlohmann::json& j) {
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


ComponentRecord ComponentRecord::from_json(
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

LinkedComponentRecord LinkedComponentRecord::from_json(
    const nlohmann::json& j, const std::string& component_path)
{
    LinkedComponentRecord record;
    record.id = get_safe_string_from_json(j, "Id", "");
    record.name = get_safe_string_from_json(j, "ComponentName", "");
    record.folder = component_path;
    record.linked_component_id = get_safe_string_from_json(j, "LinkedComponentId", "");
    record.linked_component_workspace = get_safe_string_from_json(j, "LinkedComponentWorkspace", "");
    if (j.contains("ParentComponent") && j["ParentComponent"].is_object()) {
        const auto& parent_json = j["ParentComponent"];
        ParentComponentInfo parent_info;
        parent_info.id = get_safe_string_from_json(parent_json, "Id", "");
        parent_info.plugin_type = get_safe_string_from_json(parent_json, "PluginType", "");
        parent_info.plugin_name = get_safe_string_from_json(parent_json, "PluginName", "");
        parent_info.slot_name = get_safe_string_from_json(parent_json, "SlotName", "");
        parent_info.library = get_safe_string_from_json(parent_json, "Library", "");
        parent_info.is_linked = parent_json.value("IsLinked", false);
        record.parent_component_info = parent_info;
    }
    return record;
}

ScriptDescription ScriptDescription::from_json(
    const nlohmann::json& j, const std::string& script_path)
{
    ScriptDescription description;
    description.script_path = get_safe_string_from_json(j, "ScriptPath", script_path);
    description.language = get_safe_string_from_json(j, "Language", "");
    if (j.contains("Components") && j["Components"].is_object()) {
        for (auto& [slot_name, components_array] : j["Components"].items()) {
            if (components_array.is_array()) {
                std::vector<ScriptComponent> components;
                for (const auto& comp_json : components_array) {
                    ScriptComponent component;
                    component.id = get_safe_string_from_json(comp_json, "Id", "");
                    component.plugin_name = get_safe_string_from_json(comp_json, "PluginName", "");
                    components.push_back(component);
                }
                description.components[slot_name] = components;
            }
        }
    }
    return description;
}

}