#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <stdexcept>

namespace rpp {

enum ClockType {
    RPP_CLOCK_REALTIME=0,
    RPP_CLOCK_MOCK=1
};

struct ClockOptions {
    ClockType type;
    double initial_time;

    ClockOptions(ClockType clock_type = ClockType::RPP_CLOCK_REALTIME, double init_time = 0.0)
        : type(clock_type), initial_time(init_time) {}

};

class ClockFactory;

class RppClock
{
public:
    RppClock() = default;
    virtual double now_seconds() const = 0;
    virtual uint64_t now_nanoseconds() const = 0;
};

class RppClockRealTime : public RppClock {
    friend class ClockFactory;
private:
    // Default constructor uses standard system clock (or default ROS clock if enabled)
    RppClockRealTime();
public:

    ~RppClockRealTime();

    // Returns current timestamp in seconds since epoch as a double
    virtual double now_seconds() const;

    // Returns current timestamp in nanoseconds as an integer
    virtual uint64_t now_nanoseconds() const;

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

class RppClockMock : public RppClock {
    friend class ClockFactory;

public:
    RppClockMock(double initial_time) : current_time_(initial_time) {}

    void set_time(double new_time) {
        current_time_ = new_time;
    }

    double now_seconds() const override {
        return current_time_;
    }

    double elapse(double dt) {
        current_time_ += dt;
        return current_time_;
    }

    uint64_t now_nanoseconds() const override {
        return static_cast<uint64_t>(current_time_ * 1e9);
    }

private:
    double current_time_;
};

class ClockFactory {
public:

    static std::shared_ptr<RppClock> create_clock(const ClockOptions& options)
    {
        switch (options.type) {
            case ClockType::RPP_CLOCK_REALTIME:
                return std::shared_ptr<RppClock>(new RppClockRealTime());
            case ClockType::RPP_CLOCK_MOCK:
                return std::shared_ptr<RppClock>(new RppClockMock(options.initial_time));
            default:
                throw std::invalid_argument("Invalid clock type specified.");
        }
    }
};

} // namespace rpp