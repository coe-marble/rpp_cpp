#include "rpp_cpp/python_helpers.hpp"
#include <pybind11/pybind11.h>
#include <pybind11/embed.h>

namespace rpp {

    pybind11::module_ load_python_module(const std::string& module_path, pybind11::scoped_interpreter& /* guard*/)
    {
        if (!std::filesystem::exists(module_path)) {
            throw std::runtime_error("Python module not found: " + module_path);
        }

        std::filesystem::path p(module_path);
        std::string directory = p.parent_path().string();
        std::string module_name = p.stem().string();

        pybind11::module_::import("importlib").attr("invalidate_caches")();
        pybind11::module_ sys = pybind11::module_::import("sys");
        sys.attr("dont_write_bytecode") = true;

        pybind11::list path = sys.attr("path");

        bool exists_in_path = false;
        for (size_t i = 0; i < path.size(); ++i) {
            if (path[i].cast<std::string>() == directory) {
                exists_in_path = true;
                break;
            }
        }
        if (!exists_in_path) {
            path.insert(0, directory);
        }

        return pybind11::module_::import(module_name.c_str());
    }

    void close_python_module(const std::string& module_path, pybind11::scoped_interpreter& /* guard */) {
        std::filesystem::path p(module_path);
        std::string module_name = p.stem().string();

        pybind11::module_ sys = pybind11::module_::import("sys");

        pybind11::list path = sys.attr("path");
        for (long unsigned int i = 0; i < path.size(); ++i) {
            if (path[i].cast<std::string>() == p.parent_path().string()) {
                path.attr("pop")(i);
                break;
            }
        }

        pybind11::dict modules = sys.attr("modules");
        if (modules.contains(module_name.c_str())) {
            modules.attr("pop")(module_name.c_str());
        }
    }

} // namespace rpp