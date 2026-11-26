#pragma once

namespace rpp
{
struct SingleThreaded;
struct Concurrent;
class ComponentContext;
class SubcomponentContext;

template<bool Concurrent, bool IsScript>
class BasicRuntime;
template<class Threading = SingleThreaded>
class BasicSlotManager;
template<class Threading = SingleThreaded>
class BasicComponentRuntimeBuilder;
template<class Threading = SingleThreaded>
class BasicRppServerHost;

/// Component lifecycle and administration through its owning slot.
template<bool Concurrent = false>
using BasicComponentRuntime = BasicRuntime<Concurrent, false>;

using ComponentRuntime = BasicComponentRuntime<false>;
using ConcurrentComponentRuntime = BasicComponentRuntime<true>;

/// Script services and slot collection, without a root component or owning slot.
template<bool Concurrent = false>
using BasicScriptRuntime = BasicRuntime<Concurrent, true>;

using ScriptRuntime = BasicScriptRuntime<false>;
using ConcurrentScriptRuntime = BasicScriptRuntime<true>;

using SlotManager = BasicSlotManager<SingleThreaded>;
using ConcurrentSlotManager = BasicSlotManager<Concurrent>;
using ComponentRuntimeBuilder = BasicComponentRuntimeBuilder<SingleThreaded>;
using ConcurrentComponentRuntimeBuilder = BasicComponentRuntimeBuilder<Concurrent>;
using RppServerHost = BasicRppServerHost<SingleThreaded>;
using ConcurrentRppServerHost = BasicRppServerHost<Concurrent>;
}  // namespace rpp
