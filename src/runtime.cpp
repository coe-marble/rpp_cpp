#include "rpp_cpp/runtime.hpp"

#include <utility>

namespace rpp
{

template<bool Concurrent, bool IsScript>
BasicRuntime<Concurrent, IsScript>::BasicRuntime(Storage storage)
: storage_(std::move(storage))
{
  if constexpr (IsScript) {
    if (!storage_ || storage_->instance_) {throw LifecycleError("Script storage requires a slot collection without a root plugin");}
    storage_->context_ = std::shared_ptr<ComponentContext>(new ComponentContext(*storage_));
  }
}

template<bool Concurrent, bool IsScript>
BasicRuntime<Concurrent, IsScript>::~BasicRuntime()
{release_owned_tree();}

template<bool Concurrent, bool IsScript>
void BasicRuntime<Concurrent, IsScript>::release_owned_tree() noexcept
{
  if constexpr (IsScript) {
    if (!storage_) {return;}
    try {(void)close_owned_slots(Timeout::zero());} catch (...) {}
    if (!storage_->resources_released_) {
      // Failed shutdown must retain active entries. Explicit close() permits retry.
      storage_->context_->detach();
      (void)storage_.release();
    }
  } else {
    if (storage_.owns_tree && storage_.slot) {
      try {(void)close_owned_slots(Timeout::zero());} catch (...) {}
    }
  }
}

template<bool Concurrent, bool IsScript>
BasicRuntime<Concurrent, IsScript>::BasicRuntime(Runtime && other) noexcept
: storage_(std::move(other.storage_))
{
  if constexpr (!IsScript) {other.storage_.owns_tree = false;}
}

template<bool Concurrent, bool IsScript>
BasicRuntime<Concurrent, IsScript> & BasicRuntime<Concurrent, IsScript>::operator=(Runtime && other) noexcept
{
  if (this == &other) {return *this;}
  release_owned_tree();
  storage_ = std::move(other.storage_);
  if constexpr (!IsScript) {other.storage_.owns_tree = false;}
  return *this;
}

template<bool Concurrent, bool IsScript>
CallbackResult BasicRuntime<Concurrent, IsScript>::process_deactivations(Timeout timeout) const
{
  if constexpr (IsScript) {
    return SlotManager::process_subcomponent_deactivations(component_data(), timeout);
  } else {
    return storage_.slot->process_deactivations(storage_.name, storage_.instance_id, timeout);
  }
}

template<bool Concurrent, bool IsScript>
bool BasicRuntime<Concurrent, IsScript>::allows_dynamic_components() const
{
  if constexpr (IsScript) {return false;}
  else {return storage_.slot && storage_.slot->allows_dynamic_components();}
}
template<bool Concurrent, bool IsScript>
bool BasicRuntime<Concurrent, IsScript>::allows_dynamic_components(const std::string & slot_name) const
{
  return slot(slot_name).allows_dynamic_components();
}

template<bool Concurrent, bool IsScript>
std::vector<ComponentStatus> BasicRuntime<Concurrent, IsScript>::get_subcomponent_statuses(const std::string & slot_name) const
{return slot(slot_name).list();}

template<bool Concurrent, bool IsScript>
CallbackResult BasicRuntime<Concurrent, IsScript>::load_component(
  const std::string & slot_name, const std::string & name,
  const std::string & component) const
{return slot(slot_name).load(name, component);}

template<bool Concurrent, bool IsScript>
CallbackResult BasicRuntime<Concurrent, IsScript>::configure_component(
  const std::string & slot_name,
  const std::string & name) const
{return slot(slot_name).configure(name);}

template<bool Concurrent, bool IsScript>
CallbackResult BasicRuntime<Concurrent, IsScript>::activate_component(
  const std::string & slot_name, const std::string & name,
  const std::any & state) const
{return slot(slot_name).activate(name, state);}

template<bool Concurrent, bool IsScript>
DeactivationResult<std::any> BasicRuntime<Concurrent, IsScript>::deactivate_component(
  const std::string & slot_name,
  const std::string & name, Timeout timeout) const
{return slot(slot_name).deactivate(name, timeout);}

template<bool Concurrent, bool IsScript>
CallbackResult BasicRuntime<Concurrent, IsScript>::cleanup_component(
  const std::string & slot_name,
  const std::string & name) const
{return slot(slot_name).cleanup(name);}

template<bool Concurrent, bool IsScript>
CallbackResult BasicRuntime<Concurrent, IsScript>::unload_component(
  const std::string & slot_name, const std::string & name,
  Timeout timeout) const
{return slot(slot_name).unload(name, timeout);}

template<bool Concurrent, bool IsScript>
CallbackResult BasicRuntime<Concurrent, IsScript>::switch_component(
  const std::string & slot_name, const std::string & source,
  const std::string & target, bool transfer_state, Timeout timeout) const
{return slot(slot_name).switch_components(source, target, transfer_state, timeout);}

template<bool Concurrent, bool IsScript>
CallbackResult BasicRuntime<Concurrent, IsScript>::reload_component(
  const std::string & slot_name, const std::string & name,
  bool transfer_state, Timeout timeout,
  std::function<void(const std::string &)> refresh) const
{return slot(slot_name).reload(name, transfer_state, timeout, std::move(refresh));}

template<bool Concurrent, bool IsScript>
CallbackResult BasicRuntime<Concurrent, IsScript>::set_registration_validator(
  const std::string & slot_name,
  RegistrationValidator validator)
{return slot(slot_name).set_registration_validator(std::move(validator));}

template<bool Concurrent, bool IsScript>
typename BasicRuntime<Concurrent, IsScript>::SlotManager & BasicRuntime<Concurrent, IsScript>::slot(
  const std::string & name) const
{return component_data().subcomponent_slot(name);}

template<bool Concurrent, bool IsScript>
CallbackResult BasicRuntime<Concurrent, IsScript>::configure() const
{
  if constexpr (IsScript) {return SlotManager::configure_subcomponents(component_data());}
  else {return storage_.slot->configure(storage_.name, storage_.instance_id);}
}

template<bool Concurrent, bool IsScript>
CallbackResult BasicRuntime<Concurrent, IsScript>::activate(const std::any & state) const
{
  if constexpr (IsScript) {
    (void)component_data();
    return state.has_value() ? CallbackResult::failure("A script has no plugin handover state") : CallbackResult{};
  } else {
    return storage_.slot->activate(storage_.name, state, storage_.instance_id);
  }
}

template<bool Concurrent, bool IsScript>
DeactivationResult<std::any> BasicRuntime<Concurrent, IsScript>::deactivate(Timeout timeout) const
{
  if constexpr (IsScript) {(void)component_data(); return {};}
  else {return storage_.slot->deactivate(storage_.name, timeout, storage_.instance_id);}
}

template<bool Concurrent, bool IsScript>
CallbackResult BasicRuntime<Concurrent, IsScript>::cleanup() const
{
  if constexpr (IsScript) {return SlotManager::cleanup_subcomponents(component_data());}
  else {return storage_.slot->cleanup(storage_.name, storage_.instance_id);}
}

template<bool Concurrent, bool IsScript>
CallbackResult BasicRuntime<Concurrent, IsScript>::close()
{
  if constexpr (IsScript) {return close_owned_slots(Timeout::max());}
  else {
    if (storage_.owns_tree) {return close_owned_slots(Timeout::max());}
    return storage_.slot->unload(storage_.name, Timeout::max(), storage_.instance_id);
  }
}

template<bool Concurrent, bool IsScript>
const ComponentContext & BasicRuntime<Concurrent, IsScript>::get_context() const
{return *component_data().context_;}

template<bool Concurrent, bool IsScript>
typename BasicRuntime<Concurrent, IsScript>::ComponentData & BasicRuntime<Concurrent, IsScript>::component_data() const
{
  if constexpr (IsScript) {
    if (!storage_ || storage_->resources_released_) {throw LifecycleError("The script slots have been released");}
    return *storage_;
  } else {
    if (!storage_.slot) {throw LifecycleError("The runtime handle has been moved from");}
    return storage_.slot->component_data(storage_.name, storage_.instance_id);
  }
}

template<bool Concurrent, bool IsScript>
CallbackResult BasicRuntime<Concurrent, IsScript>::close_owned_slots(Timeout timeout)
{
  if constexpr (IsScript) {
    if (!storage_ || storage_->resources_released_) {return {};}
    auto stopped = SlotManager::deactivate_descendants_for_shutdown(*storage_, timeout);
    if (!stopped.success) {return stopped;}
    CallbackResult first_error;
    for (auto slot = storage_->subcomponent_slots_.rbegin();
      slot != storage_->subcomponent_slots_.rend(); ++slot)
    {
      auto result = slot->second->close(timeout);
      if (!result.success && first_error.success) {first_error = result;}
    }
    if (!first_error.success) {return first_error;}
    storage_->subcomponent_slots_.clear();
    storage_->context_->detach();
    storage_->resources_released_ = true;
    return {};
  } else {
    return storage_.slot->close(timeout);
  }
}

template class BasicRuntime<false, false>;
template class BasicRuntime<false, true>;
template class BasicRuntime<true, false>;
template class BasicRuntime<true, true>;
}  // namespace rpp
