#pragma once

#include <stdexcept>
#include <utility>

namespace rpp
{
template<typename T>
T ComponentContext::get_parameter(const std::string & name) const
{
  if (!parameters_.contains(name)) {
    throw std::runtime_error("Parameter '" + name + "' not found.");
  }
  return parameters_.get<T>(name);
}

template<typename T>
std::shared_ptr<T> ComponentContext::get_component(const std::string & name) const
{
  auto selected = slot(name).selected_context();
  if (!selected) {
    throw std::runtime_error(
            "Subcomponent slot '" + name + "' is empty.");
  }
  return selected->get_instance<T>();
}

template<typename T>
std::vector<std::shared_ptr<T>> ComponentContext::get_components(const std::string & name) const
{
  std::vector<std::shared_ptr<T>> instances;
  for (const auto & subcomponent : slot(name).contexts()) {
    instances.push_back(subcomponent->get_instance<T>());
  }
  return instances;
}

template<typename T>
std::shared_ptr<T> ComponentContext::get_instance() const
{
  auto casted_instance = std::static_pointer_cast<T>(instance_);
  if (!casted_instance) {
    throw std::runtime_error("Failed to cast instance to the requested type.");
  }
  return casted_instance;
}

template<typename T, typename Callback>
auto ComponentContext::with_active_component(
  const std::string & slot_name, const std::string & name,
  Callback && callback, std::optional<uint64_t> generation) const
-> std::invoke_result_t<Callback, T &>
{return slot(slot_name).with_active<T>(name, std::forward<Callback>(callback), generation);}

}  // namespace rpp
