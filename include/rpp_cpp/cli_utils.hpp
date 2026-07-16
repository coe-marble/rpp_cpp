# pragma once
#include <array>
#include <cstdio>
#include <iostream>
#include <string>


std::string runCommand(const std::string& cmd) {
    std::array<char, 2048> buffer;
    std::string result;

    // Use _popen for Windows, popen for Linux/macOS
#ifdef _WIN32
    auto pipe = _popen(cmd.c_str(), "r");
#else
    auto pipe = popen(cmd.c_str(), "r");
#endif

    if (!pipe) return "Error: Failed to open pipe.";

    // Read the output line by line
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }

#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif

    return result;
}
