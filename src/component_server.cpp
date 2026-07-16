#include "rpp_cpp/plugin_loader.hpp"
#include "rpp_cpp/library_manager.hpp"
#include "rpp_cpp/rpp_server_host.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <string>

int main(int argc, char **argv) {

    // parse command line arguments,
    std::string component_dir = "";
    std::string plugin_name = "";
    std::string home_dir = "";
    std::string host = "localhost";
    std::string name;
    uint16_t plugin_port = 8080;
    uint16_t runtime_port = 8081;

    // get user home directory
    const char* home_env = std::getenv("HOME");
    if (home_env != nullptr) {
        home_dir = std::string(home_env) + "/.rpp";
    }

    const char* rpp_home_env = std::getenv("RPP_HOME");
    if (rpp_home_env != nullptr) {
        home_dir = std::string(rpp_home_env);
    }


    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [--help|-h]" << std::endl;
            return 0;
        }

        if (arg == "--component-path" && i + 1 < argc) {
            std::string path = argv[++i];
            component_dir = path;
        }

        if (arg == "--home" && i + 1 < argc) {
            std::string path = argv[++i];
            rpp::RPP_HOME = path;
        }

        if (arg == "--plugin" && i + 1 < argc) {
            plugin_name = argv[++i];
        }
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        }
        if (arg == "--plugin-port" && i + 1 < argc) {
            plugin_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
        if (arg == "--runtime-port" && i + 1 < argc) {
            runtime_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
        if (arg == "--name" && i + 1 < argc) {
            name = argv[++i];
        }
    }

    rpp::LibraryManager library_manager(rpp::RPP_HOME);

    rpp::PluginInfo plugin_info = library_manager.get_plugin_info(plugin_name);

    assert(!plugin_info.plugin_name.empty() && "Plugin not found in the registry.");

    auto instance = rpp::load_cpp_plugin_from_shared_library(plugin_info);
    auto server_adapter = rpp::load_plugin_adapter_server(plugin_info, instance, host, plugin_port, name);

    rpp::RppServerHost server_host(host, runtime_port);
    server_host.add_server(server_adapter);
    server_host.run();


}