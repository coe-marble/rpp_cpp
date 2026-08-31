#pragma once
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "context.hpp"
#include "parameter_handler.hpp"
#include "data_manager.hpp"
#include "plugin_loader.hpp"
#include "network_utils.hpp"
#include "external_component_process.hpp"
#include "rpp_paths.hpp"


namespace rpp {

    class ComponentContextBuilder {

    public:
        struct ComponentRoot {
            std::string path;
            std::string plugin_name;
        };

    private:
        struct ComponentNode {
            std::string path;
            std::string parent_path;
            std::string plugin_name;
            ComponentRecord record;
            PluginInfo plugin_info;
        };

        struct PreparedAdaptedComponent {
            std::shared_ptr<Plugin> instance;
            std::shared_ptr<ExternalComponentProcess> process;
        };

        std::shared_ptr<rpp::RppDataManager> data_manager_own_;
        rpp::RppDataManager& data_manager_;
        ClockOptions clock_options_;
        std::map<std::string, PreparedAdaptedComponent> adapted_components_;
        ExternalComponentProcessRegistry external_component_process_registry_;

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

        void register_external_component_server(
            const std::string& language, const std::string& command)
        {
            external_component_process_registry_.register_server(language, command);
        }

        ComponentContext build_component_from_path(const std::string& component_path)
        {
            prepare_component_processes({
                make_component_node(component_path, component_path, "")});
            return build_for_component(component_path);
        }

        std::vector<ComponentContext> build_from_component_roots(
            const std::vector<ComponentRoot>& roots)
        {
            std::vector<ComponentNode> root_nodes;
            root_nodes.reserve(roots.size());
            for (const auto& root : roots) {
                root_nodes.push_back(make_component_node(
                    root.path, root.path, root.plugin_name));
            }
            prepare_component_processes(root_nodes);

            std::vector<ComponentContext> contexts;
            contexts.reserve(root_nodes.size());
            for (const auto& root : root_nodes) {
                contexts.push_back(build_for_component(
                    root.path, root.parent_path, root.plugin_name));
            }
            return contexts;
        }

        ComponentContext build_script_from_path(
            const std::string& script_path,
            std::optional<std::string> configuration = std::nullopt)
        {
            return build_script_from_description_path(
                data_manager_.get_default_script_description_path(script_path),
                std::move(configuration));
        }

        ComponentContext build_script_from_library(
            const std::string& library_name,
            const std::string& script_name,
            std::optional<std::string> configuration = std::nullopt)
        {
            return build_script_from_description_path(
                data_manager_.get_script_description_path_from_library(
                    library_name, script_name),
                std::move(configuration));
        }

    private:
        ComponentContext build_script_from_description_path(
            const std::string& script_description_path,
            std::optional<std::string> configuration)
        {
            const auto script_description = data_manager_.load_script_description(
                script_description_path);
            const auto& components = get_script_components(
                script_description, configuration);
            const auto parts_folder = data_manager_
                .get_default_script_parts_folder_path_from_description(
                    script_description_path);

            std::vector<ComponentNode> roots;
            for (const auto& [slot_name, assigned_components] : components) {
                (void)slot_name;
                for (const auto& component : assigned_components) {
                    const auto component_path =
                        data_manager_.get_component_path_in_parts_folder(
                            parts_folder, component.plugin_name, component.id);
                    roots.push_back(make_component_node(
                        component_path, component_path, component.plugin_name));
                }
            }
            prepare_component_processes(roots);

            std::map<std::string, std::vector<ComponentContext>> subcomponents;
            for (const auto& [slot_name, assigned_components] : components) {
                auto& slot_subcomponents = subcomponents[slot_name];
                for (const auto& component : assigned_components) {
                    const auto component_path =
                        data_manager_.get_component_path_in_parts_folder(
                            parts_folder, component.plugin_name, component.id);
                    slot_subcomponents.push_back(build_for_component(
                        component_path, component_path, component.plugin_name));
                }
            }
            return ComponentContext(std::move(subcomponents), clock_options_);
        }

        static const ScriptDescription::ComponentAssignments&
        get_script_components(
            const ScriptDescription& script_description,
            const std::optional<std::string>& configuration)
        {
            if (script_description.configurations.empty()) {
                throw std::runtime_error(
                    "Script description does not define configurations.");
            }
            const auto& selected_configuration = configuration.has_value()
                ? *configuration : script_description.active_configuration;
            const auto selected = script_description.configurations.find(
                selected_configuration);
            if (selected == script_description.configurations.end()) {
                throw std::runtime_error(
                    "Script configuration '" + selected_configuration +
                    "' is not defined.");
            }
            return selected->second;
        }

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

        ComponentContext build_native_component(
            const rpp::ComponentRecord& record,
            const rpp::PluginInfo& plugin_info) const
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

            std::map<std::string, std::vector<rpp::ComponentContext>> subcomponents;
            for (const auto& [slot_name, subcomponent_infos] : record.subcomponents) {
                auto& slot_subcomponents = subcomponents[slot_name];
                for (const auto& subcomponent_info : subcomponent_infos) {
                    auto subcomponent_path = data_manager_
                        .get_subcomponent_folder_path(
                            record.folder, subcomponent_info.id);
                    slot_subcomponents.push_back(build_for_component(
                        subcomponent_path, record.folder,
                        subcomponent_info.plugin_name));
                }
            }
            return ComponentContext(
                std::move(instance), std::move(*params), std::move(subcomponents), clock_options_);

        }

        ComponentNode make_component_node(
            const std::string& component_path,
            const std::string& parent_component_path,
            const std::string& plugin_name) const
        {
            auto record = resolve_component(
                component_path, parent_component_path, plugin_name);
            auto plugin_info = data_manager_.get_plugin_info_from_lib(
                record.plugin_name);
            return {
                component_path, parent_component_path, plugin_name,
                record, plugin_info,
            };
        }

        void collect_component_nodes(
            const ComponentNode& node,
            std::vector<ComponentNode>& nodes) const
        {
            if (!is_cpp_source_language(node.plugin_info)) {
                nodes.push_back(node);
                return;
            }
            nodes.push_back(node);
            for (const auto& [slot_name, subcomponents] : node.record.subcomponents) {
                (void)slot_name;
                for (const auto& subcomponent : subcomponents) {
                    collect_component_nodes(
                        make_component_node(
                            data_manager_.get_subcomponent_folder_path(
                                node.record.folder, subcomponent.id),
                            node.path,
                            subcomponent.plugin_name),
                        nodes);
                }
            }
        }

        void prepare_component_processes(
            const std::vector<ComponentNode>& roots)
        {
            adapted_components_.clear();
            std::vector<ComponentNode> nodes;
            for (const auto& root : roots) {
                collect_component_nodes(root, nodes);
            }

            std::map<std::string, std::vector<ComponentNode>> groups;
            for (auto& node : nodes) {
                if (!is_cpp_source_language(node.plugin_info)) {
                    groups[to_lower_copy(node.plugin_info.source_language)].push_back(
                        std::move(node));
                }
            }

            std::set<std::string> connection_names;
            for (auto& [language, group] : groups) {
                std::vector<ExternalComponentSpec> component_specs;
                component_specs.reserve(group.size());
                for (const auto& node : group) {
                    const auto connection_name = node.record.id + "_connection";
                    if (!connection_names.insert(connection_name).second) {
                        throw std::runtime_error(
                            "Duplicate adapted component connection name '" +
                            connection_name + "'.");
                    }
                    component_specs.push_back({
                        node.record.folder,
                        node.plugin_info,
                        connection_name,
                    });
                }
                auto prepared = ExternalComponentProcess::create(
                    language, RPP_HOME, component_specs,
                    external_component_process_registry_);
                for (size_t index = 0; index < group.size(); ++index) {
                    adapted_components_[group[index].record.folder] = {
                        std::move(prepared.instances[index]), prepared.process,
                    };
                }
            }
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
            if (is_cpp_source_language(plugin_info)) {
                return build_native_component(
                    record, plugin_info);
            }
            auto prepared = adapted_components_.find(record.folder);
            if (prepared == adapted_components_.end()) {
                throw std::runtime_error(
                    "Adapted component was not prepared before context construction.");
            }

            std::unique_ptr<params::Parameters> params;
            params::ParameterHandler parameter_handler(record.folder);
            auto params_module = parameter_handler.load_parameters_from_python_module();
            params::ParameterHandler::resolve_params(
                plugin_info.plugin_metadata.parameters, params_module, params);
            std::map<std::string, std::vector<ComponentContext>> subcomponents;
            return ComponentContext(
                prepared->second.instance, std::move(*params),
                prepared->second.process, std::move(subcomponents), clock_options_);
        }

    };
} /// namespace rpp
