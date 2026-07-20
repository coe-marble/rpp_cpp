#include <pybind11/pybind11.h>
#include <pybind11/embed.h>
#include <filesystem>
#include <string>

namespace rpp {

    pybind11::module_ load_python_module(const std::string& module_path, pybind11::scoped_interpreter& /* guard*/) {
        if (!std::filesystem::exists(module_path)) {
            throw std::runtime_error("Python module not found: " + module_path);
        }

        std::filesystem::path p(module_path);
        std::string directory = p.parent_path().string();
        std::string module_name = p.stem().string(); // Uzima samo ime bez ".py"

        // Dodajte direktorij u Pythonov sys.path kako bi ga interpreter znao pronaći
        pybind11::module_ sys = pybind11::module_::import("sys");
        pybind11::list path = sys.attr("path");
        path.append(directory);

        return pybind11::module_::import(module_name.c_str());
    }


} // namespace rpp