#pragma once
#include <string>
#include <vector>
#include <map>
#include <variant>
#include <stdexcept>

namespace rpp::params {


struct ParameterValue
{
    using Object = std::map<std::string, ParameterValue>;
    using Array  = std::vector<ParameterValue>;

    std::variant<
        std::nullptr_t,
        bool,
        int64_t,
        double,
        std::string,
        Array,
        Object
    > value;

    ParameterValue() : value(nullptr) {}
    ParameterValue(bool b) : value(b) {}
    ParameterValue(int64_t i) : value(i) {}
    ParameterValue(int32_t i) : value(static_cast<int64_t>(i)) {}
    ParameterValue(int16_t i) : value(static_cast<int64_t>(i)) {}
    ParameterValue(int8_t i) : value(static_cast<int64_t>(i)) {}
    ParameterValue(float f) : value(static_cast<double>(f)) {}
    ParameterValue(double d) : value(d) {}
    ParameterValue(std::string s) : value(std::move(s)) {}
    ParameterValue(const char* s) : value(std::string(s)) {}
    ParameterValue(Array a) : value(std::move(a)) {}
    ParameterValue(Object o) : value(std::move(o)) {}

    template<typename T>
    T get() const;

};

struct ParameterReflectVisitor
{
    template<typename Name, typename Member>
    void operator()(Name&&, Member&&) const
    {
    }
};

template<typename T, typename = void>
struct has_reflect : std::false_type
{};

template<typename T>
struct has_reflect<
    T,
    std::void_t<
        decltype(
            std::declval<T&>().reflect(
                ParameterReflectVisitor{}
            )
        )
    >
> : std::true_type
{};

template<typename T, typename Enable = void>
struct ParameterConverter;

// -----------------------------------------------------------------------------
// Primitive specializations
// -----------------------------------------------------------------------------

template<>
struct ParameterConverter<int8_t>
{
    static ParameterValue to(int8_t value);
    static int8_t from(const ParameterValue& value);
};

template<>
struct ParameterConverter<int16_t>
{
    static ParameterValue to(int16_t value);
    static int16_t from(const ParameterValue& value);
};

template<>
struct ParameterConverter<int32_t>
{
    static ParameterValue to(int32_t value);
    static int32_t from(const ParameterValue& value);
};


template<>
struct ParameterConverter<int64_t>
{
    static ParameterValue to(int64_t value);
    static int64_t from(const ParameterValue& value);
};

template<>
struct ParameterConverter<float>
{
    static ParameterValue to(float value);
    static float from(const ParameterValue& value);
};

template<>
struct ParameterConverter<double>
{
    static ParameterValue to(double value);
    static double from(const ParameterValue& value);
};

template<>
struct ParameterConverter<bool>
{
    static ParameterValue to(bool value);
    static bool from(const ParameterValue& value);
};

template<>
struct ParameterConverter<std::string>
{
    static ParameterValue to(const std::string& value);
    static std::string from(const ParameterValue& value);
};


template<>
struct ParameterConverter<ParameterValue>
{
    static ParameterValue to(const ParameterValue& value);
    static ParameterValue from(const ParameterValue& value);
};

// -----------------------------------------------------------------------------
// std::vector
// -----------------------------------------------------------------------------

template<typename T>
struct ParameterConverter<std::vector<T>>
{
    static ParameterValue to(const std::vector<T>& value);
    static std::vector<T> from(const ParameterValue& value);
};

// -----------------------------------------------------------------------------
// std::map<std::string,T>
// -----------------------------------------------------------------------------

template<typename T>
struct ParameterConverter<std::map<std::string, T>>
{
    static ParameterValue to(const std::map<std::string, T>& value);
    static std::map<std::string, T> from(const ParameterValue& value);
};

// -----------------------------------------------------------------------------
// Reflectable structs
// -----------------------------------------------------------------------------


template<typename T>
struct ParameterConverter<
    T,
    std::enable_if_t<has_reflect<T>::value>
>
{
    static ParameterValue to(const T& value);
    static T from(const ParameterValue& value);
};

template<typename T>
inline T ParameterValue::get() const
{
    return ParameterConverter<T>::from(*this);
}

// -----------------------------------------------------------------------------
// ints
// -----------------------------------------------------------------------------

inline ParameterValue
ParameterConverter<int8_t>::to(int8_t value)
{
    return int64_t(value);
}

inline int8_t
ParameterConverter<int8_t>::from(const ParameterValue& value)
{
    if (!std::holds_alternative<int64_t>(value.value)) {
        throw std::runtime_error("Parameter value is not an int.");
    }
    return static_cast<int8_t>(
        std::get<int64_t>(value.value));
}

inline ParameterValue
ParameterConverter<int16_t>::to(int16_t value)
{
    return int64_t(value);
}

inline int16_t
ParameterConverter<int16_t>::from(const ParameterValue& value)
{
    if (!std::holds_alternative<int64_t>(value.value)) {
        throw std::runtime_error("Parameter value is not an int.");
    }
    return static_cast<int16_t>(
        std::get<int64_t>(value.value));
}

inline ParameterValue
ParameterConverter<int32_t>::to(int32_t value)
{
    return int64_t(value);
}

inline int32_t
ParameterConverter<int32_t>::from(const ParameterValue& value)
{
    if (!std::holds_alternative<int64_t>(value.value)) {
        throw std::runtime_error("Parameter value is not an int.");
    }
    return static_cast<int32_t>(
        std::get<int64_t>(value.value));
}

inline ParameterValue
ParameterConverter<int64_t>::to(int64_t value)
{
    return value;
}

inline int64_t
ParameterConverter<int64_t>::from(const ParameterValue& value)
{
    if (!std::holds_alternative<int64_t>(value.value)) {
        throw std::runtime_error("Parameter value is not an int.");
    }
    return std::get<int64_t>(value.value);
}

// -----------------------------------------------------------------------------
// floats
// -----------------------------------------------------------------------------

inline ParameterValue
ParameterConverter<float>::to(float value)
{
    return double(value);
}

inline float
ParameterConverter<float>::from(const ParameterValue& value)
{
    if (std::holds_alternative<int64_t>(value.value))
    {
        return static_cast<float>(
            std::get<int64_t>(value.value));
    }
    if (!std::holds_alternative<double>(value.value)) {
        throw std::runtime_error("Parameter value is not a float.");
    }
    return static_cast<float>(
        std::get<double>(value.value));
}


inline ParameterValue
ParameterConverter<double>::to(double value)
{
    return value;
}

inline double
ParameterConverter<double>::from(const ParameterValue& value)
{
    if (std::holds_alternative<int64_t>(value.value))
    {
        return static_cast<double>(
            std::get<int64_t>(value.value));
    }
    if (!std::holds_alternative<double>(value.value)) {
        throw std::runtime_error("Parameter value is not a double.");
    }
    return std::get<double>(value.value);
}

// -----------------------------------------------------------------------------
// bool
// -----------------------------------------------------------------------------

inline ParameterValue
ParameterConverter<bool>::to(bool value)
{
    return value;
}

inline bool
ParameterConverter<bool>::from(const ParameterValue& value)
{
    if (!std::holds_alternative<bool>(value.value)) {
        throw std::runtime_error("Parameter value is not a boolean.");
    }
    return std::get<bool>(value.value);
}

// -----------------------------------------------------------------------------
// string
// -----------------------------------------------------------------------------

inline ParameterValue
ParameterConverter<std::string>::to(
    const std::string& value)
{
    return value;
}

inline std::string
ParameterConverter<std::string>::from(
    const ParameterValue& value)
{
    return std::get<std::string>(value.value);
}


inline ParameterValue
ParameterConverter<ParameterValue>::to(
    const ParameterValue& value)
{
    return value;
}

inline ParameterValue
ParameterConverter<ParameterValue>::from(
    const ParameterValue& value)
{
    return value;
}

// -----------------------------------------------------------------------------
// std::vector
// -----------------------------------------------------------------------------

template<typename T>
ParameterValue
ParameterConverter<std::vector<T>>::to(
    const std::vector<T>& value)
{
    ParameterValue::Array array;

    for (const auto& x : value)
    {
        array.push_back(
            ParameterConverter<T>::to(x));
    }

    return array;
}

template<typename T>
std::vector<T>
ParameterConverter<std::vector<T>>::from(
    const ParameterValue& value)
{
    const auto& array =
        std::get<ParameterValue::Array>(value.value);

    std::vector<T> result;
    result.reserve(array.size());

    for (const auto& x : array)
    {
        result.push_back(
            ParameterConverter<T>::from(x));
    }

    return result;
}

// -----------------------------------------------------------------------------
// std::map<std::string, T>
// -----------------------------------------------------------------------------

template<typename T>
ParameterValue
ParameterConverter<std::map<std::string, T>>::to(
    const std::map<std::string, T>& value)
{
    ParameterValue::Object object;

    for (const auto& kv : value)
    {
        object[kv.first] =
            ParameterConverter<T>::to(kv.second);
    }

    return object;
}

template<typename T>
std::map<std::string, T>
ParameterConverter<std::map<std::string, T>>::from(
    const ParameterValue& value)
{
    const auto& object =
        std::get<ParameterValue::Object>(value.value);

    std::map<std::string, T> result;

    for (const auto& kv : object)
    {
        result[kv.first] =
            ParameterConverter<T>::from(kv.second);
    }

    return result;
}


// -----------------------------------------------------------------------------
// Reflectable structs
// -----------------------------------------------------------------------------

template<typename T>
ParameterValue
ParameterConverter<
    T,
    std::enable_if_t<has_reflect<T>::value>
>::to(const T& value)
{
    ParameterValue::Object object;

    T copy = value;

    copy.reflect(
        [&](auto name, auto& member)
        {
            using Member =
                std::remove_cv_t<
                std::remove_reference_t<
                decltype(member)>>;

            object[name] =
                ParameterConverter<Member>::to(member);
        });

    return object;
}

template<typename T>
T
ParameterConverter<
    T,
    std::enable_if_t<has_reflect<T>::value>
>::from(const ParameterValue& value)
{
    const auto& object =
        std::get<ParameterValue::Object>(value.value);

    T result{};

    result.reflect(
        [&](auto name, auto& member)
        {
            using Member =
                std::remove_cv_t<
                std::remove_reference_t<
                decltype(member)>>;

            auto it = object.find(name);

            if (it == object.end())
            {
                throw std::runtime_error(
                    "Field '" + std::string(name)
                    + "' not found in parameter object for struct '"
                    + T::struct_name() + "'.");
            }

            member =
                ParameterConverter<Member>::from(
                    it->second);
        });

    return result;
}

struct ParameterDescription
{
    std::string name;

    ParameterValue defaultValue;


    template<typename T>
    static ParameterDescription create(
        std::string name,
        T defaultValue)
    {
        return {
            std::move(name),
            ParameterConverter<T>::to(defaultValue)
        };
    }

    template<typename T>
    static ParameterDescription create(std::string name)
    {
        return {
            std::move(name),
            ParameterConverter<T>::to(T{})
        };
    }

};


using ParametersDescription = std::vector<ParameterDescription>;
using RppParameters_T = ParametersDescription;
using RppComponents_T = std::map<std::string, std::string>;


} // namespace rpp::params
