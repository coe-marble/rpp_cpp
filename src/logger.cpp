#include "rpp_cpp/logger.hpp"

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

struct RppLogger::Impl {
#if defined(USE_ROS2_COMPILATION)
    // Unique ptr prevents cross-contamination of structures when ROS is absent
    std::shared_ptr<rclcpp::Logger> ros_logger;
#elif defined(USE_SPDLOG_COMPILATION)
    std::shared_ptr<spdlog::logger> logger;
#elif defined(USE_STD_LOGGING)
    std::string logger_name;
#endif
};


    // Default constructor for standard environment or fallback ROS core logger
RppLogger::RppLogger(const LoggerOptions& options)
    : options_(options),
      pimpl_(std::make_unique<Impl>())
{
    #if defined(USE_ROS2_COMPILATION)
        pimpl_->ros_logger = std::make_shared<rclcpp::Logger>(rclcpp::get_logger(options_.name));
    #elif defined(USE_SPDLOG_COMPILATION)
        // Setup a named or default spdlog channel safely
        pimpl_->logger = spdlog::get(options_.name);
        if (!pimpl_->logger) {
            pimpl_->logger = spdlog::stdout_color_mt(options_.name);
        }
    #elif defined(USE_STD_LOGGING)
        pimpl_->logger_name = options_.name;
    #endif
}

    // Explicit constructor to hook into a specific ROS 2 Node/Component logger name
RppLogger::RppLogger(const std::string& name)
    : options_({LogLevel::INFO, name})
{
    #if defined(USE_ROS2_COMPILATION)
        pimpl_->ros_logger = std::make_shared<rclcpp::Logger>(rclcpp::get_logger(name));
    #elif defined(USE_SPDLOG_COMPILATION)
        pimpl_->logger = spdlog::get(name);
        if (!pimpl_->logger) {
            pimpl_->logger = spdlog::stdout_color_mt(name);
        }
    #elif defined(USE_STD_LOGGING)
        pimpl_->logger_name = name;
    #endif
    }

RppLogger::~RppLogger() = default;

    // Core log execution logic containing the underlying backends mapping
void RppLogger::log(LogLevel level, const char* file, int line, std::string_view msg)
{
    #if defined(USE_ROS2_COMPILATION)
        if (!pimpl_->ros_logger) return;
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
        rcutils_log(&location, severity, pimpl_->ros_logger->get_name(), "%s", msg.data());
    #elif defined(USE_SPDLOG_COMPILATION)
        auto standard_logger = pimpl_->logger ? pimpl_->logger : spdlog::default_logger();
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

} // namespace rpp