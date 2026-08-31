#pragma once

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace rpp {

class ChildProcess final {
public:
    explicit ChildProcess(std::vector<std::string> args)
        : args_(std::move(args))
    {
        if (args_.empty() || args_[0].empty()) {
            throw std::invalid_argument("Child process requires a command.");
        }
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&&) = delete;
    ChildProcess& operator=(ChildProcess&&) = delete;

    ~ChildProcess() { cleanup(); }

    void start();
    bool running();
    int wait(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    void terminate();
    void kill();

private:
    void cleanup() noexcept;

#ifndef _WIN32
    pid_t pid_ = -1;
#else
    HANDLE process_handle_ = nullptr;
#endif
    std::vector<std::string> args_;
    int exit_code_ = -1;
    bool started_ = false;
};

}  // namespace rpp