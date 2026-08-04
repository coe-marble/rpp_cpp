#pragma once
#include <filesystem>
#include <string>

namespace pybind11 {
    class module_;
    class scoped_interpreter;
    class handle;
    class dict;
}


namespace rpp {
    pybind11::module_ load_python_module(
        const std::string& module_path, pybind11::scoped_interpreter&);

    void close_python_module(
        const std::string& module_path, pybind11::scoped_interpreter&);

} // namespace rpp