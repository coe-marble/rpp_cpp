#pragma once

#include <string>
#include <string_view>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace rpp {

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR
};

struct LoggerOptions {
    LogLevel level = LogLevel::DEBUG;
    std::string name = "rpp_logger";
};

class RppLogger {

public:
    // Default constructor for standard environment or fallback ROS core logger
    RppLogger(const LoggerOptions& options = LoggerOptions());

    // Explicit constructor to hook into a specific ROS 2 Node/Component logger name
    explicit RppLogger(const std::string& name);
    ~RppLogger();

    // Core log execution logic containing the underlying backends mapping
    void log(LogLevel level, const char* file, int line, std::string_view msg);

private:
    LoggerOptions options_;
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

// Helper proxy class designed to capture runtime source file/line location metadata automatically
class LogMessageBuilder {
    template<typename ... Args>
    [[maybe_unused]] std::string format(const std::string& format, Args ... args )
    {
        int size_s = std::snprintf( nullptr, 0, format.c_str(), args ... ) + 1; // Extra space for '\0'
        if( size_s <= 0 ){ throw std::runtime_error( "Error during formatting." ); }
        auto size = static_cast<size_t>( size_s );
        std::unique_ptr<char[]> buf( new char[ size ] );
        std::snprintf( buf.get(), size, format.c_str(), args ... );
        return std::string( buf.get(), buf.get() + size - 1 ); // We don't want the '\0' inside
    }
public:
    LogMessageBuilder(RppLogger& logger, LogLevel level, const char* file, int line)
        : logger_(logger), level_(level), file_(file), line_(line) {}


    template<typename ... Args>
    void operator()(const std::string& msg, Args ... args) {
        logger_.log(level_, file_, line_, format(msg, args ...));
    }

    void operator()(const std::string& msg) {
        logger_.log(level_, file_, line_, msg);
    }

private:
    RppLogger& logger_;
    LogLevel level_;
    const char* file_;
    int line_;
};

// Clean context macros using the updated names
#define RPP_LOG_DEBUG(logger_inst, msg, ...) \
    rpp::LogMessageBuilder(logger_inst, rpp::LogLevel::DEBUG, __FILE__, __LINE__)(msg, ##__VA_ARGS__)
#define RPP_LOG_INFO(logger_inst, msg, ...)  \
    rpp::LogMessageBuilder(logger_inst, rpp::LogLevel::INFO,  __FILE__, __LINE__)(msg, ##__VA_ARGS__)
#define RPP_LOG_WARN(logger_inst, msg, ...)  \
    rpp::LogMessageBuilder(logger_inst, rpp::LogLevel::WARN,  __FILE__, __LINE__)(msg, ##__VA_ARGS__)
#define RPP_LOG_ERROR(logger_inst, msg, ...) \
    rpp::LogMessageBuilder(logger_inst, rpp::LogLevel::ERROR, __FILE__, __LINE__)(msg, ##__VA_ARGS__)

#define RPP_LOG_INFO_ONCE(logger_inst, msg, ...) \
    do { \
        static std::once_flag rpp_log_once_flag; \
        std::call_once(rpp_log_once_flag, [&]() { \
            RPP_LOG_INFO(logger_inst, msg, ##__VA_ARGS__); \
        }); \
    } while (false)

#define RPP_LOG_DEBUG_ONCE(logger_inst, msg, ...) \
    do { \
        static std::once_flag rpp_log_once_flag; \
        std::call_once(rpp_log_once_flag, [&]() { \
            RPP_LOG_DEBUG(logger_inst, msg, ##__VA_ARGS__); \
        }); \
    } while (false)

#define RPP_LOG_WARN_ONCE(logger_inst, msg, ...) \
    do { \
        static std::once_flag rpp_log_once_flag; \
        std::call_once(rpp_log_once_flag, [&]() { \
            RPP_LOG_WARN(logger_inst, msg, ##__VA_ARGS__); \
        }); \
    } while (false)

#define RPP_LOG_ERROR_ONCE(logger_inst, msg, ...) \
    do { \
        static std::once_flag rpp_log_once_flag; \
        std::call_once(rpp_log_once_flag, [&]() { \
            RPP_LOG_ERROR(logger_inst, msg, ##__VA_ARGS__); \
        }); \
    } while (false)

} // namespace rpp
