#include "rpp_cpp/clock.hpp"

// Conditional compilation includes wrapped cleanly inside the header
#if defined(USE_ROS2_COMPILATION) || (defined(__has_include) && __has_include(<rclcpp/rclcpp.hpp>))
    #ifndef USE_ROS2_COMPILATION
        #define USE_ROS2_COMPILATION
    #endif
    #include <rclcpp/rclcpp.hpp>
#endif

namespace rpp {


struct RppClockRealTime::Impl {
#ifdef USE_ROS2_COMPILATION
    rclcpp::Clock::SharedPtr ros_clock;
#endif
};

RppClockRealTime::RppClockRealTime()
    : pimpl_(std::make_unique<Impl>())
{

    #ifdef USE_ROS2_COMPILATION
    // Fallback to a standard ROS clock instance type (ROS_SYSTEM_TIME by default)
    pimpl_->ros_clock = std::make_shared<rclcpp::Clock>(RCL_SYSTEM_TIME);
    #endif
}

RppClockRealTime::~RppClockRealTime() = default;

    // Returns current timestamp in seconds since epoch as a double
double RppClockRealTime::now_seconds() const
{
#ifdef USE_ROS2_COMPILATION
    if (pimpl_->ros_clock) {
        return pimpl_->ros_clock->now().seconds();
    }
    return 0.0;
#else
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration<double>(duration).count();
#endif
}

    // Returns current timestamp in nanoseconds as an integer
uint64_t RppClockRealTime::now_nanoseconds() const
{
#ifdef USE_ROS2_COMPILATION
    if (pimpl_->ros_clock) {
        return pimpl_->ros_clock->now().nanoseconds();
    }
    return 0;
#else
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
#endif
}

} // namespace rpp