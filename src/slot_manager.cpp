#include "rpp_cpp/slot_manager.hpp"

#include <algorithm>
#include <mutex>

#include "rpp_cpp/runtime.hpp"
#include "rpp_cpp/context.hpp"
#include "rpp_cpp/external_component_process.hpp"

namespace rpp
{
using detail::ComponentBuildRequest;

template<class Threading>
struct BasicSlotManager<Threading>::Entry
{
  explicit Entry(std::string name) {info.name = std::move(name);}
  ComponentStatus info;
  std::unique_ptr<ComponentData> component;
  std::shared_ptr<SubcomponentContext> subcomponent_context;
  bool configured = false;
  bool may_be_active = false;
  std::optional<ComponentBuildRequest> build_request;
};

template<class Threading>
bool BasicSlotManager<Threading>::allows_dynamic_components() const
{
  return allow_dynamic_components_;
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::activate_selected(
  std::vector<std::shared_ptr<ComponentRuntime>> & activated)
{
  if (has_local_call()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  if (registration_validation_in_progress_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  for (const auto & subcomponent : activation_runtimes()) {
    const bool was_active = get_state(subcomponent->storage_.name) == LifecycleState::ACTIVE;
    auto result = activate(subcomponent->storage_.name, {}, subcomponent->storage_.instance_id);
    if (!result.success) {return result;}
    if (!was_active) {activated.push_back(subcomponent);}
  }
  return {};
}

template<class Threading>
std::vector<std::shared_ptr<rpp::ComponentRuntime<Threading::concurrent>>> BasicSlotManager<Threading>::runtimes() const
{
  std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
  std::vector<std::shared_ptr<ComponentRuntime>> result;
  result.reserve(registration_order_.size());
  for (const auto & name : registration_order_) {
    auto item = component_entries_.at(name);
    if (item->component && !item->component->resources_released_) {
      result.push_back(runtime_view(*item));
    }
  }
  return result;
}

template<class Threading>
std::shared_ptr<rpp::ComponentRuntime<Threading::concurrent>> BasicSlotManager<Threading>::runtime(
  const std::string & name) const
{
  std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
  auto item = entry_unlocked(name);
  return item->component && !item->component->resources_released_ ? runtime_view(*item) : nullptr;
}

template<class Threading>
std::vector<std::shared_ptr<Plugin>> BasicSlotManager<Threading>::subcomponent_instances() const
{
  std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
  std::vector<std::shared_ptr<Plugin>> instances;
  for (const auto & name : registration_order_) {
    const auto & item = component_entries_.at(name);
    if (!item->component || item->component->resources_released_) {continue;}
    if (!item->component->instance_) {throw LifecycleError("Subcomponent is not loaded: " + name);}
    instances.push_back(item->component->instance_);
  }
  return instances;
}

template<class Threading>
std::shared_ptr<Plugin> BasicSlotManager<Threading>::subcomponent_instance(
  const std::optional<std::string> & component_name) const
{
  std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
  if (!component_name && activation_policy_ == ActivationPolicy::MULTIPLE) {
    throw LifecycleError("List slots require a component name");
  }
  auto item = component_name ? entry_unlocked(*component_name) : selected_entry_unlocked();
  if (!item || !item->component || item->component->resources_released_ ||
    !item->component->instance_)
  {
    throw LifecycleError("Subcomponent is not loaded");
  }
  return item->component->instance_;
}

template<class Threading>
std::shared_ptr<rpp::ComponentRuntime<Threading::concurrent>> BasicSlotManager<Threading>::
selected_runtime() const
{
  std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
  auto selected = selected_entry_unlocked();
  return selected ? runtime_view(*selected) : nullptr;
}

template<class Threading>
std::shared_ptr<const SubcomponentContext>
BasicSlotManager<Threading>::subcomponent_context(
  const std::optional<std::string> & component_name) const
{
  std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
  if (!component_name && activation_policy_ == ActivationPolicy::MULTIPLE) {
    throw LifecycleError("A list slot requires a component name");
  }
  auto item = component_name ? entry_unlocked(*component_name) : selected_entry_unlocked();
  if (!item) {throw LifecycleError("The slot has no loaded component");}
  if (!item->component || item->component->resources_released_ || !item->subcomponent_context) {
    throw LifecycleError("The selected component is not loaded");
  }
  return item->subcomponent_context;
}

template<class Threading>
std::vector<std::shared_ptr<const SubcomponentContext>>
BasicSlotManager<Threading>::subcomponent_contexts() const
{
  std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
  std::vector<std::shared_ptr<const SubcomponentContext>> views;
  views.reserve(registration_order_.size());
  for (const auto & name : registration_order_) {
    const auto & item = component_entries_.at(name);
    if (item->component && !item->component->resources_released_ && item->subcomponent_context) {
      views.push_back(item->subcomponent_context);
    }
  }
  return views;
}

template<class Threading>
std::vector<std::shared_ptr<rpp::ComponentRuntime<Threading::concurrent>>> BasicSlotManager<Threading>::activation_runtimes() const
{
  if (activation_policy_ != ActivationPolicy::EXCLUSIVE) {return runtimes();}
  auto selected = selected_runtime();
  return selected ? std::vector<std::shared_ptr<ComponentRuntime>>{selected} :
         std::vector<std::shared_ptr<ComponentRuntime>>{};
}

template<class Threading>
std::vector<std::shared_ptr<rpp::ComponentRuntime<Threading::concurrent>>> BasicSlotManager<Threading>::deactivation_runtimes() const
{
  std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
  std::vector<std::shared_ptr<ComponentRuntime>> result;
  result.reserve(registration_order_.size());
  for (const auto & name : registration_order_) {
    const auto & item = component_entries_.at(name);
    if (item->component && !item->component->resources_released_ && item->may_be_active) {
      result.push_back(runtime_view(*item));
    }
  }
  return result;
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::validate(
  const ComponentRuntime & candidate,
  const std::string & name)
{
  const auto & candidate_type = component_data(name, candidate.storage_.instance_id).plugin_type_;
  if (!plugin_type_.empty() && !candidate_type.empty() && candidate_type != plugin_type_) {
    return CallbackResult::failure("Expected component type " + plugin_type_ + ", got " +
        candidate_type);
  }
  auto named = registration_validators_by_name_.find(name);
  if (!registration_validator_ && named == registration_validators_by_name_.end()) {return {};}
  std::vector<std::shared_ptr<const ComponentRuntime>> registered;
  for (const auto & key : registration_order_) {
    const auto & item = component_entries_.at(key);
    if (key != name && item->component && !item->component->resources_released_) {
      registered.push_back(runtime_view(*item));
    }
  }
  registration_validation_in_progress_ = true;
  auto result = capture(
    "registration validator", [&]() {
      if (registration_validator_) {
        auto result = registration_validator_(candidate, registered);
        if (!result.success) {return result;}
      }
      return named == registration_validators_by_name_.end() ? CallbackResult{} :
             named->second(candidate, registered);
    });
  registration_validation_in_progress_ = false;
  return result;
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::set_registration_validator(Validator validator)
{
  if (has_local_call()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  if (registration_validation_in_progress_) {
    return CallbackResult::failure("Cannot replace a running registration validator");
  }
  registration_validator_ = std::move(validator);
  return {};
}

template<class Threading>
std::shared_ptr<typename BasicSlotManager<Threading>::Entry> BasicSlotManager<Threading>::entry(
  const std::string & name) const
{
  std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
  return entry_unlocked(name);
}

template<class Threading>
std::shared_ptr<typename BasicSlotManager<Threading>::Entry>
BasicSlotManager<Threading>::entry_unlocked(const std::string & name) const
{
  auto found = component_entries_.find(name);
  if (found == component_entries_.end()) {throw LifecycleError("Unknown component: " + name);}
  return found->second;
}

template<class Threading>
std::shared_ptr<typename BasicSlotManager<Threading>::Entry>
BasicSlotManager<Threading>::selected_entry_unlocked() const
{
  if (!component_name_.empty()) {
    auto found = component_entries_.find(component_name_);
    if (found != component_entries_.end() && found->second->component &&
      !found->second->component->resources_released_) {return found->second;}
  }
  for (const auto & name : registration_order_) {
    const auto & item = component_entries_.at(name);
    if (item->component && !item->component->resources_released_) {return item;}
  }
  return nullptr;
}

template<class Threading>
ComponentStatus BasicSlotManager<Threading>::get_status(const std::string & name) const
{
  std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
  return entry_unlocked(name)->info;
}

template<class Threading>
std::optional<CallbackResult> BasicSlotManager<Threading>::begin(
  Entry & item, const std::string & operation,
  std::initializer_list<LifecycleState> allowed, bool during_close)
{
  std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
  std::string reason;
  if (item.info.transition) {
    reason = "A lifecycle operation is already in progress";
  } else if (shutdown_requested_ && !during_close) {
    reason = "Slot manager is closing";
  } else if (std::find(allowed.begin(), allowed.end(), item.info.state) == allowed.end()) {
    reason = "Invalid lifecycle state for " + operation;
  }
  if (!reason.empty()) {
    auto failure = CallbackResult::failure(reason);
    if (!item.info.transition) {item.info.last_result = failure;}
    return failure;
  }
  item.info.transition = operation;
  item.info.callable = false;
  return {};
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::finish(
  Entry & item, LifecycleState state,
  CallbackResult result)
{
  std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
  item.info.state = state;
  item.info.last_result = result;
  item.info.transition.reset();
  item.info.callable = state == LifecycleState::ACTIVE && result.success;
  return result;
}

template<class Threading>
template<typename Callback>
CallbackResult BasicSlotManager<Threading>::capture(
  const std::string & operation,
  Callback callback)
{
  try {
    return callback();
  } catch (const std::exception & error) {
    return CallbackResult::failure(operation + " failed: " + error.what());
  } catch (...) {
    return CallbackResult::failure(operation + " failed: unknown exception");
  }
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::drain(Entry & item, Timeout timeout)
{
  if constexpr (Threading::concurrent) {
    std::unique_lock<typename Threading::Mutex> lock(slot_mutex_);
    auto empty = [&] {return item.info.in_flight == 0;};
    if (timeout == Timeout::max()) {
      guarded_calls_drained_.wait(lock, empty);
    } else if (!guarded_calls_drained_.wait_for(lock, timeout, empty)) {
      return CallbackResult::failure("Timed out draining active calls; instance retained");
    }
  } else {
    (void)timeout;
    if (has_local_call()) {
      return CallbackResult::failure("Cannot stop a component inside its active call");
    }
  }
  return {};
}

template<class Threading>
std::shared_ptr<rpp::ComponentRuntime<Threading::concurrent>> BasicSlotManager<Threading>::
runtime_view(const Entry & item) const
{
  auto owner = const_cast<SlotManager *>(this)->shared_from_this();
  return std::shared_ptr<ComponentRuntime>(new ComponentRuntime(
      typename ComponentRuntime::ComponentHandle{
      std::move(owner), item.info.name, item.info.instance_id, false}));
}

template<class Threading>
typename BasicSlotManager<Threading>::ComponentData & BasicSlotManager<Threading>::component_data(
  const std::string & name, std::string_view instance_id) const
{
  std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
  auto * component = entry_unlocked(name)->component.get();
  if (!component || component->resources_released_ || component->instance_id_ != instance_id) {
    throw LifecycleError("The component has been released or replaced");
  }
  return *component;
}

template<class Threading>
std::unique_ptr<detail::ComponentData<Threading>> BasicSlotManager<Threading>::take_component(
  const ComponentRuntime & runtime)
{
  auto item = entry(runtime.storage_.name);
  if (item->info.state != LifecycleState::UNCONFIGURED || item->info.transition ||
    item->info.instance_id != runtime.storage_.instance_id)
  {
    throw LifecycleError("Only an unconfigured component can transfer construction ownership");
  }
  item->component->context_->detach();
  item->component->context_.reset();
  item->component->owning_slot_ = nullptr;
  item->component->registration_name_.clear();
  item->subcomponent_context.reset();
  item->info.state = LifecycleState::UNLOADED;
  item->info.instance_id.clear();
  return std::move(item->component);
}

template<class Threading>
rpp::ComponentRuntime<Threading::concurrent> BasicSlotManager<Threading>::create_root(
  std::unique_ptr<ComponentData> component, std::shared_ptr<SlotManager> manager)
{
  if (!component->instance_) {
    throw LifecycleError("A component slot entry requires a plugin instance");
  }
  if (!manager) {
    manager = std::make_shared<SlotManager>(RuntimeFactory{}, ActivationPolicy::EXCLUSIVE, false);
  }
  const auto id = component->instance_id_;
  const auto name = component->component_name_.empty() ? "root" : component->component_name_;
  const auto path = component->component_reference_;
  manager->initialize({name, path, path, "", {}}, {}, std::move(component)).require_success(
    "register root component");
  manager->entry(name)->subcomponent_context.reset();
  return ComponentRuntime(typename ComponentRuntime::ComponentHandle{std::move(manager), name, id,
             true});
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::initialize(
  const ComponentBuildRequest & request, const PreparedComponents & prepared,
  std::unique_ptr<ComponentData> prepared_component)
{
  return load_impl(request.name, request.path, true, &request, &prepared,
      std::move(prepared_component));
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::load(
  const std::string & name, const std::string & component)
{
  return load_impl(name, component, false);
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::load(
  const std::string & name, const std::string & component,
  const std::string & plugin_name)
{
  ComponentBuildRequest request{name, component, component, plugin_name, {}};
  return load_impl(name, component, false, &request);
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::load_impl(
  const std::string & name, const std::string & component, bool initial,
  const ComponentBuildRequest * request, const PreparedComponents * prepared,
  std::unique_ptr<ComponentData> prepared_component)
{
  if (has_local_call()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  if (!initial && !allow_dynamic_components_) {
    return CallbackResult::failure("Dynamic component modification is disabled");
  }
  if (registration_validation_in_progress_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  if (name.empty()) {return CallbackResult::failure("Component name must not be empty");}
  std::shared_ptr<Entry> item;
  bool new_entry = false;
  {
    std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
    if (shutdown_requested_) {return CallbackResult::failure("Slot manager is closing");}
    auto inserted = component_entries_.emplace(name, nullptr);
    new_entry = inserted.second;
    if (new_entry) {
      inserted.first->second = std::make_shared<Entry>(name);
      registration_order_.push_back(name);
    }
    item = inserted.first->second;
  }
  auto rejected = begin(*item, "load", {LifecycleState::UNLOADED});
  if (rejected) {return *rejected;}
  const auto previous_component = item->info.component;
  const auto previous_request = item->build_request;
  bool admission_rejected = false;
  {
    std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
    if (!component.empty()) {item->info.component = component;}
  }
  if (request) {
    item->build_request = *request;
  } else if (!item->build_request || item->build_request->path != item->info.component) {
    item->build_request = ComponentBuildRequest{
      name, item->info.component, item->info.component, "", {}};
  }
  auto result = capture(
    "load", [&] {
      if (!initial && item->info.component.empty()) {
        return CallbackResult::failure("A component path is required");
      }
      if (!prepared_component && !factory_ && !constructor_) {
        return CallbackResult::failure("No construction configuration is attached to this slot");
      }
      auto candidate = std::move(prepared_component);
      if (!candidate) {
        if (factory_) {
          auto constructed = factory_(item->info.component);
          if (!constructed.storage_.slot) {
            return CallbackResult::failure(
              "A slot factory must return an unconfigured component runtime");
          }
          candidate = constructed.storage_.slot->take_component(constructed);
        } else {
          auto components = constructor_->construct_components({*item->build_request}, prepared);
          candidate = std::move(components.front());
        }
      }
      if (!candidate || !candidate->instance_) {
        return CallbackResult::failure("A slot entry requires a plugin instance");
      }
      candidate->owning_slot_ = this;
      candidate->registration_name_ = name;
      candidate->context_ = std::shared_ptr<ComponentContext>(new ComponentContext(*candidate));
      auto subcomponent_context = std::shared_ptr<SubcomponentContext>(
        new SubcomponentContext(candidate->context_));
      {
        std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
        item->info.instance_id = candidate->instance_id_;
        item->component = std::move(candidate);
        item->subcomponent_context = std::move(subcomponent_context);
        item->may_be_active = false;
      }
      auto view = runtime_view(*item);
      auto admitted = validate(*view, name);
      if (!admitted.success) {
        auto released = release_resources(*item->component);
        if (!released.success) {return released;}
        {
          std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
          item->subcomponent_context.reset();
          item->component.reset();
          item->info.instance_id.clear();
        }
        admission_rejected = true;
        return admitted;
      }
      item->configured = false;
      return CallbackResult{};
    });
  if (admission_rejected) {
    finish(*item, LifecycleState::UNLOADED, result);
    std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
    if (new_entry) {
      component_entries_.erase(name);
      registration_order_.erase(std::remove(registration_order_.begin(), registration_order_.end(),
          name), registration_order_.end());
    } else {
      item->info.component = previous_component;
      item->build_request = previous_request;
    }
    return result;
  }
  if (result.success) {
    std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
    if (component_name_.empty()) {component_name_ = name;}
  }
  return finish(
    *item, result.success ? LifecycleState::UNCONFIGURED : LifecycleState::ERROR,
    result);
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::configure(
  const std::string & name,
  std::optional<std::string_view> expected_instance_id)
{
  if (has_local_call()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  if (registration_validation_in_progress_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  auto item = entry(name);
  if (expected_instance_id && item->info.instance_id != *expected_instance_id) {
    return CallbackResult::failure("Stale component runtime instance");
  }
  auto rejected = begin(*item, "configure", {LifecycleState::UNCONFIGURED});
  if (rejected) {return *rejected;}
  item->configured = true;
  auto result = capture("configure", [&] {return configure_component(*item->component);});
  return finish(*item, result.success ? LifecycleState::INACTIVE : LifecycleState::ERROR, result);
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::activate(
  const std::string & name, HandoverState state,
  std::optional<std::string_view> expected_instance_id)
{
  if (has_local_call()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  if (registration_validation_in_progress_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  if (expected_instance_id && entry(name)->info.instance_id != *expected_instance_id) {
    return CallbackResult::failure("Stale component runtime instance");
  }
  if (!allow_dynamic_components_ && activation_policy_ == ActivationPolicy::EXCLUSIVE &&
    name != component_name_)
  {
    return CallbackResult::failure("Dynamic component selection is disabled");
  }
  if (activation_policy_ == ActivationPolicy::EXCLUSIVE) {
    for (const auto & [key, other] : component_entries_) {
      if (key != name && other->may_be_active) {
        return CallbackResult::failure("An exclusive slot already has an active component");
      }
    }
  }
  auto item = entry(name);
  if (item->info.state == LifecycleState::ACTIVE && !item->info.transition &&
    !shutdown_requested_)
  {
    return {};
  }
  auto rejected = begin(*item, "activate", {LifecycleState::INACTIVE});
  if (rejected) {return *rejected;}
  {
    std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
    item->may_be_active = true;
  }
  auto result = capture(
    "activate", [&] {return activate_component(*item->component, std::move(state));});
  if (!result.success) {
    const bool active_work = requires_deactivation(*item->component);
    std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
    item->may_be_active = active_work;
  }
  if (result.success && activation_policy_ == ActivationPolicy::EXCLUSIVE) {
    std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
    component_name_ = name;
  }
  return finish(*item, result.success ? LifecycleState::ACTIVE : LifecycleState::ERROR, result);
}

template<class Threading>
DeactivationResult<HandoverState> BasicSlotManager<Threading>::stop(Entry & item)
{
  try {
    auto result = deactivate_component(*item.component);
    if (result.result.success) {
      std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
      item.may_be_active = false;
    } else {
      result.state.reset();
    }
    return result;
  } catch (const std::exception & error) {
    return {CallbackResult::failure(std::string("deactivate failed: ") + error.what()), {}};
  } catch (...) {
    return {CallbackResult::failure("deactivate failed: unknown exception"), {}};
  }
}

template<class Threading>
DeactivationResult<HandoverState> BasicSlotManager<Threading>::deactivate(
  const std::string & name, Timeout timeout,
  std::optional<std::string_view> expected_instance_id)
{
  if (has_local_call()) {
    return {CallbackResult::failure("Cannot manage a slot from inside its guarded call"), {}};
  }
  if (registration_validation_in_progress_) {
    return {CallbackResult::failure("Cannot mutate a slot from its registration validator"), {}};
  }
  auto item = entry(name);
  if (expected_instance_id && item->info.instance_id != *expected_instance_id) {
    return {CallbackResult::failure("Stale component runtime instance"), {}};
  }
  if (item->info.state == LifecycleState::INACTIVE && !item->info.transition) {return {};}
  if (item->info.state == LifecycleState::ERROR && !item->may_be_active) {
    return {CallbackResult::failure("Failed configuration requires cleanup before reconfiguration"),
      {}};
  }
  auto rejected = begin(*item, "deactivate", {LifecycleState::ACTIVE, LifecycleState::ERROR});
  if (rejected) {return {*rejected, {}};}
  auto drained = drain(*item, timeout);
  auto result = drained.success ? stop(*item) : DeactivationResult<HandoverState>{drained, {}};
  finish(
    *item, result.result.success ? LifecycleState::INACTIVE : LifecycleState::ERROR,
    result.result);
  return result;
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::cleanup(
  const std::string & name,
  std::optional<std::string_view> expected_instance_id)
{
  if (has_local_call()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  if (registration_validation_in_progress_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  auto item = entry(name);
  if (expected_instance_id && item->info.instance_id != *expected_instance_id) {
    return CallbackResult::failure("Stale component runtime instance");
  }
  if (item->may_be_active) {
    return CallbackResult::failure("Deactivate the component before cleanup");
  }
  auto rejected = begin(
    *item, "cleanup", {LifecycleState::UNCONFIGURED,
      LifecycleState::INACTIVE, LifecycleState::ERROR});
  if (rejected) {return *rejected;}
  if (!item->component) {
    return finish(
      *item, LifecycleState::ERROR,
      CallbackResult::failure("No loaded context; unload before retrying load"));
  }
  auto result = capture(
    "cleanup", [&] {
      return cleanup_component(*item->component);
    });
  if (result.success) {item->configured = false;}
  return finish(
    *item, result.success ? LifecycleState::UNCONFIGURED : LifecycleState::ERROR,
    result);
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::unload(
  const std::string & name, Timeout timeout,
  std::optional<std::string_view> expected_instance_id)
{
  if (has_local_call()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  if (!allow_dynamic_components_ && !shutdown_requested_) {
    return CallbackResult::failure("Dynamic component modification is disabled");
  }
  if (registration_validation_in_progress_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  auto item = entry(name);
  if (expected_instance_id && item->info.instance_id != *expected_instance_id) {
    return CallbackResult::failure("Stale component runtime instance");
  }
  auto rejected = begin(
    *item, "unload", {LifecycleState::UNLOADED, LifecycleState::UNCONFIGURED,
      LifecycleState::INACTIVE, LifecycleState::ACTIVE, LifecycleState::ERROR}, true);
  if (rejected) {return *rejected;}
  auto result = drain(*item, timeout);
  if (result.success && item->component && !item->component->resources_released_) {
    if (item->may_be_active) {result = stop(*item).result;}
    if (result.success) {
      result = capture(
        "deactivate descendants", [&] {
          return deactivate_descendants_for_shutdown(*item->component, timeout);
        });
    }
    if (result.success && item->configured) {
      result = capture("cleanup", [&] {return cleanup_component(*item->component);});
      if (result.success) {item->configured = false;}
    }
    if (result.success) {
      result = capture("unload", [&] {return release_resources(*item->component);});
      if (result.success) {
        std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
        item->subcomponent_context.reset();
        item->info.instance_id.clear();
      }
    }
  }
  return finish(*item, result.success ? LifecycleState::UNLOADED : LifecycleState::ERROR, result);
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::switch_components(
  const std::string & source, const std::string & target,
  bool transfer_state, Timeout timeout)
{
  if (has_local_call()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  if (!allow_dynamic_components_) {
    return CallbackResult::failure("Dynamic component modification is disabled");
  }
  if (registration_validation_in_progress_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  if (source == target) {return CallbackResult::failure("Switch source and target must differ");}
  auto old = entry(source), next = entry(target);
  if (old->info.state != LifecycleState::ACTIVE) {
    return CallbackResult::failure("Switch source must be active");
  }
  if (next->info.state == LifecycleState::UNLOADED) {
    auto result = load(target, "");
    if (!result.success) {return result;}
  }
  if (next->info.state == LifecycleState::UNCONFIGURED) {
    auto result = configure(target);
    if (!result.success) {return result;}
  }
  if (next->info.state != LifecycleState::INACTIVE) {
    return CallbackResult::failure("Switch target must be inactive");
  }
  if (transfer_state) {
    auto first = old->component->instance_;
    auto second = next->component->instance_;
    if (first->handover_state_type() != second->handover_state_type()) {
      return CallbackResult::failure("Incompatible handover state types");
    }
  }
  auto stopped = deactivate(source, timeout);
  if (!stopped.result.success) {return stopped.result;}
  return activate(
    target, transfer_state && stopped.state ? std::move(*stopped.state) : HandoverState{});
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::reload(
  const std::string & name, bool transfer_state, Timeout timeout,
  std::function<void(const std::string &)> refresh)
{
  if (has_local_call()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  if (!allow_dynamic_components_) {
    return CallbackResult::failure("Dynamic component modification is disabled");
  }
  if (registration_validation_in_progress_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  auto item = entry(name);
  const bool was_active = item->info.state == LifecycleState::ACTIVE;
  HandoverState snapshot;
  if (was_active) {
    auto stopped = deactivate(name, timeout);
    if (!stopped.result.success) {return stopped.result;}
    if (transfer_state && stopped.state) {snapshot = std::move(*stopped.state);}
  }
  auto result = unload(name, timeout);
  if (!result.success) {return result;}
  if (refresh) {
    result = capture("refresh", [&] {refresh(item->info.component); return CallbackResult{};});
    if (!result.success) {return finish(*item, LifecycleState::UNLOADED, result);}
  }
  result = load(name, "");
  if (result.success) {result = configure(name);}
  if (result.success && was_active) {result = activate(name, std::move(snapshot));}
  return result;
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::close(Timeout timeout)
{
  if (has_local_call()) {
    return CallbackResult::failure("Cannot manage a slot from inside its guarded call");
  }
  if (registration_validation_in_progress_) {
    return CallbackResult::failure("Cannot mutate a slot from its registration validator");
  }
  std::vector<std::string> names;
  {
    std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
    for (const auto & name : registration_order_) {
      auto item = component_entries_.at(name);
      if (item->info.transition.has_value())
      {return CallbackResult::failure("Cannot close manager from inside managed plugin code");}
    }
    shutdown_requested_ = true;
    names = registration_order_;
  }
  CallbackResult result;
  for (auto name = names.rbegin(); name != names.rend(); ++name) {
    auto stopped = unload(*name, timeout);
    if (!stopped.success && result.success) {result = stopped;}
  }
  return result;
}

template<class Threading>
BasicSlotManager<Threading>::BasicSlotManager(
  RuntimeFactory factory, ActivationPolicy policy,
  bool allow_dynamic_components, std::string plugin_type, Validator validator,
  NamedValidators named_validators)
: factory_(std::move(factory)), activation_policy_(policy),
  allow_dynamic_components_(allow_dynamic_components),
  plugin_type_(std::move(plugin_type)), registration_validator_(std::move(validator)),
  registration_validators_by_name_(std::move(named_validators)) {}

template<class Threading>
BasicSlotManager<Threading>::~BasicSlotManager()
{
  auto result = close(Timeout::zero());
  if (!result.success) {
    // Failed shutdown cannot unload active code. Explicit close() permits retry;
    // at final destruction keep unsafe resources alive and invalidate their views.
    for (const auto & [name, item] : component_entries_) {
      (void)name;
      if (item->component && !item->component->resources_released_) {
        item->component->context_->detach();
        (void)new std::shared_ptr<Entry>(item);
      }
    }
  }
}

template<class Threading>
typename BasicSlotManager<Threading>::ActiveCall BasicSlotManager<Threading>::borrow(
  const std::string & name,
  std::optional<std::string_view> expected_instance_id)
{
  auto call = try_borrow(std::optional<std::string>{name}, expected_instance_id);
  if (!call) {throw LifecycleError("Component is not available for active calls");}
  return std::move(*call);
}

template<class Threading>
std::optional<typename BasicSlotManager<Threading>::ActiveCall>
BasicSlotManager<Threading>::try_borrow(
  const std::optional<std::string> & component_name,
  std::optional<std::string_view> expected_instance_id)
{
  std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
  if (!component_name && activation_policy_ == ActivationPolicy::MULTIPLE) {
    throw LifecycleError("A list slot requires a component name");
  }
  auto item = component_name ? entry_unlocked(*component_name) : selected_entry_unlocked();
  if (!item) {return std::nullopt;}
  if (expected_instance_id && *expected_instance_id != std::string_view(item->info.instance_id)) {
    throw LifecycleError("Component instance_id does not match the loaded instance");
  }
  if (shutdown_requested_ || !item->info.callable || item->info.state != LifecycleState::ACTIVE) {
    return std::nullopt;
  }
  auto instance = item->component->instance_;
  guarded_call_nesting_.enter();
  if constexpr (Threading::concurrent) {++item->info.in_flight;}
  return ActiveCall(std::move(instance), std::move(item), *this);
}

template<class Threading>
void BasicSlotManager<Threading>::release_call(Entry & entry)
{
  std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
  guarded_call_nesting_.leave();
  if constexpr (Threading::concurrent) {
    if (--entry.info.in_flight == 0) {guarded_calls_drained_.notify_all();}
  }
}

template<class Threading>
LifecycleState BasicSlotManager<Threading>::get_state(const std::string & name) const
{
  return get_status(name).state;
}

template<class Threading>
std::vector<ComponentStatus> BasicSlotManager<Threading>::list() const
{
  std::lock_guard<typename Threading::Mutex> lock(slot_mutex_);
  std::vector<ComponentStatus> result;
  result.reserve(registration_order_.size());
  for (const auto & name : registration_order_) {
    result.push_back(component_entries_.at(name)->info);
  }
  return result;
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::release_resources(
  ComponentData & component)
{
  CallbackResult first_error;
  component.instance_.reset();
  if (component.context_) {component.context_->detach();}
  for (auto item = component.subcomponent_slots_.rbegin();
    item != component.subcomponent_slots_.rend(); ++item)
  {
    auto result = item->second->close();
    if (!result.success && first_error.success) {first_error = result;}
  }
  auto released = release_external_component_process(component);
  if (!released.success && first_error.success) {first_error = released;}
  if (first_error.success) {
    component.subcomponent_slots_.clear();
    component.resources_released_ = true;
    component.deactivation_pending_ = false;
  }
  return first_error;
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::configure_subcomponents(ComponentData & component)
{
  for (const auto & [name, manager] : component.subcomponent_slots_) {
    (void)name;
    for (const auto & subcomponent : manager->runtimes()) {
      auto result = manager->configure(subcomponent->storage_.name,
          subcomponent->storage_.instance_id);
      if (!result.success) {return result;}
    }
  }
  return {};
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::configure_component(ComponentData & component)
{
  auto result = configure_subcomponents(component);
  if (!result.success) {return result;}
  if (component.external_component_process_) {return configure_external_component(component);}
  if (!component.instance_) {
    return CallbackResult::failure("Component instance is not loaded; unload before configuring");
  }
  return component.instance_->configure(*component.context_);
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::cleanup_component(
  ComponentData & component)
{
  if (requires_deactivation(component)) {
    return CallbackResult::failure("Deactivate the context before cleanup");
  }
  CallbackResult first_error;
  auto record_error = [&](const CallbackResult & result) {
      if (!result.success) {
        if (first_error.success) {
          first_error = result;
        } else {
          RPP_LOG_ERROR(
            *component.logger_, "Additional component cleanup failure: %s",
            result.message.c_str());
        }
      }
    };
  if (component.external_component_process_) {
    record_error(cleanup_external_component(component));
  } else if (component.instance_) {
    record_error(component.instance_->cleanup());
  }
  record_error(cleanup_subcomponents(component));
  return first_error;
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::cleanup_subcomponents(ComponentData & component)
{
  if (requires_deactivation(component)) {
    return CallbackResult::failure("Deactivate subcomponents before cleanup");
  }
  CallbackResult first_error;
  auto record_error = [&](const CallbackResult & result) {
      if (!result.success) {
        if (first_error.success) {first_error = result;} else {
          RPP_LOG_ERROR(*component.logger_, "Additional subcomponent cleanup failure: %s",
            result.message.c_str());
        }
      }
    };
  for (auto item = component.subcomponent_slots_.rbegin();
    item != component.subcomponent_slots_.rend(); ++item)
  {
    auto subcomponents = item->second->runtimes();
    for (auto subcomponent = subcomponents.rbegin(); subcomponent != subcomponents.rend();
      ++subcomponent)
    {
      record_error(item->second->cleanup((*subcomponent)->storage_.name,
        (*subcomponent)->storage_.instance_id));
    }
  }
  return first_error;
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::activate_component(
  ComponentData & component, HandoverState state)
{
  if (component.external_component_process_) {
    return activate_external_component(component, std::move(state));
  }
  if (!component.instance_) {
    return state.has_value() ?
           CallbackResult::failure("A context without a plugin accepts no state") :
           CallbackResult{};
  }
  auto validation = component.instance_->validate_handover_state(state);
  if (!validation.success) {return validation;}
  component.activation_attempted_ = true;
  auto result = component.instance_->activate_state(std::move(state));
  return result;
}

template<class Threading>
DeactivationResult<HandoverState> BasicSlotManager<Threading>::deactivate_component(
  ComponentData & component)
{
  if (component.external_component_process_) {
    return deactivate_external_component(component);
  }
  if (!component.instance_ || !component.activation_attempted_) {return {};}
  auto result = component.instance_->deactivate_state();
  if (result.result.success) {
    component.activation_attempted_ = false;
    component.deactivation_pending_ = false;
  } else {
    result.state.reset();
  }
  return result;
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::activate_subcomponents(
  ComponentData & component)
{
  if (component.resources_released_) {
    return CallbackResult::failure("The component runtime has been released");
  }
  std::vector<std::shared_ptr<ComponentRuntime>> activated;
  CallbackResult result;
  for (const auto & [name, manager] : component.subcomponent_slots_) {
    (void)name;
    result = manager->activate_selected(activated);
    if (!result.success) {break;}
  }
  if (!result.success) {
    for (auto subcomponent = activated.rbegin(); subcomponent != activated.rend(); ++subcomponent) {
      auto rollback = (*subcomponent)->storage_.slot->deactivate(
        (*subcomponent)->storage_.name, Timeout::max(), (*subcomponent)->storage_.instance_id);
      if (!rollback.result.success) {
        RPP_LOG_ERROR(
          *component.logger_, "Component activation rollback failed: %s",
          rollback.result.message.c_str());
      }
    }
  }
  return result;
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::deactivate_subcomponents(
  ComponentData & component, Timeout timeout)
{
  if (component.resources_released_) {
    return CallbackResult::failure("The component runtime has been released");
  }
  CallbackResult first_error;
  for (auto slot = component.subcomponent_slots_.rbegin();
    slot != component.subcomponent_slots_.rend(); ++slot)
  {
    auto subcomponents = slot->second->deactivation_runtimes();
    for (auto subcomponent = subcomponents.rbegin(); subcomponent != subcomponents.rend();
      ++subcomponent)
    {
      auto result = slot->second->deactivate((*subcomponent)->storage_.name, timeout,
          (*subcomponent)->storage_.instance_id).result;
      if (!result.success) {
        if (first_error.success) {first_error = result;} else {
          RPP_LOG_ERROR(
            *component.logger_, "Additional component deactivation failure: %s",
            result.message.c_str());
        }
      }
    }
  }
  return first_error;
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::deactivate_descendants_for_shutdown(
  ComponentData & component, Timeout timeout)
{
  auto first_error = deactivate_subcomponents(component, timeout);
  // An inactive subcomponent may still own active descendants when its callbacks left them running.
  for (auto slot = component.subcomponent_slots_.rbegin();
    slot != component.subcomponent_slots_.rend(); ++slot)
  {
    auto subcomponents = slot->second->runtimes();
    for (auto subcomponent = subcomponents.rbegin(); subcomponent != subcomponents.rend();
      ++subcomponent)
    {
      auto result = deactivate_descendants_for_shutdown(
        slot->second->component_data((*subcomponent)->storage_.name,
        (*subcomponent)->storage_.instance_id), timeout);
      if (!result.success) {
        if (first_error.success) {first_error = result;} else {
          RPP_LOG_ERROR(
            *component.logger_, "Additional subtree deactivation failure: %s",
            result.message.c_str());
        }
      }
    }
  }
  return first_error;
}

template<class Threading>
bool BasicSlotManager<Threading>::requires_deactivation(
  ComponentData & component)
{
  if (component.resources_released_) {return false;}
  if (component.activation_attempted_) {return true;}
  for (const auto & [name, manager] : component.subcomponent_slots_) {
    (void)name;
    if (!manager->deactivation_runtimes().empty()) {return true;}
    for (const auto & subcomponent : manager->runtimes()) {
      if (requires_deactivation(manager->component_data(subcomponent->storage_.name,
          subcomponent->storage_.instance_id))) {return true;}
    }
  }
  return false;
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::process_deactivations(
  const std::string & name, std::string_view instance_id, Timeout timeout)
{
  auto & component = component_data(name, instance_id);
  if (component.resources_released_) {
    return CallbackResult::failure("The component runtime has been released");
  }
  CallbackResult first_error;
  if (Threading::take(component.deactivation_pending_)) {
    try {
      first_error = deactivate(name, timeout).result;
    } catch (const std::exception & error) {
      first_error = CallbackResult::failure(error.what());
    } catch (...) {
      first_error = CallbackResult::failure("Self-deactivation failed: unknown exception");
    }
    if (!first_error.success) {component.deactivation_pending_ = true;}
  }
  auto descendants = process_subcomponent_deactivations(component, timeout);
  if (!descendants.success && first_error.success) {first_error = descendants;}
  return first_error;
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::process_subcomponent_deactivations(
  ComponentData & component, Timeout timeout)
{
  CallbackResult first_error;
  for (const auto & [slot_name, manager] : component.subcomponent_slots_) {
    (void)slot_name;
    for (const auto & subcomponent : manager->runtimes()) {
      auto result = manager->process_deactivations(subcomponent->storage_.name,
          subcomponent->storage_.instance_id, timeout);
      if (!result.success && first_error.success) {first_error = result;}
    }
  }
  return first_error;
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::configure_external_component(
  ComponentData & component)
{
  auto * adapter = dynamic_cast<ClientAdapter *>(component.instance_.get());
  if (!adapter) {return CallbackResult::failure("Remote component has no client adapter");}
  try {
    return component.external_component_process_->configure_component(
      adapter->get_info_adapter_client__().connection_name);
  } catch (const kj::Exception & error) {
    return CallbackResult::failure(error.getDescription().cStr());
  } catch (const std::exception & error) {
    return CallbackResult::failure(error.what());
  }
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::cleanup_external_component(
  ComponentData & component)
{
  auto * adapter = dynamic_cast<ClientAdapter *>(component.instance_.get());
  if (!adapter) {return CallbackResult::failure("Remote component has no client adapter");}
  try {
    return component.external_component_process_->cleanup_component(
      adapter->get_info_adapter_client__().connection_name);
  } catch (const kj::Exception & error) {
    return CallbackResult::failure(error.getDescription().cStr());
  } catch (const std::exception & error) {
    return CallbackResult::failure(error.what());
  }
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::activate_external_component(
  ComponentData & component, HandoverState state)
{
  auto * adapter = dynamic_cast<ClientAdapter *>(component.instance_.get());
  if (!adapter) {return CallbackResult::failure("Remote component has no client adapter");}
  try {
    auto result = component.external_component_process_->activate_component(
      adapter->get_info_adapter_client__().connection_name, *adapter, std::move(state));
    if (result.success) {component.activation_attempted_ = true;}
    return result;
  } catch (const kj::Exception & error) {
    return CallbackResult::failure(error.getDescription().cStr());
  } catch (const std::exception & error) {
    return CallbackResult::failure(error.what());
  }
}

template<class Threading>
DeactivationResult<HandoverState> BasicSlotManager<Threading>::deactivate_external_component(
  ComponentData & component)
{
  if (!component.activation_attempted_) {return {};}
  auto * adapter = dynamic_cast<ClientAdapter *>(component.instance_.get());
  if (!adapter) {return {CallbackResult::failure("Remote component has no client adapter"), {}};}
  try {
    auto result = component.external_component_process_->deactivate_component(
      adapter->get_info_adapter_client__().connection_name, *adapter);
    if (result.result.success) {
      component.activation_attempted_ = false;
      component.deactivation_pending_ = false;
    } else {
      result.state.reset();
    }
    return result;
  } catch (const kj::Exception & error) {
    return {CallbackResult::failure(error.getDescription().cStr()), {}};
  } catch (const std::exception & error) {
    return {CallbackResult::failure(error.what()), {}};
  }
}

template<class Threading>
CallbackResult BasicSlotManager<Threading>::release_external_component_process(
  ComponentData & component)
{
  if (!component.external_component_process_) {return {};}
  if (component.external_component_process_.use_count() == 1) {
    auto result = component.external_component_process_->close();
    if (!result.success) {return result;}
  }
  component.external_component_process_.reset();
  return {};
}


template<class Threading>
CallbackResult BasicSlotManager<Threading>::request_deactivation(ComponentData & component)
{
  if (component.resources_released_) {
    return CallbackResult::failure("The component runtime has been released");
  }
  component.deactivation_pending_ = true;
  return {};
}

template class BasicSlotManager<SingleThreaded>;
template class BasicSlotManager<Concurrent>;

}  // namespace rpp
