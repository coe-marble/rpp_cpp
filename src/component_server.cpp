#include "rpp_cpp/plugin_loader.hpp"
#include "rpp_cpp/data_manager.hpp"
#include "rpp_cpp/context_builder.hpp"
#include "rpp_cpp/rpp_server_host.hpp"
#include "rpp_cpp/logger.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <string>
#include <csignal>

namespace {
rpp::RppServerHost* active_server_host = nullptr;

void handle_interrupt(int)
{
    if (active_server_host != nullptr) {
        active_server_host->shutdown();
    }
}
}  // namespace

int main(int argc, char **argv) {
    rpp::LoggerOptions logger_options;
    logger_options.level = rpp::LogLevel::DEBUG;
    logger_options.name = "rpp_component_server_cpp";
    auto logger = std::make_shared<rpp::RppLogger>(logger_options);
    RPP_LOG_DEBUG(*logger, "Starting component server (argc=%d).", argc);

    // parse command line arguments,
    std::string home_dir = "";
    std::string host = "localhost";
    uint16_t port = 8080;
    std::vector<std::string> paths;
    std::vector<std::string> conns;
    std::vector<std::string> plugin_names;

    // get user home directory
    const char* home_env = std::getenv("HOME");
    if (home_env != nullptr) {
        home_dir = std::string(home_env) + "/.rpp";
    }

    const char* rpp_home_env = std::getenv("RPP_HOME");
    if (rpp_home_env != nullptr) {
        home_dir = std::string(rpp_home_env);
    }


    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [--help|-h]" << std::endl;
            return 0;
        }

        if (arg == "--home" && i + 1 < argc) {
            std::string path = argv[++i];
            rpp::RPP_HOME = path;
        }

        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        }
        if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }

        if (arg == "--path" && i + 1 < argc) {
            std::string path = argv[++i];
            paths.push_back(path);
        }

        if (arg == "--plugin" && i + 1 < argc) {
            std::string plugin_name = argv[++i];
            plugin_names.push_back(plugin_name);
        }
        if (arg == "--conn" && i + 1 < argc) {
            std::string connection_name = argv[++i];
            conns.push_back(connection_name);
        }
    }

    if (paths.empty() || conns.empty() || plugin_names.empty()) {
        std::cerr << "Error: Missing required arguments. Please provide --path, --conn, and --plugin." << std::endl;
        return 1;
    }

    if (paths.size() != conns.size() || paths.size() != plugin_names.size()) {
        std::cerr << "Error: The number of --path, --conn, and --plugin arguments must be the same." << std::endl;
        return 1;
    }

    RPP_LOG_DEBUG(*logger, "Parsed host=%s port=%d home=%s components=%zu.",
                 host.c_str(), port, rpp::RPP_HOME.c_str(), paths.size());

    rpp::RppDataManager data_manager(rpp::RPP_HOME);
    rpp::RppServerHost server_host(host, port, logger);
    active_server_host = &server_host;
    std::signal(SIGINT, handle_interrupt);
    std::signal(SIGTERM, handle_interrupt);
    rpp::ComponentContextBuilder context_builder(data_manager);

    std::cout << "Starting component server on " << host << ":" << port << std::endl;

    std::vector<rpp::ComponentContextBuilder::ComponentRoot> roots;
    roots.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        const auto plugin_info = data_manager.get_plugin_info_from_lib(plugin_names[i]);
        if (!rpp::is_cpp_source_language(plugin_info)) {
            std::cerr << "Error: component_server_cpp can only host C++ root components: "
                      << plugin_names[i] << std::endl;
            return 1;
        }
        roots.push_back({paths[i], plugin_names[i]});
    }

    std::cout << "Building component contexts from roots." << std::endl;

    auto contexts = context_builder.build_from_component_roots(roots);
    RPP_LOG_DEBUG(*logger, "Contexts built.");
    for (size_t i = 0; i < contexts.size(); ++i) {
        const auto& conn = conns[i];
        auto plugin_info = data_manager.get_plugin_info_from_lib(plugin_names[i]);
        assert(!plugin_info.plugin_name.empty() && "Plugin not found in the registry.");

        std::cout << "Building component: " << plugin_info.plugin_name << std::endl;
        auto& context = contexts[i];
        context.initialize();
        RPP_LOG_DEBUG(*logger, "Context initialized plugin=%s connection=%s.",
                     plugin_names[i].c_str(), conn.c_str());
        auto server_adapter = rpp::load_plugin_adapter_server(
            plugin_info, context.get_instance<rpp::Plugin>(),
            conn + "_server", conn, "create_plugin_server", logger);
        std::cout << "Starting server adapter for plugin: " << plugin_info.plugin_name << std::endl;

        server_host.add_server(std::move(server_adapter));
        RPP_LOG_DEBUG(*logger, "Adapter registered plugin=%s connection=%s.",
                     plugin_names[i].c_str(), conn.c_str());
    }
    RPP_LOG_DEBUG(*logger, "Entering server host host=%s port=%d.", host.c_str(), port);
    server_host.run();
    active_server_host = nullptr;
}
