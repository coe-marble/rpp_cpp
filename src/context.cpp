#include "rpp_cpp/context.hpp"

namespace rpp
{

const std::string & ComponentContext::get_instance_id() const
{
  return visit([](auto & component) -> const std::string & {return component.instance_id_;});
}

std::shared_ptr<const RppLogger> ComponentContext::get_logger() const
{return visit([](auto & component) {return component.logger_;});}

std::shared_ptr<const RppClock> ComponentContext::get_clock() const
{return visit([](auto & component) {return component.clock_;});}

std::vector<std::string> ComponentContext::list_subcomponents() const
{
  return visit([](auto & component) {
             std::vector<std::string> names;
             for (const auto & [name, manager] : component.subcomponent_slots_) {
               (void)manager;
               names.push_back(name);
             }
             return names;
  });
}

bool ComponentContext::is_native_subcomponent(
  const std::string & slot_name, std::optional<std::string> component_name) const
{
  return visit([&](auto & component) {
             return component.subcomponent_slot(slot_name).subcomponent_context(
        component_name)->is_native();
  });
}

const SubcomponentContext & ComponentContext::get_subcomponent_context(
  const std::string & slot_name, std::optional<std::string> component_name) const
{
  return visit([&](auto & component) -> const SubcomponentContext & {
             return *component.subcomponent_slot(slot_name).subcomponent_context(component_name);
  });
}

SubcomponentContextRange ComponentContext::enumerate_subcomponent_contexts(
  const std::string & slot_name) const
{
  return visit([&](auto & component) {
             return SubcomponentContextRange(
      component.subcomponent_slot(slot_name).subcomponent_contexts());
  });
}

bool ComponentContext::is_active() const
{
  return visit([](auto & component) {
             return component.owning_slot_ &&
                    component.owning_slot_->get_state(component.registration_name_) ==
                    LifecycleState::ACTIVE;
  });
}

CallbackResult ComponentContext::request_deactivation() const
{
  return visit([](auto & component) {
             if (!component.owning_slot_) {
               return CallbackResult::failure("A script has no component to deactivate");
             }
             using SlotManager = typename std::decay_t<decltype(component)>::SlotManager;
             return SlotManager::request_deactivation(component);
  });
}

CallbackResult SubcomponentContext::activate(HandoverState state) const
{
  return context_->visit([&](auto & component) {
             return component.owning_slot_->activate(
      component.registration_name_, std::move(state), component.instance_id_);
  });
}

DeactivationResult<HandoverState> SubcomponentContext::deactivate(Timeout timeout) const
{
  return context_->visit([&](auto & component) {
             return component.owning_slot_->deactivate(component.registration_name_, timeout,
        component.instance_id_);
  });
}

CallbackResult ComponentContext::activate_subcomponents() const
{
  return visit([](auto & component) {
             using SlotManager = typename std::decay_t<decltype(component)>::SlotManager;
             return SlotManager::activate_subcomponents(component);
  });
}

CallbackResult ComponentContext::deactivate_subcomponents(Timeout timeout) const
{
  return visit([&](auto & component) {
             using SlotManager = typename std::decay_t<decltype(component)>::SlotManager;
             return SlotManager::deactivate_subcomponents(component, timeout);
  });
}
}  // namespace rpp
