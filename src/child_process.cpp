#include "rpp_cpp/child_process.hpp"

#include <cerrno>
#include <cstring>
#include <thread>

#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <sstream>
#endif

namespace rpp {

#ifndef _WIN32

void ChildProcess::start()
{
    if (started_) {
        throw std::logic_error("Child process has already been started.");
    }

    // argv is built before fork(): the child must avoid heap allocation,
    // since another thread may hold an allocator lock at fork time.
    std::vector<char*> argv;
    argv.reserve(args_.size() + 1);
    for (auto& arg : args_) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    pid_ = fork();
    if (pid_ < 0) {
        throw std::runtime_error(
            "Unable to fork child process: " + std::string(std::strerror(errno)));
    }
    if (pid_ == 0) {
        execvp(argv[0], argv.data());
        _exit(127);
    }
    started_ = true;
}

bool ChildProcess::running()
{
    if (!started_ || pid_ < 0) {
        return false;
    }
    int status = 0;
    const auto result = waitpid(pid_, &status, WNOHANG);
    if (result == pid_) {
        if (WIFEXITED(status)) {
            exit_code_ = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            exit_code_ = 128 + WTERMSIG(status);
        }
        pid_ = -1;
        started_ = false;
        return false;
    }
    return result == 0 || (result < 0 && errno == EINTR);
}

int ChildProcess::wait(std::chrono::milliseconds timeout)
{
    if (!started_ || pid_ < 0) {
        return exit_code_;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    while (true) {
        const auto result = waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            if (WIFEXITED(status)) {
                exit_code_ = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                exit_code_ = 128 + WTERMSIG(status);
            }
            pid_ = -1;
            started_ = false;
            return exit_code_;
        }
        if (result < 0 && errno != EINTR) {
            throw std::runtime_error(
                "Unable to wait for child process: " + std::string(std::strerror(errno)));
        }
        if (timeout != std::chrono::milliseconds::max() &&
                std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Timed out waiting for child process.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void ChildProcess::terminate()
{
    if (started_ && pid_ >= 0 && ::kill(pid_, SIGTERM) != 0 && errno != ESRCH) {
        throw std::runtime_error(
            "Unable to terminate child process: " + std::string(std::strerror(errno)));
    }
}

void ChildProcess::kill()
{
    if (started_ && pid_ >= 0 && ::kill(pid_, SIGKILL) != 0 && errno != ESRCH) {
        throw std::runtime_error(
            "Unable to kill child process: " + std::string(std::strerror(errno)));
    }
}

#else

static std::wstring utf8_to_wide(const std::string& value)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("Unable to convert process argument to UTF-16.");
    }
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    result.pop_back();
    return result;
}

static std::wstring quote_windows_argument(const std::wstring& argument)
{
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result += L'\"';
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            result += character;
            backslashes = 0;
        }
    }
    result.append(backslashes * 2, L'\\');
    return result + L'\"';
}

void ChildProcess::start()
{
    if (started_) {
        throw std::logic_error("Child process has already been started.");
    }

    std::wstring command_line;
    for (const auto& argument : args_) {
        if (!command_line.empty()) {
            command_line += L" ";
        }
        command_line += quote_windows_argument(utf8_to_wide(argument));
    }
    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0,
            nullptr, nullptr, &startup_info, &process_info)) {
        throw std::runtime_error("Unable to create child process.");
    }
    CloseHandle(process_info.hThread);
    process_handle_ = process_info.hProcess;
    started_ = true;
}

bool ChildProcess::running()
{
    return started_ && WaitForSingleObject(process_handle_, 0) == WAIT_TIMEOUT;
}

int ChildProcess::wait(std::chrono::milliseconds timeout)
{
    if (!started_) {
        return exit_code_;
    }
    const auto milliseconds = timeout == std::chrono::milliseconds::max()
        ? INFINITE : static_cast<DWORD>(timeout.count());
    const auto result = WaitForSingleObject(process_handle_, milliseconds);
    if (result == WAIT_TIMEOUT) {
        throw std::runtime_error("Timed out waiting for child process.");
    }
    if (result != WAIT_OBJECT_0) {
        throw std::runtime_error("Unable to wait for child process.");
    }
    DWORD code = 0;
    GetExitCodeProcess(process_handle_, &code);
    exit_code_ = static_cast<int>(code);
    CloseHandle(process_handle_);
    process_handle_ = nullptr;
    started_ = false;
    return exit_code_;
}

void ChildProcess::terminate()
{
    if (started_ && !TerminateProcess(process_handle_, 1)) {
        throw std::runtime_error("Unable to terminate child process.");
    }
}

void ChildProcess::kill()
{
    terminate();
}

#endif

void ChildProcess::cleanup() noexcept
{
    if (!started_) {
        return;
    }
    try {
        terminate();
        wait(std::chrono::seconds(2));
    } catch (...) {
        try {
            kill();
            wait();
        } catch (...) {
        }
    }
}

}  // namespace rpp