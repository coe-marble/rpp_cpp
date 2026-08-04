#include "rpp_cpp/parameter_handler.hpp"
#include <pybind11/pybind11.h>
#include <pybind11/embed.h>

namespace rpp::params
{


static pybind11::scoped_interpreter& get_python_interpreter()
{
    static pybind11::scoped_interpreter guard{};
    return guard;
}
static void ensure_python_interpreter()
{
    get_python_interpreter();
}

ParameterHandler::ParameterHandler(const std::string& component_path)
    : component_path_(component_path)
{
    ensure_python_interpreter();
}

ParameterHandler::~ParameterHandler()
{
    close_python_module(
        component_path_ + "/" + COMPONENT_PARAMS_PATH_PY,
        get_python_interpreter()
        );
}


void ParameterHandler::resolve_params(
    const ParametersDescription& description,
    const std::map<std::string, ParameterValue>& instance,
    std::unique_ptr<Parameters>& out_params)
{
    std::map<std::string, ParameterValue> resolved_params;

    for (const auto& param_desc : description)
    {
        auto it = instance.find(param_desc.name);
        if (it != instance.end())
            resolved_params[param_desc.name] = it->second;
        else
            resolved_params[param_desc.name] = param_desc.defaultValue;
    }
    out_params.reset(new Parameters(resolved_params));
}

void ParameterHandler::resolve_params(
    const std::map<std::string, ParameterValue>& description,
    const std::map<std::string, ParameterValue>& instance,
    std::unique_ptr<Parameters>& out_params)
{
    std::map<std::string, ParameterValue> resolved_params;

    for (const auto& [param_name, param_desc] : description)
    {
        auto it = instance.find(param_name);
        if (it != instance.end())
            resolved_params[param_name] = it->second;
        else
            resolved_params[param_name] = param_desc;
    }

    out_params.reset(new Parameters(resolved_params));
}

std::map<std::string, ParameterValue>
ParameterHandler::load_parameters_from_python_module()
{
    pybind11::module_ params_module =
        load_python_module(
            component_path_ + "/" + COMPONENT_PARAMS_PATH_PY,
            get_python_interpreter());
    if (!pybind11::hasattr(params_module, "ComponentParameters"))
        throw std::runtime_error("Class 'ComponentParameters' not found in the Python module.");

    pybind11::dict target_dict = get_python_object_as_dict(params_module.attr("ComponentParameters"));

    std::map<std::string, ParameterValue> parameters_map;
    for (auto item : target_dict)
    {
        std::string key = item.first.cast<std::string>();
        if (key.rfind("__", 0) == 0)
            continue; // Skip private attributes
        parameters_map[key] = parse_parameter_from_python(item.second);
    }
    return parameters_map;
}

pybind11::dict
ParameterHandler::get_python_object_as_dict(pybind11::handle obj)
{
    if (pybind11::isinstance<pybind11::dict>(obj))
        return pybind11::reinterpret_borrow<pybind11::dict>(obj);

    bool is_dataclass = pybind11::hasattr(obj, "__dataclass_fields__");
    pybind11::dict target_dict;
    if (is_dataclass)
    {
        pybind11::module_ dataclasses_mod = pybind11::module_::import("dataclasses");
        return dataclasses_mod.attr("asdict")(obj).cast<pybind11::dict>();
    }
    else
        return obj.attr("__dict__").cast<pybind11::dict>();
}

ParameterValue
ParameterHandler::parse_parameter_from_python(pybind11::handle value)
{
    if (pybind11::isinstance<pybind11::bool_>(value))
        return ParameterValue(value.cast<bool>());
    else if (pybind11::isinstance<pybind11::int_>(value))
        return ParameterValue(value.cast<int64_t>());
    else if (pybind11::isinstance<pybind11::float_>(value))
        return ParameterValue(value.cast<double>());
    else if (pybind11::isinstance<pybind11::str>(value))
        return ParameterValue(value.cast<std::string>());
    else if (pybind11::isinstance<pybind11::list>(value))
    {
        std::vector<ParameterValue> vec;
        for (const auto& elem : value) {
            vec.push_back(parse_parameter_from_python(elem));
        }
        return ParameterValue(vec);
    }
    auto target_dict = get_python_object_as_dict(value);

    std::map<std::string, ParameterValue> map;
    for (auto item : target_dict)
    {
        std::string key = item.first.cast<std::string>();
        if (key.rfind("__", 0) == 0)
            continue; // Skip private attributes
        map[key] = parse_parameter_from_python(item.second);
    }
    return ParameterValue(map);
}
} // namespace rpp::params