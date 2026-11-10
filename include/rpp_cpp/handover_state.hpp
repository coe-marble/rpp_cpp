#pragma once

#include <memory>
#include <stdexcept>
#include <type_traits>
#include <typeinfo>
#include <utility>

namespace rpp
{

/// Move-only type erasure for a lifecycle handover payload.
/// A handover has one destination, so it intentionally never copies its contained state.
class HandoverState final
{
  struct Value
  {
    virtual ~Value() = default;
    virtual const std::type_info & type() const noexcept = 0;
  };

  template<typename T>
  struct TypedValue final : Value
  {
    explicit TypedValue(T && value)
    : value(std::move(value)) {}

    const std::type_info & type() const noexcept override {return typeid(T);}
    T value;
  };

  std::unique_ptr<Value> value_;

public:
  HandoverState() = default;
  HandoverState(HandoverState &&) noexcept = default;
  HandoverState & operator=(HandoverState &&) noexcept = default;
  HandoverState(const HandoverState &) = delete;
  HandoverState & operator=(const HandoverState &) = delete;

  template<typename T,
    typename ValueType = std::decay_t<T>,
    std::enable_if_t<!std::is_same_v<ValueType, HandoverState>, int> = 0>
  HandoverState(T && value)
  : value_(std::make_unique<TypedValue<ValueType>>(std::forward<T>(value))) {}

  bool has_value() const noexcept {return static_cast<bool>(value_);}
  explicit operator bool() const noexcept {return has_value();}
  const std::type_info & type() const noexcept {return value_ ? value_->type() : typeid(void);}

  template<typename T>
  bool holds() const noexcept {return type() == typeid(T);}

  template<typename T>
  const T & get() const
  {
    if (!holds<T>()) {throw std::bad_cast();}
    return static_cast<const TypedValue<T> &>(*value_).value;
  }

  template<typename T>
  T take() &&
  {
    if (!holds<T>()) {throw std::bad_cast();}
    auto typed = static_cast<TypedValue<T> *>(value_.get());
    T result = std::move(typed->value);
    value_.reset();
    return result;
  }
};

}  // namespace rpp
