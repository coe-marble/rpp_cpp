#pragma once
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <variant>
#include "rpp_param_struct_headers.hpp"
#include "parameters.hpp"
#include "python_helpers.hpp"

namespace rpp::params {

class ParameterHandler
{
private:
    const std::string COMPONENT_PARAMS_PATH_PY = "params/parameters.py";
    std::string component_path_;

    pybind11::dict get_python_object_as_dict(pybind11::handle obj);
    ParameterValue parse_parameter_from_python(pybind11::handle value);

public:
    ParameterHandler(const std::string& component_path);

    ~ParameterHandler();

    std::map<std::string, ParameterValue> load_parameters_from_python_module();

    static void resolve_params(
        const ParametersDescription& description,
        const std::map<std::string, ParameterValue>& instance,
        std::unique_ptr<Parameters>& out_params);

    static void resolve_params(
        const std::map<std::string, ParameterValue>& description,
        const std::map<std::string, ParameterValue>& instance,
        std::unique_ptr<Parameters>& out_params);
};


} // namespace rpp::params