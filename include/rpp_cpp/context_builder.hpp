#include <string>
#include "context.hpp"
#include "parameter_handler.hpp"
#include "data_manager.hpp"
#include "plugin_loader.hpp"


namespace rpp {

    class __attribute__((visibility("hidden")))
    ComponentContextBuilder {

        std::string component_path_;
        rpp::RppDataManager& data_manager_;
        pybind11::scoped_interpreter& python_interpreter_;
        std::string host_;
        uint16_t plugin_port_;
        uint16_t runtime_port_;

    public:
        ComponentContextBuilder(rpp::RppDataManager& data_manager,
            const std::string& component_path,
            pybind11::scoped_interpreter& python_interpreter,
            const std::string& host = "", uint16_t plugin_port = 0,
            uint16_t runtime_port = 0)
            : component_path_(component_path),
              data_manager_(data_manager),
              python_interpreter_(python_interpreter),
              host_(host),
              plugin_port_(plugin_port),
              runtime_port_(runtime_port) {}

        virtual ~ComponentContextBuilder() = default;

        ComponentContext build() const
        {
            return build_for_component(component_path_);
        }

    private:

        struct SubcomponentAdapter {
            std::string plugin_name;
            std::string component_path;
        };

        ComponentContext build_for_component(std::string component_path, bool is_subcomponent = false) const
        {
            auto component_record = data_manager_.load_component_info(component_path);
            if (std::holds_alternative<LinkedComponentRecord>(component_record)) {
                throw std::runtime_error("Linked components are not supported in this context builder.");
            }
            else if (std::holds_alternative<ComponentRecord>(component_record)) {
            }
            else {
                throw std::runtime_error("Invalid component record type.");
            }
            rpp::ComponentRecord record = std::get<ComponentRecord>(component_record);
            rpp::PluginInfo plugin_info =
                data_manager_.get_plugin_info_from_lib(record.plugin_name);

            if (plugin_info.source_language == "cpp")
                return handle_cpp_subcomponent(record, plugin_info, component_path);
            return handle_adapter_subcomponent(record, plugin_info, component_path, subcomponent_adapters);
        }

        ComponentContext handle_cpp_subcomponent(
            const rpp::ComponentRecord& record,
            const rpp::PluginInfo& plugin_info,
            const std::string& component_path) const
        {
            auto instance =
                rpp::load_cpp_plugin_from_shared_library(plugin_info);
            std::unique_ptr<rpp::params::Parameters> params;
            {
                // In scope to ensure ParameterHandler is destroyed before returning
                params::ParameterHandler parameter_handler(component_path, python_interpreter_);
                auto params_module = parameter_handler.load_parameters_from_python_module();
                params::ParameterHandler::resolve_params(
                    plugin_info.plugin_metadata.parameters, params_module, params);
            }

            std::map<std::string, rpp::ComponentContext> subcomponents;
            for (const auto& [slot_name, subcomponent_info] : record.subcomponents) {
                auto subcomponent_path = data_manager_
                    .get_subcomponent_folder_path(component_path, subcomponent_info.id);
                subcomponents.try_emplace(slot_name,
                    std::move(build_for_component(subcomponent_path, true)));
            }
            return ComponentContext(
                std::move(instance), std::move(*params), std::move(subcomponents));

        }

        ComponentContext handle_adapter_subcomponent(
            const rpp::ComponentRecord& record,
            const rpp::PluginInfo& plugin_info,
            const std::string& component_path,
            std::map<std::string, std::vector<SubcomponentAdapter>>& subcomponent_adapters) const
        {
            auto source_language = plugin_info.source_language;
            subcomponent_adapters[source_language].push_back(
                SubcomponentAdapter{ record.plugin_name, component_path });
            std::string command = get_command_name_for_source_language(source_language) +
                " --host " + host_ +
                " --plugin-port " + std::to_string(plugin_port_) +
                " --runtime-port " + std::to_string(runtime_port_) +
                " --plugin " + plugin_info.name +
                " --home " + TestSuite::rpp_home_dir +
                " --component-path " + TestSuite::rpp_home_dir + "/components";

            return ComponentContext(command);
        }


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