#pragma once

#include <string>
#include <memory>
#include <map>
#include <vector>
#include "plugin_def.hpp"
#include "parameters.hpp"
#include <functional>
#include "logger.hpp"
#include "clock.hpp"

namespace rpp {

    class ComponentContextBuilder;
    class ExternalComponentProcess;

    class ComponentContext {
        friend class ComponentContextBuilder;
        friend class ComponentContextAccess;

    private:
        using PluginPtr = std::unique_ptr<Plugin, std::function<void(Plugin*)>>;
        using PluginPtrShared = std::shared_ptr<Plugin>;
        using SubcomponentMap =
            std::map<std::string, std::vector<ComponentContext>>;
        params::Parameters parameters_;
        SubcomponentMap subcomponents_;
        std::shared_ptr<ExternalComponentProcess> external_component_process_;
        PluginPtrShared instance_;
        std::shared_ptr<rpp::RppLogger> logger_;
        std::shared_ptr<rpp::RppClock> clock_;

        ComponentContext(
            PluginPtr&& instance,
            params::Parameters&& parameters,
            SubcomponentMap&& subcomponents,
            const ClockOptions& clock_options)
                : parameters_(std::move(parameters)),
                  subcomponents_(std::move(subcomponents)),
                  external_component_process_(nullptr),
                  instance_(std::move(instance)),
                  logger_(std::make_shared<rpp::RppLogger>()),
                  clock_(ClockFactory::create_clock(clock_options))
            {}

          ComponentContext(
            PluginPtrShared instance,
            params::Parameters&& parameters,
            std::shared_ptr<ExternalComponentProcess> external_component_process,
            SubcomponentMap&& subcomponents,
            const ClockOptions& clock_options)
                : parameters_(std::move(parameters)),
                subcomponents_(std::move(subcomponents)),
                external_component_process_(std::move(external_component_process)),
                instance_(std::move(instance)),
                logger_(std::make_shared<rpp::RppLogger>()),
                clock_(ClockFactory::create_clock(clock_options))
            {}

        // for script-based components that don't have an instance
        ComponentContext(
            SubcomponentMap&& subcomponents,
            const ClockOptions& clock_options)
                : parameters_(),
                subcomponents_(std::move(subcomponents)),
                external_component_process_(nullptr),
                instance_(nullptr),
                logger_(std::make_shared<rpp::RppLogger>()),
                clock_(ClockFactory::create_clock(clock_options))
            {}

        explicit ComponentContext(
            std::shared_ptr<rpp::RppLogger> logger,
            const ClockOptions& clock_options = ClockOptions())
            : parameters_(),
              subcomponents_(),
              external_component_process_(nullptr),
              instance_(nullptr),
              logger_(logger ? std::move(logger)
                             : std::make_shared<rpp::RppLogger>()),
              clock_(ClockFactory::create_clock(clock_options))
        {}

    public:
        virtual ~ComponentContext() = default;
        ComponentContext(ComponentContext&& other) noexcept = default;
        ComponentContext& operator=(ComponentContext&& other) noexcept = default;

        // Forbid copy construction and copy assignment
        ComponentContext(const ComponentContext&) = delete;
        ComponentContext& operator=(const ComponentContext&) = delete;


        std::shared_ptr<rpp::RppLogger> get_logger() const{
            return logger_;
        }

        std::shared_ptr<rpp::RppClock> get_clock() const{
            return clock_;
        }


        void initialize() const {
            for (const auto& [name, subcomponents] : subcomponents_) {
                (void)name;
                for (const auto& subcomponent : subcomponents) {
                    subcomponent.initialize();
                }
            }
            if (instance_) {
                instance_->initialize(*this);
            }
        }

        template <typename T>
        T get_parameter(const std::string& name) const
        {
            if (!parameters_.contains(name)) {
                throw std::runtime_error("Parameter '" + name + "' not found.");
            }
            return parameters_.get<T>(name);
        }

        template <typename T>
        std::shared_ptr<T> get_component(const std::string& name) const
        {
            if (subcomponents_.find(name) == subcomponents_.end()) {
                throw std::runtime_error("Subcomponent '" + name + "' not found.");
            }
            const auto& subcomponents = subcomponents_.at(name);
            if (subcomponents.empty()) {
                throw std::runtime_error(
                    "Subcomponent slot '" + name + "' is empty.");
            }
            return subcomponents.front().get_instance<T>();
        }

        template <typename T>
        std::vector<std::shared_ptr<T>> get_components(const std::string& name) const
        {
            if (subcomponents_.find(name) == subcomponents_.end()) {
                throw std::runtime_error("Subcomponent '" + name + "' not found.");
            }
            std::vector<std::shared_ptr<T>> instances;
            for (const auto& subcomponent : subcomponents_.at(name)) {
                instances.push_back(subcomponent.get_instance<T>());
            }
            return instances;
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
        std::shared_ptr<T> get_instance() const
        {
            auto casted_instance = std::static_pointer_cast<T>(instance_);
            if (!casted_instance) {
                throw std::runtime_error("Failed to cast instance to the requested type.");
            }
            return casted_instance;
        }

        const ComponentContext& get_subcomponent_context(const std::string& name) const
        {
            if (subcomponents_.find(name) == subcomponents_.end()) {
                throw std::runtime_error("Subcomponent '" + name + "' not found.");
            }
            const auto& subcomponents = subcomponents_.at(name);
            if (subcomponents.empty()) {
                throw std::runtime_error(
                    "Subcomponent slot '" + name + "' is empty.");
            }
            return subcomponents.front();
        }

        const std::vector<ComponentContext>& get_subcomponent_contexts(
            const std::string& name) const
        {
            if (subcomponents_.find(name) == subcomponents_.end()) {
                throw std::runtime_error("Subcomponent '" + name + "' not found.");
            }
            return subcomponents_.at(name);
        }

    };

} /// namespace rpp
