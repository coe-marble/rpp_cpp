#pragma once

#include <map>
#include <memory>
#include <string>

#include "runtime_fwd.hpp"
#include "threading.hpp"
#include "parameters.hpp"
#include "plugin_def.hpp"
#include "logger.hpp"
#include "clock.hpp"

namespace rpp
{
class ExternalComponentProcess;
namespace detail
{
// Owned by a slot entry, or directly by a script runtime for its services and slot collection.
template<class Threading>
struct ComponentData
{
  using SlotManager = BasicSlotManager<Threading>;
  using SlotManagers = std::map<std::string, std::shared_ptr<BasicSlotManager<Threading>>>;
  ComponentData(std::shared_ptr<Plugin> instance, params::Parameters parameters,
    std::shared_ptr<ExternalComponentProcess> process, SlotManagers slots,
    const ClockOptions & clock_options, bool is_native);
  static std::string create_instance_id();
  SlotManager & subcomponent_slot(const std::string & name) const
  {
    auto found = subcomponent_slots_.find(name);
    if (found == subcomponent_slots_.end()) {throw LifecycleError("Component slot '" + name + "' not found");}
    return *found->second;
  }

  // Set only for real component entries. Script slot collections have no owning slot.
  SlotManager * owning_slot_ = nullptr;
  std::string registration_name_;
  params::Parameters parameters_;
  SlotManagers subcomponent_slots_;
  std::string component_name_;
  std::string component_reference_;
  std::string plugin_type_;
  std::string instance_id_;
  bool is_native_ = false;
  std::shared_ptr<ExternalComponentProcess> external_component_process_;
  std::shared_ptr<ComponentContext> context_;
  std::shared_ptr<Plugin> instance_;
  std::shared_ptr<RppLogger> logger_;
  std::shared_ptr<RppClock> clock_;
  typename Threading::Flag resources_released_{false};
  typename Threading::Flag deactivation_pending_{false};
  bool activation_attempted_ = false;
};
}  // namespace detail
}  // namespace rpp
