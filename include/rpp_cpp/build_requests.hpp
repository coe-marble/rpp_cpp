#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace rpp
{

class RppLogger;

namespace detail
{

/// Internal resolved construction input for one component registration.
struct ComponentBuildRequest
{
  std::string name;
  std::string path;
  std::string parent_path;
  std::string plugin_name;
  std::string slot_declaration;
};

}  // namespace detail

/// Public input for one top-level component.
struct ComponentRequest
{
  std::string path;
  std::string plugin_name;
};

struct ScriptPath {std::string path;};
struct LibraryScript {std::string library_name; std::string script_name;};
struct ScriptDescriptionPath {std::string path;};

/// Public input for script construction.
struct ScriptRequest
{
  std::variant<ScriptPath, LibraryScript, ScriptDescriptionPath> source;
  std::optional<std::string> configuration;
  std::optional<std::string> parts_folder;
};

/// One connection-backed server slot and its initial top-level components.
struct ComponentServerSlotRequest
{
  std::string connection_name;
  std::string declaration;
  std::vector<ComponentRequest> components;
};

/// Public input for component-server construction.
struct ComponentServerRequest
{
  std::string host = "localhost";
  uint16_t port = 8080;
  std::vector<ComponentServerSlotRequest> slots;
  std::shared_ptr<RppLogger> logger;
};

}  // namespace rpp
