#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace rpp
{

enum class ActivationPolicy {EXCLUSIVE, MULTIPLE};
enum class SlotKind {SINGLE, OPTION, LIST};

struct ComponentSlot
{
  std::string plugin_type;
  SlotKind kind;

  bool accepts_component_list() const {return kind != SlotKind::SINGLE;}

  ActivationPolicy activation_policy() const
  {
    return kind == SlotKind::LIST ? ActivationPolicy::MULTIPLE : ActivationPolicy::EXCLUSIVE;
  }
};

inline ComponentSlot parse_component_slot_type(std::string_view declaration)
{
  const auto trim = [](std::string_view value) -> std::string_view {
      const auto first = value.find_first_not_of(" \t\r\n\f\v");
      if (first == std::string_view::npos) {return {};}
      const auto last = value.find_last_not_of(" \t\r\n\f\v");
      return value.substr(first, last - first + 1);
    };
  auto plugin_type = trim(declaration);
  auto kind = SlotKind::SINGLE;
  if (plugin_type.size() >= 8 && plugin_type.substr(0, 7) == "Option[" &&
    plugin_type.back() == ']')
  {
    kind = SlotKind::OPTION;
    plugin_type = trim(plugin_type.substr(7, plugin_type.size() - 8));
  } else if (plugin_type.size() >= 6 && plugin_type.substr(0, 5) == "List[" &&
    plugin_type.back() == ']')
  {
    kind = SlotKind::LIST;
    plugin_type = trim(plugin_type.substr(5, plugin_type.size() - 6));
  }
  if (plugin_type.empty() || plugin_type.find_first_of("[]") != std::string_view::npos) {
    throw std::invalid_argument(
            "Invalid component slot type '" + std::string(declaration) +
            "'; expected T, Option[T], or List[T]");
  }
  return {std::string(plugin_type), kind};
}

}  // namespace rpp
