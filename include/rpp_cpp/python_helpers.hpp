#pragma once
#include <string>

namespace pybind11
{
class module_;
class handle;
class dict;
}


namespace rpp
{
/// Initialize the process-wide embedded Python interpreter once.
/// It intentionally remains alive until process exit so component threads can
/// safely acquire the GIL throughout the runtime's lifetime.
void ensure_python_interpreter();

pybind11::module_ load_python_module(const std::string & module_path);

void close_python_module(const std::string & module_path);

} // namespace rpp
