#pragma once

#include <string>
#include <map>


class LibraryManager;

namespace rpp {

class PluginInfo final {

    private:
        PluginInfo() = default;

    public:
        std::string plugin_name;
        std::string source_language;
        std::string library;
        std::string plugin_type_name;
        std::string class_name;
        std::string plugin_path;
        std::string source_file;
        std::string plugin_shared_library_path;
        std::string plugin_type_shared_library_path;
        std::map<std::string, std::string> components;


    friend class LibraryManager;

};

inline std::string to_lower_copy(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

inline bool is_cpp_source_language(const PluginInfo& info) {
    return to_lower_copy(info.source_language) == "cpp";
}

}  // namespace rpp