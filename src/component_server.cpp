#include "rpp_cpp/plugin_loader.hpp"
#include "rpp_cpp/data_manager.hpp"
#include "rpp_cpp/rpp_server_host.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <string>

int main(int argc, char **argv) {

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

    rpp::RppDataManager data_manager(rpp::RPP_HOME);
    rpp::RppServerHost server_host(host, port);
    for (size_t i = 0; i < paths.size(); ++i) {
        std::string path = paths[i];
        std::string conn = conns[i];
        std::string plugin_name = plugin_names[i];

        rpp::PluginInfo plugin_info = data_manager.get_plugin_info_from_lib(plugin_name);

        assert(!plugin_info.plugin_name.empty() && "Plugin not found in the registry.");

        std::cout << "Loading plugin: " << plugin_info.plugin_name << std::endl;
        auto instance = rpp::load_cpp_plugin_from_shared_library(plugin_info);
        std::cout << "Loading plugin: " << plugin_info.plugin_name << " done." << std::endl;
        auto server_adapter = rpp::load_plugin_adapter_server(
            plugin_info, std::move(instance), conn + "_server", conn);
        std::cout << "Starting server adapter for plugin: " << plugin_info.plugin_name << std::endl;

        server_host.add_server(std::move(server_adapter));
    }
    server_host.run();
}