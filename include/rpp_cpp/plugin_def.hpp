#pragma once

namespace rpp {

class ComponentContext;

class Plugin {
public:
    virtual ~Plugin() = default;

    virtual void initialize(const ComponentContext& context) = 0;
    virtual void reset() {};
};



}  // namespace rpp