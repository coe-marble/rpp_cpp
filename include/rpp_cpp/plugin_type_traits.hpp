#pragma once

#include <map>
#include <vector>
#include <string>

namespace rpp
{

template <typename T, typename = std::void_t<>> struct rpp_has_components : std::false_type {};
template <typename T> struct rpp_has_components<T, std::void_t<decltype(T::COMPONENTS)>> : std::true_type {};

template <typename T, typename = std::void_t<>> struct rpp_has_parameters : std::false_type {};
template <typename T> struct rpp_has_parameters<T, std::void_t<decltype(T::PARAMETERS)>> : std::true_type {};

} // namespace rpp::params