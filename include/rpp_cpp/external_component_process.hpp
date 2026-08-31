#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "adapter_bases.hpp"
#include "component_call_executor.hpp"
#include "child_process.hpp"
#include "data_model.hpp"
#include "network_utils.hpp"
#include "plugin_loader.hpp"
#include "plugin_runtime.hpp"
#include "logger.hpp"

namespace rpp {

using ClientAdapterOwner = std::shared_ptr<ClientAdapter>;

struct ExternalComponentSpec {
    std::string component_path;
    PluginInfo plugin_info;
    std::string connection_name;
};

class ExternalComponentProcessRegistry final {
public:
    ExternalComponentProcessRegistry()
    {
        register_server("python", "rpp_component_server_python");
        register_server("cpp", "rpp_component_server_cpp");
    }

    void register_server(const std::string& language, const std::string& command)
    {
        if (language.empty() || command.empty()) {
            throw std::invalid_argument(
                "External component server language and command are required.");
        }
        commands_[to_lower_copy(language)] = command;
    }

    const std::string& command_for(const std::string& language) const
    {
        auto server = commands_.find(to_lower_copy(language));
        if (server == commands_.end()) {
            throw std::runtime_error(
                "No external component server is registered for source language '" +
                language + "'.");
        }
        return server->second;
    }

private:
    std::map<std::string, std::string> commands_;
};

class ExternalComponentProcess final {
public:
    struct PreparedComponents {
        std::shared_ptr<ExternalComponentProcess> process;
        std::vector<std::shared_ptr<Plugin>> instances;
    };

    static PreparedComponents create(
        const std::string& language,
        const std::string& rpp_home,
        const std::vector<ExternalComponentSpec>& components,
        const ExternalComponentProcessRegistry& process_registry,
        const std::string& host = "127.0.0.1",
        std::shared_ptr<RppLogger> logger = nullptr)
    {
        if (components.empty()) {
            throw std::invalid_argument(
                "External component runtime requires a component.");
        }

        const auto port = get_available_port();
        const auto& server_command = process_registry.command_for(language);
        if (!logger) {
            logger = std::make_shared<RppLogger>(LoggerOptions{
                LogLevel::DEBUG, "rpp_external_runtime"});
        }
        std::vector<ClientAdapterOwner> adapter_owners;
        std::vector<std::shared_ptr<Plugin>> instances;
        std::vector<std::string> command{
            server_command, "--host", host,
            "--port", std::to_string(port), "--home", rpp_home,
        };
        for (const auto& component : components) {
            auto adapter = load_plugin_adapter_client(
                component.plugin_info,
                component.plugin_info.plugin_name + "_client",
                component.connection_name,
                "create_plugin_client",
                logger);
            auto plugin = dynamic_cast<Plugin*>(adapter.get());
            if (plugin == nullptr) {
                throw std::runtime_error(
                    "Adapted component client does not implement Plugin.");
            }
            std::shared_ptr<ClientAdapter> adapter_owner = std::move(adapter);
            adapter_owners.push_back(adapter_owner);
            instances.emplace_back(adapter_owner, plugin);
            command.insert(command.end(), {
                "--path", component.component_path,
                "--plugin", component.plugin_info.plugin_name,
                "--conn", component.connection_name,
            });
        }

        return {
            std::make_shared<ExternalComponentProcess>(
                std::move(command), host, port, std::move(adapter_owners),
                std::chrono::seconds(5), std::move(logger)),
            std::move(instances),
        };
    }

    ExternalComponentProcess(
        std::vector<std::string> command,
        const std::string& host,
        uint16_t port,
        std::vector<ClientAdapterOwner> adapter_clients,
        std::chrono::milliseconds startup_timeout = std::chrono::seconds(5),
        std::shared_ptr<RppLogger> logger = nullptr)
        : process_(std::move(command)),
          runtime_client_context_(nullptr),
          adapter_clients_(std::move(adapter_clients)),
          logger_(logger ? std::move(logger) : std::make_shared<RppLogger>(
              LoggerOptions{LogLevel::DEBUG, "rpp_external_runtime"}))
    {
        if (adapter_clients_.empty()) {
            throw std::invalid_argument(
                "External component runtime requires an adapter client.");
        }
        process_.start();
        executor_.start();
        executor_started_ = true;
        RPP_LOG_DEBUG(*logger_, "Child process started; waiting for runtime.");
        try {
            executor_.call([this, &host, port, startup_timeout](kj::AsyncIoContext& io) {
                RPP_LOG_DEBUG(*logger_, "Connecting to runtime host=%s port=%d.",
                              host.c_str(), port);
                runtime_client_context_ = std::make_unique<RppRuntimeClientContext>(
                    host, port, startup_timeout, &io);
                for (const auto& adapter_client : adapter_clients_) {
                    if (!adapter_client->connect_adapter_client__(
                            *runtime_client_context_, std::ref(executor_))) {
                        throw std::runtime_error(
                            "Failed to connect external component adapter client.");
                    }
                }
                runtime_client_ = std::make_unique<PluginRuntimeClient>(
                    *runtime_client_context_, logger_);
                RPP_LOG_DEBUG(*logger_, "Runtime connection established.");
            });
        } catch (const kj::Exception& error) {
            RPP_LOG_ERROR(*logger_, "Runtime connection failed: %s.",
                          error.getDescription().cStr());
            throw std::runtime_error(
                "Timed out connecting to external component server.");
        }
    }

    ~ExternalComponentProcess()
    {
        if (executor_started_) {
            try {
                executor_.call([this](kj::AsyncIoContext&) {
                    adapter_clients_.clear();
                    if (runtime_client_ != nullptr) {
                        runtime_client_->shutdown();
                        runtime_client_.reset();
                    }
                    runtime_client_context_.reset();
                });
            } catch (...) {
            }
            executor_.stop();
            executor_started_ = false;
        }
        try {
            process_.wait(std::chrono::seconds(1));
        } catch (...) {
        }
    }

    ExternalComponentProcess(const ExternalComponentProcess&) = delete;
    ExternalComponentProcess& operator=(const ExternalComponentProcess&) = delete;

private:
    ChildProcess process_;
    ComponentCallExecutor executor_;
    bool executor_started_ = false;
    std::unique_ptr<RppRuntimeClientContext> runtime_client_context_;
    std::vector<ClientAdapterOwner> adapter_clients_;
    std::shared_ptr<RppLogger> logger_;
    std::unique_ptr<PluginRuntimeClient> runtime_client_;
};

}  // namespace rpp
