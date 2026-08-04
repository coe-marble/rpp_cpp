#pragma once

#include <chrono>
#include <memory>
#include <string>

// Conditional compilation includes wrapped cleanly inside the header
#if defined(USE_ROS2_COMPILATION) || (defined(__has_include) && __has_include(<rclcpp/rclcpp.hpp>))
    #ifndef USE_ROS2_COMPILATION
        #define USE_ROS2_COMPILATION
    #endif
    #include <rclcpp/rclcpp.hpp>
#endif

namespace rpp {

class RppClock {
public:
    // Default constructor uses standard system clock (or default ROS clock if enabled)
    RppClock() {
    #ifdef USE_ROS2_COMPILATION
        // Fallback to a standard ROS clock instance type (ROS_SYSTEM_TIME by default)
        ros_clock_ = std::make_shared<rclcpp::Clock>(RCL_SYSTEM_TIME);
    #endif
    }

    #ifdef USE_ROS2_COMPILATION
    // Explicit constructor to hook into a specific ROS 2 Node's clock interface
    // This is critical for getting correct simulation time (use_sim_time)
    explicit RppClock(rclcpp::Clock::SharedPtr clock_ptr) : ros_clock_(clock_ptr) {}
    #endif

    ~RppClock() = default;

    // Returns current timestamp in seconds since epoch as a double
    double now_seconds() const {
    #ifdef USE_ROS2_COMPILATION
        if (ros_clock_) {
            return ros_clock_->now().seconds();
        }
        return 0.0;
    #else
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration<double>(duration).count();
    #endif
    }

    // Returns current timestamp in nanoseconds as an integer
    uint64_t now_nanoseconds() const {
    #ifdef USE_ROS2_COMPILATION
        if (ros_clock_) {
            return ros_clock_->now().nanoseconds();
        }
        return 0;
    #else
        auto now = std::chrono::system_clock::now();
        auto duration = now.time_since_epoch();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    #endif
    }

private:
#ifdef USE_ROS2_COMPILATION
    rclcpp::Clock::SharedPtr ros_clock_;
#endif
};

} // namespace rpp