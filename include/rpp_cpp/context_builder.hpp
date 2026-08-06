#pragma once
#include <string>
#include "context.hpp"
#include "parameter_handler.hpp"
#include "data_manager.hpp"
#include "plugin_loader.hpp"


namespace rpp {

    class ComponentContextBuilder {

        std::string parts_folder_;
        std::shared_ptr<rpp::RppDataManager> data_manager_own_;
        rpp::RppDataManager& data_manager_;
        ClockOptions clock_options_;

    public:
        ComponentContextBuilder(rpp::RppDataManager& data_manager,
            const ClockOptions& clock_options = ClockOptions())
            : data_manager_own_(nullptr),
              data_manager_(data_manager),
              clock_options_(clock_options){}

        explicit ComponentContextBuilder(const ClockOptions& clock_options = ClockOptions())
            : data_manager_own_(std::make_shared<rpp::RppDataManager>()),
              data_manager_(*data_manager_own_.get()),
              clock_options_(clock_options){}

        virtual ~ComponentContextBuilder() = default;

        ComponentContext build_from_component_path(const std::string& component_path)
        {
            return build_for_component(component_path);
        }

        ComponentContext build_from_script(
            const std::string& script_path,
            const std::string& parts_folder="")
        {
            auto script_description = data_manager_.load_script_description(script_path);
            if (script_description.components.empty()) {
                throw std::runtime_error("Script description does not contain any components.");
            }
            std::map<std::string, ComponentContext> subcomponents;
            if (parts_folder.empty()) {
                parts_folder_ = data_manager_.get_default_script_parts_folder_path(script_path);
            }
            else {
                parts_folder_ = parts_folder;
            }

            for (const auto& [slot_name, components] : script_description.components) {
                const auto& component = components.front();
                auto component_path = data_manager_.get_component_path_in_parts_folder(
                    parts_folder_, component.plugin_name, component.id);
                subcomponents.try_emplace(slot_name, build_for_component(component_path));
            }
            return ComponentContext(std::move(subcomponents), clock_options_);
        }

    private:

        struct SubcomponentAdapter {
            std::string plugin_name;
            std::string component_path;
        };

        using SubcomponentAdaptersMap =
            std::map<std::string, std::vector<SubcomponentAdapter>>;

        ComponentRecord resolve_component(
            const std::string& component_path,
            const std::string& parent_component_path,
            const std::string& plugin_name = "") const
        {
            auto component_record = data_manager_.load_component_info(component_path);
            if (std::holds_alternative<LinkedComponentRecord>(component_record)) {
                auto linked_record = std::get<LinkedComponentRecord>(component_record);
                if (plugin_name.empty()) {
                    throw std::runtime_error(
                        "Plugin name must be provided for linked components.");
                }
                auto linked_component_path = data_manager_.get_linked_component_folder_path(
                    parent_component_path, plugin_name, linked_record.linked_component_id);
                auto linked_component_record =
                    data_manager_.load_component_info(linked_component_path);
                if (std::holds_alternative<ComponentRecord>(linked_component_record)) {
                    return std::get<ComponentRecord>(linked_component_record);
                }
                else {
                    throw std::runtime_error("Doubly linked components are not supported.");
                }
            }
            else if (std::holds_alternative<ComponentRecord>(component_record)) {
                return std::get<ComponentRecord>(component_record);
            }
            else {
                throw std::runtime_error("Invalid component record type.");
            }
        }

        ComponentContext handle_cpp_component(
            const rpp::ComponentRecord& record,
            const rpp::PluginInfo& plugin_info,
            const std::string& parent_component_path) const
        {
            auto instance =
                rpp::load_cpp_plugin_from_shared_library(plugin_info);
            std::unique_ptr<rpp::params::Parameters> params;
            {
                // In scope to ensure ParameterHandler is destroyed before returning
                params::ParameterHandler parameter_handler(record.folder);
                auto params_module = parameter_handler.load_parameters_from_python_module();
                params::ParameterHandler::resolve_params(
                    plugin_info.plugin_metadata.parameters, params_module, params);
            }

            std::map<std::string, rpp::ComponentContext> subcomponents;
            for (const auto& [slot_name, subcomponent_info] : record.subcomponents) {
                auto subcomponent_path = data_manager_
                    .get_subcomponent_folder_path(record.folder, subcomponent_info.id);
                subcomponents.try_emplace(slot_name,
                    std::move(build_for_component(
                        subcomponent_path, parent_component_path,
                        subcomponent_info.plugin_name)));
            }
            return ComponentContext(
                std::move(instance), std::move(*params), std::move(subcomponents), clock_options_);

        }

        ComponentContext build_for_component(
            std::string component_path,
            std::string parent_component_path = "",
            std::string plugin_name = "") const
        {
            if (parent_component_path.empty()) {
                parent_component_path = component_path;
            }
            auto record = resolve_component(component_path,
                parent_component_path, plugin_name);

            rpp::PluginInfo plugin_info =
                data_manager_.get_plugin_info_from_lib(record.plugin_name);
            // SubcomponentAdaptersMap subcomponent_adapters;
            // if (plugin_info.source_language == "cpp")
            return handle_cpp_component(record,
                plugin_info, parent_component_path);
            // return handle_adapter_subcomponent(record, plugin_info, component_path, subcomponent_adapters);
        }

        // ComponentContext handle_adapter_subcomponent(
        //     const rpp::ComponentRecord& record,
        //     const rpp::PluginInfo& plugin_info,
        //     const std::string& component_path,
        //     SubcomponentAdaptersMap& subcomponent_adapters)
        // {
        //     auto source_language = plugin_info.source_language;
        //     subcomponent_adapters[source_language].push_back(
        //         SubcomponentAdapter{ record.plugin_name, component_path });
        //     std::string command = get_command_name_for_source_language(source_language) +
        //         " --host " + host_ +
        //         " --plugin-port " + std::to_string(plugin_port_) +
        //         " --runtime-port " + std::to_string(runtime_port_) +
        //         " --plugin " + plugin_info.name +
        //         " --home " + TestSuite::rpp_home_dir +
        //         " --component-path " + TestSuite::rpp_home_dir + "/components";

        //     return ComponentContext(command);
        // }


        std::string get_command_name_for_source_language(const std::string& source_language) const
        {
            if (source_language == "python")
                return "rpp_component_server_python";
            else if (source_language == "cpp")
                return "rpp_component_server_cpp";
            else
                throw std::runtime_error("Unsupported source language: " + source_language);
        }

    };
} /// namespace rpp