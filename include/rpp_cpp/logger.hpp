#pragma once

#include <string>
#include <string_view>
#include <memory>

// Conditional compilation includes wrapped cleanly inside the header
#if defined(USE_ROS2_COMPILATION) || defined(__has_include) && __has_include(<rclcpp/rclcpp.hpp>)
    #ifndef USE_ROS2_COMPILATION
        #define USE_ROS2_COMPILATION // Force-enable it if the header exists on the system
    #endif
    #include <rclcpp/rclcpp.hpp>
#elif defined(USE_SPDLOG_COMPILATION)
    #ifndef USE_SPDLOG_COMPILATION
        #define USE_SPDLOG_COMPILATION // Force-enable it if the header exists on the system
    #endif
    #include <spdlog/spdlog.hpp>
    #include <spdlog/sinks/stdout_color_sinks.h>
#else
    #define USE_STD_LOGGING
    #include <iostream>
#endif


namespace rpp {

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR
};

class RppLogger {
public:
    // Default constructor for standard environment or fallback ROS core logger
    RppLogger() {
    #if defined(USE_ROS2_COMPILATION)
        ros_logger_ = std::make_unique<rclcpp::Logger>(rclcpp::get_logger("rpp_core"));
    #elif defined(USE_SPDLOG_COMPILATION)
        // Setup a named or default spdlog channel safely
        logger_ = spdlog::get("rpp_logger");
        if (!logger_) {
            logger_ = spdlog::stdout_color_mt("rpp_logger");
        }
    #elif defined(USE_STD_LOGGING)
        logger_name_ = "rpp_logger";
    #endif
    }

    // Explicit constructor to hook into a specific ROS 2 Node/Component logger name
    explicit RppLogger(const std::string& name) {
    #if defined(USE_ROS2_COMPILATION)
        ros_logger_ = std::make_unique<rclcpp::Logger>(rclcpp::get_logger(name));
    #elif defined(USE_SPDLOG_COMPILATION)
        logger_ = spdlog::get(name);
        if (!logger_) {
            logger_ = spdlog::stdout_color_mt(name);
        }
    #elif defined(USE_STD_LOGGING)
        logger_name_ = name;
    #endif
    }

    ~RppLogger() = default;

    // Core log execution logic containing the underlying backends mapping
    void log(LogLevel level, const char* file, int line, std::string_view msg) {
    #if defined(USE_ROS2_COMPILATION)
        if (!ros_logger_) return;
        // Correct struct assignment order for rcutils: function, file, line
        rcutils_log_location_t location{"", file, static_cast<size_t>(line)};

        int severity = RCUTILS_LOG_SEVERITY_INFO;
        switch (level) {
            case LogLevel::DEBUG: severity = RCUTILS_LOG_SEVERITY_DEBUG; break;
            case LogLevel::INFO:  severity = RCUTILS_LOG_SEVERITY_INFO;  break;
            case LogLevel::WARN:  severity = RCUTILS_LOG_SEVERITY_WARN;  break;
            case LogLevel::ERROR: severity = RCUTILS_LOG_SEVERITY_ERROR; break;
        }

        // Call the official public rcutils function to safely forward logs to ROS 2
        rcutils_log(&location, severity, ros_logger_->get_name(), "%s", msg.data());
    #elif defined(USE_SPDLOG_COMPILATION)
        auto standard_logger = logger_ ? logger_ : spdlog::default_logger();
        spdlog::source_loc loc{file, line, ""};
        switch (level) {
            case LogLevel::DEBUG: standard_logger->log(loc, spdlog::level::debug, msg); break;
            case LogLevel::INFO:  standard_logger->log(loc, spdlog::level::info, msg); break;
            case LogLevel::WARN:  standard_logger->log(loc, spdlog::level::warn, msg); break;
            case LogLevel::ERROR: standard_logger->log(loc, spdlog::level::err, msg); break;
        }
    #elif defined(USE_STD_LOGGING)
        const char* lvl_str = "[INFO]";
        std::ostream* out = &std::cout;

        switch (level) {
            case LogLevel::DEBUG: lvl_str = "[DEBUG]"; break;
            case LogLevel::INFO:  lvl_str = "[INFO]";  break;
            case LogLevel::WARN:  lvl_str = "[WARN]";  break;
            case LogLevel::ERROR: lvl_str = "[ERROR]"; out = &std::cerr; break;
        }
        // Ispis u formatu: [RAZINA] [datoteka:linija] poruka
        *out << lvl_str << " [" << file << ":" << line << "] " << msg << "\n";
    #endif
    }

private:
#if defined(USE_ROS2_COMPILATION)
    // Unique ptr prevents cross-contamination of structures when ROS is absent
    std::unique_ptr<rclcpp::Logger> ros_logger_;
#elif defined(USE_SPDLOG_COMPILATION)
    std::shared_ptr<spdlog::logger> logger_;
#elif defined(USE_STD_LOGGING)
    std::string logger_name_;
#endif
};

// Helper proxy class designed to capture runtime source file/line location metadata automatically
class LogMessageBuilder {
public:
    LogMessageBuilder(RppLogger& logger, LogLevel level, const char* file, int line)
        : logger_(logger), level_(level), file_(file), line_(line) {}

    void operator()(std::string_view msg) {
        logger_.log(level_, file_, line_, msg);
    }

private:
    RppLogger& logger_;
    LogLevel level_;
    const char* file_;
    int line_;
};

// Clean context macros using the updated names
#define RPP_LOG_DEBUG(logger_inst, msg) rpp::LogMessageBuilder(logger_inst, rpp::LogLevel::DEBUG, __FILE__, __LINE__)(msg)
#define RPP_LOG_INFO(logger_inst, msg)  rpp::LogMessageBuilder(logger_inst, rpp::LogLevel::INFO,  __FILE__, __LINE__)(msg)
#define RPP_LOG_WARN(logger_inst, msg)  rpp::LogMessageBuilder(logger_inst, rpp::LogLevel::WARN,  __FILE__, __LINE__)(msg)
#define RPP_LOG_ERROR(logger_inst, msg) rpp::LogMessageBuilder(logger_inst, rpp::LogLevel::ERROR, __FILE__, __LINE__)(msg)

} // namespace rpp