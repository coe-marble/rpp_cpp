#pragma once

#include <string>
#include <memory>
#include "plugin_def.hpp"
#include "parameters.hpp"
#include <functional>

namespace rpp {

    class ComponentContextBuilder;

    class ComponentContext {
        friend class ComponentContextBuilder;

    private:
        using PluginPtr = std::unique_ptr<Plugin, std::function<void(Plugin*)>>;
        params::Parameters parameters_;
        std::map<std::string, ComponentContext> subcomponents_;
        PluginPtr instance_;

        ComponentContext(
            PluginPtr&& instance,
            params::Parameters&& parameters,
            std::map<std::string, ComponentContext>&& subcomponents)
                : parameters_(std::move(parameters)),
                  subcomponents_(std::move(subcomponents)),
                  instance_(std::move(instance)) {}
    public:
        virtual ~ComponentContext() = default;
        ComponentContext(ComponentContext&& other) noexcept = default;
        ComponentContext& operator=(ComponentContext&& other) noexcept = default;

        // Forbid copy construction and copy assignment
        ComponentContext(const ComponentContext&) = delete;
        ComponentContext& operator=(const ComponentContext&) = delete;

        template <typename T>
        T get_parameter(const std::string& name) const
        {
            if (!parameters_.contains(name)) {
                throw std::runtime_error("Parameter '" + name + "' not found.");
            }
            return parameters_.get<T>(name);
        }

        template <typename T>
        T& get_component(const std::string& name)
        {
            if (subcomponents_.find(name) == subcomponents_.end()) {
                throw std::runtime_error("Subcomponent '" + name + "' not found.");
            }
            return subcomponents_.at(name).get_instance<T>();
        }

        std::vector<std::string> list_subcomponents() const
        {
            std::vector<std::string> names;
            for (const auto& [name, _] : subcomponents_) {
                names.push_back(name);
            }
            return names;
        }

        template <typename T>
        T& get_instance() const
        {
            T* instance_ptr = dynamic_cast<T*>(instance_.get());
            if (!instance_ptr) {
                throw std::runtime_error("Failed to cast plugin instance to the requested type.");
            }
            return *instance_ptr;
        }

        const ComponentContext& get_subcomponent_context(const std::string& name) const
        {
            if (subcomponents_.find(name) == subcomponents_.end()) {
                throw std::runtime_error("Subcomponent '" + name + "' not found.");
            }
            return subcomponents_.at(name);
        }

    };

} /// namespace rpp