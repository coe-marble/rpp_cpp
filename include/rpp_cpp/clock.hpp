#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

#include <memory>
#include <stdexcept>

namespace rpp
{

enum ClockType
{
  RPP_CLOCK_REALTIME=0,
  RPP_CLOCK_MOCK=1
};

struct ClockOptions
{
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
  // Canonical clock timestamps are unsigned 64-bit nanoseconds.
  virtual std::uint64_t now_nanoseconds() const = 0;
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
  virtual std::uint64_t now_nanoseconds() const;

private:
  struct Impl;
  std::unique_ptr<Impl> pimpl_;
};

class RppClockMock : public RppClock {
  friend class ClockFactory;

public:
  RppClockMock(double initial_time)
  {set_time(initial_time);}

  void set_time(double new_time)
  {
    (void)to_nanoseconds(new_time);
    current_time_ = new_time;
  }

  double now_seconds() const override
  {
    return current_time_;
  }

  double elapse(double dt)
  {
    set_time(current_time_ + dt);
    return current_time_;
  }

  std::uint64_t now_nanoseconds() const override
  {
    return to_nanoseconds(current_time_);
  }

private:
  static std::uint64_t to_nanoseconds(double seconds)
  {
    const double nanoseconds = seconds * 1e9;
    // The double upper bound rounds to 2^64, which must be excluded before conversion.
    if (!std::isfinite(nanoseconds) || nanoseconds < 0.0 ||
      nanoseconds >= static_cast<double>(std::numeric_limits<std::uint64_t>::max()))
    {
      throw std::out_of_range("Clock time must fit unsigned 64-bit nanoseconds");
    }
    return static_cast<std::uint64_t>(nanoseconds);
  }

  double current_time_;
};

class ClockFactory {
public:
  static std::shared_ptr<RppClock> create_clock(const ClockOptions & options)
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
