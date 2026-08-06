#pragma once
#include <string>
#include <vector>
#include <map>
#include "parameter_description.hpp"

namespace rpp::params {

class Parameters
{
friend class ParameterHandler;
private:
    Parameters(
        std::map<std::string, ParameterValue> values)
          : values_(std::move(values)) {}

public:
    Parameters()
        : values_() {}

    template<typename T>
    T get(std::string_view name) const
    {
        if (!contains(name))
        {
            throw std::runtime_error(
                "Parameter '" + std::string(name)
                + "' not found.");
        }
        return ParameterConverter<T>::from(
            values_.at(std::string(name))
        );
    }


    bool contains(
        std::string_view name) const
    {
        return values_.find(std::string(name))
            != values_.end();
    }


private:
    std::map<std::string, ParameterValue> values_;
};
} // namespace rpp::params