#pragma once

#include <string>
#include "parameter_description.hpp"
#include "list_definitions.hpp"
#include "rpp_param_struct_headers.hpp"
#include "plugin_def.hpp"
#include "context.hpp"

#define RPP_COMPONENTS(...) \
    static inline const std::map<std::string, std::string> COMPONENTS = { __VA_ARGS__ };

#define RPP_PARAMETERS(...) \
    static inline const std::vector<rpp::params::ParameterDescription> PARAMETERS = { __VA_ARGS__ };
