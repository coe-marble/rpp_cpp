#include "rpp_cpp/context.hpp"

#include <utility>
#include "rpp_cpp/external_component_process.hpp"

namespace rpp
{
ComponentContext::~ComponentContext() = default;
ComponentContext::ComponentContext(ComponentContext && other) noexcept = default;
ComponentContext & ComponentContext::operator=(ComponentContext && other) noexcept = default;

ComponentContext::ComponentContext(
  PluginPtrShared instance,
  params::Parameters && parameters,
  std::shared_ptr<ExternalComponentProcess> external_component_process,
  SubcomponentSlots && subcomponent_slots,
  const ClockOptions & clock_options)
: parameters_(std::move(parameters)),
  subcomponent_slots_(std::move(subcomponent_slots)),
  external_component_process_(std::move(external_component_process)),
  instance_(std::move(instance)),
  logger_(std::make_shared<rpp::RppLogger>()),
  clock_(ClockFactory::create_clock(clock_options))
{}

ComponentContext::ComponentContext(
  ScriptComponentSlots && script_components, const ClockOptions & clock_options)
: ComponentContext(nullptr, params::Parameters{}, nullptr, std::move(
      script_components), clock_options)
{}

std::shared_ptr<const rpp::RppLogger> ComponentContext::get_logger() const
{
  return logger_;
}

std::shared_ptr<const rpp::RppClock> ComponentContext::get_clock() const
{
  return clock_;
}

CallbackResult ComponentContext::close_impl()
{
  CallbackResult first_error;
  instance_.reset();
  for (auto item = subcomponent_slots_.rbegin(); item != subcomponent_slots_.rend(); ++item) {
    auto result = item->second->close();
    if (!result.success && first_error.success) {first_error = result;}
  }
  auto released = close_runtime();
  if (!released.success && first_error.success) {first_error = released;}
  if (first_error.success) {subcomponent_slots_.clear();}
  return first_error;
}

CallbackResult ComponentContext::configure_impl() const
{
  configuration_attempted_ = true;
  for (const auto & [name, manager] : subcomponent_slots_) {
    (void)name;
    for (const auto & subcomponent : manager->contexts()) {
      auto result = subcomponent->configure();
      if (!result.success) {return result;}
    }
  }
  if (external_component_process_) {return configure_runtime();}
  if (instance_) {
    return instance_->configure(*this);
  }
  return {};
}

CallbackResult ComponentContext::cleanup_impl() const
{
  if (has_active_work()) {
    return CallbackResult::failure("Deactivate the context before cleanup");
  }
  configuration_attempted_ = true;
  CallbackResult first_error;
  auto record_error = [&](const CallbackResult & result) {
      if (!result.success) {
        if (first_error.success) {
          first_error = result;
        } else {
          RPP_LOG_ERROR(
            *logger_, "Additional component cleanup failure: %s",
            result.message.c_str());
        }
      }
    };
  if (external_component_process_) {
    record_error(cleanup_runtime());
  } else if (instance_) {
    record_error(instance_->cleanup());
  }
  for (auto item = subcomponent_slots_.rbegin(); item != subcomponent_slots_.rend(); ++item) {
    auto children = item->second->contexts();
    for (auto child = children.rbegin(); child != children.rend(); ++child) {
      record_error((*child)->cleanup());
    }
  }
  if (first_error.success) {configuration_attempted_ = false;}
  return first_error;
}

CallbackResult ComponentContext::activate_impl(const std::any & state) const
{
  if (is_active_) {return {};}
  if (external_component_process_) {
    return CallbackResult::failure("Adapted lifecycle activation is not supported yet");
  }
  if (!instance_) {
    return state.has_value() ?
           CallbackResult::failure("A context without a plugin accepts no state") : CallbackResult{};
  }
  auto validation = instance_->validate_handover_state(state);
  if (!validation.success) {return validation;}
  activation_attempted_ = true;
  auto result = instance_->activate_state(state);
  if (result.success) {is_active_ = true;}
  return result;
}

DeactivationResult<std::any> ComponentContext::deactivate_impl() const
{
  if (external_component_process_) {
    return {CallbackResult::failure("Adapted lifecycle deactivation is not supported yet"), {}};
  }
  if (!instance_ || !activation_attempted_) {return {};}
  auto result = instance_->deactivate_state();
  if (result.result.success) {
    activation_attempted_ = false;
    is_active_ = false;
  } else {
    result.state.reset();
  }
  return result;
}

CallbackResult ComponentContext::activate_components() const
{
  if (contains_guarded_call()) {
    return CallbackResult::failure("Cannot manage components from inside a guarded component call");
  }
  std::vector<std::shared_ptr<ComponentContext>> activated;
  CallbackResult result;
  for (const auto & [name, manager] : subcomponent_slots_) {
    (void)name;
    result = manager->activate_selected(activated);
    if (!result.success) {break;}
  }
  if (!result.success) {
    for (auto child = activated.rbegin(); child != activated.rend(); ++child) {
      auto rollback = (*child)->deactivate();
      if (!rollback.result.success) {
        RPP_LOG_ERROR(
          *logger_, "Component activation rollback failed: %s",
          rollback.result.message.c_str());
      }
    }
  }
  return result;
}

CallbackResult ComponentContext::deactivate_components(Timeout timeout) const
{
  if (contains_guarded_call()) {
    return CallbackResult::failure("Cannot manage components from inside a guarded component call");
  }
  CallbackResult first_error;
  for (auto slot = subcomponent_slots_.rbegin(); slot != subcomponent_slots_.rend(); ++slot) {
    auto children = slot->second->deactivation_contexts();
    for (auto child = children.rbegin(); child != children.rend(); ++child) {
      auto result = slot->second->deactivate((*child)->owner_name_, timeout, child->get()).result;
      if (!result.success) {
        if (first_error.success) {first_error = result;} else {
          RPP_LOG_ERROR(
            *logger_, "Additional component deactivation failure: %s",
            result.message.c_str());
        }
      }
    }
  }
  return first_error;
}

CallbackResult ComponentContext::deactivate_components_for_shutdown(Timeout timeout) const
{
  auto first_error = deactivate_components(timeout);
  // An inactive child may still own active descendants when its callbacks left them running.
  for (auto slot = subcomponent_slots_.rbegin(); slot != subcomponent_slots_.rend(); ++slot) {
    auto children = slot->second->contexts();
    for (auto child = children.rbegin(); child != children.rend(); ++child) {
      auto result = (*child)->deactivate_components_for_shutdown(timeout);
      if (!result.success) {
        if (first_error.success) {first_error = result;} else {
          RPP_LOG_ERROR(
            *logger_, "Additional subtree deactivation failure: %s",
            result.message.c_str());
        }
      }
    }
  }
  return first_error;
}

std::vector<std::string> ComponentContext::list_subcomponents() const
{
  std::vector<std::string> names;
  for (const auto & [name, _] : subcomponent_slots_) {
    names.push_back(name);
  }
  return names;
}

const ComponentContext & ComponentContext::get_subcomponent_context(const std::string & name) const
{
  auto selected = slot(name).selected_context();
  if (!selected) {
    throw std::runtime_error(
            "Subcomponent slot '" + name + "' is empty.");
  }
  return *selected;
}

std::vector<std::shared_ptr<const ComponentContext>> ComponentContext::get_subcomponent_contexts(
  const std::string & name) const
{
  auto children = slot(name).contexts();
  return {children.begin(), children.end()};
}

const ComponentContext & ComponentContext::get_subcomponent_context(
  const std::string & slot_name,
  const std::string & component_name) const
{
  auto found = slot(slot_name).context(component_name);
  if (!found) {throw LifecycleError("Component is not loaded: " + component_name);}
  return *found;
}

bool ComponentContext::allows_dynamic_components() const
{
  auto manager = owning_manager();
  return manager && manager->allows_dynamic_components();
}

bool ComponentContext::allows_dynamic_components(const std::string & slot_name) const
{
  return slot(slot_name).allows_dynamic_components();
}

std::vector<ComponentInfo> ComponentContext::list_components(const std::string & slot_name) const
{return slot(slot_name).list();}

CallbackResult ComponentContext::load_component(
  const std::string & slot_name, const std::string & name,
  const std::string & component) const
{return slot(slot_name).load(name, component);}

CallbackResult ComponentContext::configure_component(
  const std::string & slot_name,
  const std::string & name) const
{return slot(slot_name).configure(name);}

CallbackResult ComponentContext::activate_component(
  const std::string & slot_name, const std::string & name,
  const std::any & state) const
{return slot(slot_name).activate(name, state);}

DeactivationResult<std::any> ComponentContext::deactivate_component(
  const std::string & slot_name,
  const std::string & name, Timeout timeout) const
{return slot(slot_name).deactivate(name, timeout);}

CallbackResult ComponentContext::cleanup_component(
  const std::string & slot_name,
  const std::string & name) const
{return slot(slot_name).cleanup(name);}

CallbackResult ComponentContext::unload_component(
  const std::string & slot_name, const std::string & name,
  Timeout timeout) const
{return slot(slot_name).unload(name, timeout);}

CallbackResult ComponentContext::switch_component(
  const std::string & slot_name, const std::string & source,
  const std::string & target, bool transfer_state, Timeout timeout) const
{return slot(slot_name).switch_components(source, target, transfer_state, timeout);}

CallbackResult ComponentContext::reload_component(
  const std::string & slot_name, const std::string & name,
  bool transfer_state, Timeout timeout,
  std::function<void(const std::string &)> refresh) const
{return slot(slot_name).reload(name, transfer_state, timeout, std::move(refresh));}

CallbackResult ComponentContext::set_registration_validator(
  const std::string & slot_name,
  RegistrationValidator validator)
{return slot(slot_name).set_registration_validator(std::move(validator));}

ComponentManager & ComponentContext::slot(const std::string & name) const
{
  auto found = subcomponent_slots_.find(name);
  if (found == subcomponent_slots_.end()) {
    throw LifecycleError("Component slot '" + name + "' not found");
  }
  return *found->second;
}

CallbackResult ComponentContext::configure() const
{
  if (contains_guarded_call()) {
    return CallbackResult::failure(
      "Cannot manage a context from inside its guarded component call");
  }
  auto manager = owning_manager();
  return manager ? manager->configure(owner_name_, this) : configure_impl();
}

CallbackResult ComponentContext::activate(const std::any & state) const
{
  if (contains_guarded_call()) {
    return CallbackResult::failure(
      "Cannot manage a context from inside its guarded component call");
  }
  auto manager = owning_manager();
  return manager ? manager->activate(owner_name_, state, this) : activate_impl(state);
}

DeactivationResult<std::any> ComponentContext::deactivate() const
{
  if (contains_guarded_call()) {
    return {CallbackResult::failure(
        "Cannot manage a context from inside its guarded component call"), {}};
  }
  auto manager = owning_manager();
  return manager ? manager->deactivate(owner_name_, Timeout::max(), this) : deactivate_impl();
}

CallbackResult ComponentContext::cleanup() const
{
  if (contains_guarded_call()) {
    return CallbackResult::failure(
      "Cannot manage a context from inside its guarded component call");
  }
  auto manager = owning_manager();
  return manager ? manager->cleanup(owner_name_, this) : cleanup_impl();
}

CallbackResult ComponentContext::close()
{
  if (contains_guarded_call()) {
    return CallbackResult::failure(
      "Cannot manage a context from inside its guarded component call");
  }
  auto manager = owning_manager();
  if (manager) {return manager->unload(owner_name_, Timeout::max(), this);}
  if (has_active_work()) {
    auto result = deactivate_impl().result;
    if (!result.success) {return result;}
  }
  auto stopped_children = deactivate_components_for_shutdown(Timeout::max());
  if (!stopped_children.success) {return stopped_children;}
  if (configuration_attempted_) {
    auto result = cleanup_impl();
    if (!result.success) {return result;}
  }
  return close_impl();
}

std::shared_ptr<ComponentManager> ComponentContext::owning_manager() const
{
  auto manager = owner_.lock();
  if (!manager && !owner_name_.empty()) {
    throw LifecycleError("The component's owning slot has been released");
  }
  return manager;
}

bool ComponentContext::contains_guarded_call() const
{
  for (const auto & [name, manager] : subcomponent_slots_) {
    (void)name;
    if (manager->has_guarded_call_on_current_thread()) {return true;}
    for (const auto & child : manager->contexts()) {
      if (child->contains_guarded_call()) {return true;}
    }
  }
  return false;
}

bool ComponentContext::has_active_work() const
{
  if (activation_attempted_) {return true;}
  for (const auto & [name, manager] : subcomponent_slots_) {
    (void)name;
    if (!manager->deactivation_contexts().empty()) {return true;}
    for (const auto & child : manager->contexts()) {
      if (child->has_active_work()) {return true;}
    }
  }
  return false;
}

CallbackResult ComponentContext::configure_runtime() const
{
  auto * adapter = dynamic_cast<ClientAdapter *>(instance_.get());
  if (!adapter) {return CallbackResult::failure("Remote component has no client adapter");}
  try {
    return external_component_process_->configure_component(
      adapter->get_info_adapter_client__().connection_name);
  } catch (const kj::Exception & error) {
    return CallbackResult::failure(error.getDescription().cStr());
  } catch (const std::exception & error) {
    return CallbackResult::failure(error.what());
  }
}

CallbackResult ComponentContext::cleanup_runtime() const
{
  auto * adapter = dynamic_cast<ClientAdapter *>(instance_.get());
  if (!adapter) {return CallbackResult::failure("Remote component has no client adapter");}
  try {
    return external_component_process_->cleanup_component(
      adapter->get_info_adapter_client__().connection_name);
  } catch (const kj::Exception & error) {
    return CallbackResult::failure(error.getDescription().cStr());
  } catch (const std::exception & error) {
    return CallbackResult::failure(error.what());
  }
}

CallbackResult ComponentContext::close_runtime()
{
  if (!external_component_process_) {return {};}
  if (external_component_process_.use_count() == 1) {
    auto result = external_component_process_->close();
    if (!result.success) {return result;}
  }
  external_component_process_.reset();
  return {};
}
}  // namespace rpp
