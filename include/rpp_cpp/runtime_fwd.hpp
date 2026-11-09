#pragma once

namespace rpp
{
struct SingleThreaded;
struct Concurrent;
class ComponentContext;
class SubcomponentContext;

template<bool Concurrent, bool IsScript> class BasicRuntime;
template<class Threading = SingleThreaded> class BasicSlotManager;
template<class Threading = SingleThreaded> class BasicComponentRuntimeBuilder;

/// Component lifecycle and administration through its owning slot.
template<bool Concurrent = false>
using ComponentRuntime = BasicRuntime<Concurrent, false>;

/// Script services and slot collection, without a root component or owning slot.
template<bool Concurrent = false>
using ScriptRuntime = BasicRuntime<Concurrent, true>;

using SlotManager = BasicSlotManager<SingleThreaded>;
using ConcurrentSlotManager = BasicSlotManager<Concurrent>;
using ComponentRuntimeBuilder = BasicComponentRuntimeBuilder<SingleThreaded>;
using ConcurrentComponentRuntimeBuilder = BasicComponentRuntimeBuilder<Concurrent>;
}  // namespace rpp
