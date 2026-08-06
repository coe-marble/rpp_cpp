#pragma once

#include <string>
#include <memory>
#include "plugin_def.hpp"
#include "parameters.hpp"
#include <functional>
#include "logger.hpp"
#include "clock.hpp"

namespace rpp {

    class ComponentContextBuilder;

    class ComponentContext {
        friend class ComponentContextBuilder;

    private:
        using PluginPtr = std::unique_ptr<Plugin, std::function<void(Plugin*)>>;
        using PluginPtrShared = std::shared_ptr<Plugin>;
        params::Parameters parameters_;
        std::map<std::string, ComponentContext> subcomponents_;
        PluginPtrShared instance_;
        std::shared_ptr<rpp::RppLogger> logger_;
        std::shared_ptr<rpp::RppClock> clock_;

        ComponentContext(
            PluginPtr&& instance,
            params::Parameters&& parameters,
            std::map<std::string, ComponentContext>&& subcomponents,
            const ClockOptions& clock_options)
                : parameters_(std::move(parameters)),
                  subcomponents_(std::move(subcomponents)),
                  instance_(std::move(instance)),
                  logger_(std::make_shared<rpp::RppLogger>()),
                  clock_(ClockFactory::create_clock(clock_options))
            {}

        // for script-based components that don't have an instance
        ComponentContext(
            std::map<std::string, ComponentContext>&& subcomponents,
            const ClockOptions& clock_options)
                : parameters_(),
                subcomponents_(std::move(subcomponents)),
                instance_(nullptr),
                logger_(std::make_shared<rpp::RppLogger>()),
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
            if (instance_) {
                instance_->initialize(*this);
            }
            for (auto& [name, subcomponent] : subcomponents_) {
                subcomponent.initialize();
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
            return subcomponents_.at(name);
        }

    };

} /// namespace rpp