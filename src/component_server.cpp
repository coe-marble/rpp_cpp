#include <cstdint>
#include <cstdlib>
#include <memory>
#include <map>
#include <utility>
#include <vector>
#include "rpp_cpp/data_manager.hpp"
#include "rpp_cpp/component_runtime_builder.hpp"
#include "rpp_cpp/rpp_server_host.hpp"
#include "rpp_cpp/logger.hpp"
#include <iostream>
#include <string>
#include <csignal>

namespace
{
rpp::RppServerHost * active_server_host = nullptr;

void handle_interrupt(int)
{
  if (active_server_host != nullptr) {
    active_server_host->shutdown();
  }
}
}  // namespace

int main(int argc, char ** argv)
{
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
  std::vector<std::string> slot_declarations;
  std::vector<std::string> plugin_names;
  bool allow_dynamic_components = false;

  // get user home directory
  const char * home_env = std::getenv("HOME");
  if (home_env != nullptr) {
    home_dir = std::string(home_env) + "/.rpp";
  }

  const char * rpp_home_env = std::getenv("RPP_HOME");
  if (rpp_home_env != nullptr) {
    home_dir = std::string(rpp_home_env);
  }


  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--allow-dynamic-components") {allow_dynamic_components = true;}
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
    if (arg == "--slot" && i + 1 < argc) {
      slot_declarations.push_back(argv[++i]);
    }
  }

  if (paths.empty() || conns.empty() || plugin_names.empty() || slot_declarations.empty()) {
    std::cerr <<
      "Error: Missing required arguments. Please provide --path, --plugin, --conn, and --slot." <<
      std::endl;
    return 1;
  }

  if (paths.size() != conns.size() || paths.size() != plugin_names.size() ||
    paths.size() != slot_declarations.size())
  {
    std::cerr << "Error: The number of --path, --plugin, --conn, and --slot arguments must be the same." <<
      std::endl;
    return 1;
  }

  RPP_LOG_DEBUG(
    *logger, "Parsed host=%s port=%d home=%s components=%zu.",
    host.c_str(), port, rpp::RPP_HOME.c_str(), paths.size());

  rpp::RppDataManager data_manager(rpp::RPP_HOME);
  std::signal(SIGINT, handle_interrupt);
  std::signal(SIGTERM, handle_interrupt);
  rpp::ComponentRuntimeBuilder runtime_builder(data_manager, rpp::ClockOptions{},
    allow_dynamic_components);

  std::cout << "Starting component server on " << host << ":" << port << std::endl;

  rpp::ComponentServerRequest request;
  request.host = host;
  request.port = port;
  request.logger = logger;
  std::map<std::string, size_t> slot_indices;
  for (size_t i = 0; i < paths.size(); ++i) {
    auto [slot, inserted] = slot_indices.try_emplace(conns[i], request.slots.size());
    if (inserted) {
      request.slots.push_back({conns[i], slot_declarations[i], {}});
    }
    auto & slot_request = request.slots[slot->second];
    if (slot_request.declaration != slot_declarations[i]) {
      std::cerr << "Error: A connection must use one slot declaration: " << conns[i] << std::endl;
      return 1;
    }
    slot_request.components.push_back({paths[i], plugin_names[i]});
  }

  std::cout << "Building host-owned component slots." << std::endl;
  try {
    auto server_host = runtime_builder.build_component_server(request);
    active_server_host = server_host.get();
    RPP_LOG_DEBUG(*logger, "Entering server host host=%s port=%d.", host.c_str(), port);
    server_host->run();
    server_host->close().require_success("on_cleanup");
  } catch (...) {
    active_server_host = nullptr;
    throw;
  }
  active_server_host = nullptr;
}
