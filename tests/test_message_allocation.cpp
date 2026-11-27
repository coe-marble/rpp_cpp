#include <rpp_cpp/list_definitions.hpp>
#include <rpp_schema/rpp_testing/AllocationEnvelope.hpp>
#include <rpp_schema/rpp_testing/AllocationFixedSample.hpp>
#include <rpp_schema/rpp_testing/AllocationHintedSample.hpp>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

// This executable is isolated from registry state. Observe
// real Cap'n Proto calloc() segments without replacing the wrapper or compiler.
#if defined(__GLIBC__)
extern "C" void * __libc_malloc(std::size_t) noexcept;
extern "C" void * __libc_calloc(std::size_t, std::size_t) noexcept;

namespace
{
struct Allocations
{
  std::size_t heap_calls{0}, segment_calls{0}, largest_segment_bytes{0};
};
thread_local bool recording = false;
thread_local Allocations allocations;

class ObserveAllocations
{
public:
  ObserveAllocations() {allocations = {}; recording = true;}
  ~ObserveAllocations() {recording = false;}
  Allocations stop() {recording = false; return allocations;}
};

void check(bool condition, const char * message)
{
  if (!condition) {throw std::runtime_error(message);}
}

void check_initial_segment(const Allocations & observed, std::size_t bytes)
{
  check(observed.segment_calls == 1, "Expected exactly one initial segment");
  check(observed.largest_segment_bytes == bytes, "Incorrect initial segment capacity");
}

template<typename Operation>
void expect_length_error(Operation && operation)
{
  try {
    operation();
  } catch (const std::length_error &) {
    return;
  }
  throw std::runtime_error("Expected length_error before allocating an oversized message");
}

using FixedSample = rpp_schema::rpp_testing::AllocationFixedSample;
using Envelope = rpp_schema::rpp_testing::AllocationEnvelope;
using WireSample = schema::rpp_testing::AllocationFixedSample;

void fixed_struct()
{
  ObserveAllocations construction;
  FixedSample sample;
  check_initial_segment(construction.stop(), 56);

  ObserveAllocations updates;
  for (unsigned int index = 0; index < 1000; ++index) {
    sample.a() = static_cast<double>(index);
    sample.f() = 7.0;
  }
  check(updates.stop().heap_calls == 0, "Scalar updates allocated after preparation");
  const auto view = sample.as_const();
  check(view.a() == 999.0 && view.f() == 7.0, "Scalar fields changed meaning");

  ObserveAllocations small_hint;
  FixedSample minimum(1);
  check_initial_segment(small_hint.stop(), 56);
}

void populate(Envelope & envelope, const std::string & name)
{
  envelope.name() = name;
  auto payload = envelope.payload();
  payload.resize(9);
  payload[8] = 42;
  auto values = envelope.values();
  values.resize(3);
  values[2] = 3.5;
  auto child = envelope.child();
  child.a() = 7.0;
  auto history = envelope.history();
  history.resize(2);
  auto last = history[1];
  last.f() = 9.0;
}

void dynamic_children()
{
  const std::string name(2048, 'x');
  ObserveAllocations hinted;
  Envelope envelope(1024);
  populate(envelope, name);
  check_initial_segment(hinted.stop(), 8192);

  const auto view = envelope.as_const();
  check(view.name() == name && view.payload()[8] == 42, "Dynamic fields were corrupted");
  check(view.values()[2] == 3.5 && view.child().a() == 7.0, "Nested fields were corrupted");
  check(view.history()[1].f() == 9.0, "Struct-list fields were corrupted");

  ObserveAllocations minimal;
  Envelope growing;
  // Five pointer fields and the root pointer; child contents are not fixed storage.
  check_initial_segment(minimal.stop(), 48);
  ObserveAllocations growth;
  populate(growing, name);
  check(growth.stop().segment_calls > 0, "Dynamic children could not grow the message");
  check(growing.as_const().name() == name, "Growing message lost child data");
}

void owned_copy()
{
  const auto copied = [] {
      capnp::MallocMessageBuilder source;
      auto future = source.initRoot<schema::rpp_testing::AllocationEvolvedSample>();
      future.setA(2.0);
      future.setName(std::string(4096, 'n'));
      future.initValues(3).set(2, 42.0);

      // View the newer message through the old schema, without losing its wire fields.
      capnp::SegmentArrayMessageReader old_view(source.getSegmentsForOutput());
      const auto reader = old_view.getRoot<WireSample>();
      const auto bytes = (reader.totalSize().wordCount + 1) * sizeof(capnp::word);
      ObserveAllocations copying;
      auto result = FixedSample::Const::from_wire(reader);
      check_initial_segment(copying.stop(), bytes);
      return result;
    }();

  check(copied.a() == 2.0, "Owned copy depended on the destroyed source");
  capnp::MallocMessageBuilder roundtrip;
  roundtrip.setRoot(static_cast<WireSample::Reader>(copied));
  auto restored = roundtrip.getRoot<schema::rpp_testing::AllocationEvolvedSample>();
  check(restored.getName().size() == 4096, "Owned copy discarded unknown text fields");
  check(restored.getValues()[2] == 42.0, "Owned copy discarded unknown list fields");
}

void borrowed_views()
{
  auto owner = std::make_shared<capnp::MallocMessageBuilder>();
  auto parent = owner->initRoot<schema::rpp_testing::AllocationEnvelope>();
  parent.initChild().setA(1.0);
  parent.initValues(2).set(1, 3.5);

  ObserveAllocations borrowing;
  Envelope wrapper(parent);
  auto child = wrapper.child();
  child.a() = 7.0;
  auto values = wrapper.values();
  values[1] = 9.0;
  check(borrowing.stop().segment_calls == 0, "Borrowed wrappers allocated new messages");
  check(parent.getChild().getA() == 7.0, "Child wrapper copied its parent data");
  check(parent.getValues()[1] == 9.0, "List wrapper copied its parent data");

  auto retained = Envelope::Const(parent.asReader(), owner).child();
  owner.reset();
  check(retained.a() == 7.0, "Nested const view did not retain its supplied owner");

  std::array<capnp::word, 7> storage{};
  capnp::MallocMessageBuilder stack_message(kj::arrayPtr(storage.data(), storage.size()));
  ObserveAllocations stack;
  rpp::List<double> stack_values(stack_message.initRoot<capnp::List<double>>(6),
    [&](std::size_t size) {return stack_message.initRoot<capnp::List<double>>(size);});
  stack_values[5] = 3.5;
  check(stack.stop().segment_calls == 0, "Caller-supplied storage allocated a heap segment");
  const auto segments = stack_message.getSegmentsForOutput();
  check(segments.size() == 1 && segments[0].begin() == storage.data(),
    "Borrowed list did not use caller-supplied storage");
}

void numeric_lists()
{
  ObserveAllocations empty;
  rpp::List<double> initially_empty;
  rpp::Data empty_data;
  check(empty.stop().heap_calls == 0, "Empty wrappers eagerly allocated builders");

  ObserveAllocations doubles;
  initially_empty.resize(6);
  check_initial_segment(doubles.stop(), 56);
  ObserveAllocations updates;
  for (unsigned int index = 0; index < 1000; ++index) {
    initially_empty[5] = 3.5;
  }
  check(updates.stop().heap_calls == 0, "Numeric list updates allocated");

  ObserveAllocations bits;
  rpp::List<bool> flags(65);
  check_initial_segment(bits.stop(), 24);
  flags[64] = true;
  check(static_cast<bool>(flags[64]), "Packed Boolean boundary was corrupted");

  ObserveAllocations bytes;
  rpp::List<std::uint8_t> octets(9);
  check_initial_segment(bytes.stop(), 24);
  octets[8] = 42;
  check(static_cast<std::uint8_t>(octets[8]) == 42, "Byte-list boundary was corrupted");

  ObserveAllocations structs;
  rpp::List<FixedSample> samples(2);
  check_initial_segment(structs.stop(), 112);
  auto last = samples[1];
  last.f() = 7.0;
  const rpp::ListConst<FixedSample> reader = samples;
  check(reader[1].f() == 7.0, "Inline-composite list layout was corrupted");
}

void owned_blobs()
{
  const auto bytes = [] {
      ObserveAllocations allocation;
      rpp::Data payload(9);
      check_initial_segment(allocation.stop(), 24);
      payload[8] = 42;
      return static_cast<rpp::DataConst>(payload);
    }();
  check(bytes[8] == 42, "Data view did not retain its owning message");

  const auto strings = [] {
      ObserveAllocations allocation;
      rpp::List<std::string> values(2);
      check_initial_segment(allocation.stop(), 24);
      values[0] = std::string("first");
      values[1] = std::string("second");
      return static_cast<rpp::ListConst<std::string>>(values);
    }();
  check(strings[0] == "first" && strings[1] == "second", "Text list lost its owning message");

  const auto retained = [] {
      rpp::List<double> values(6);
      values[5] = 3.5;
      const rpp::ListConst<double> view = values;
      auto moved = std::move(values);
      moved[0] = 7.0;
      check(view[0] == 7.0, "List move copied its storage");
      return view;
    }();
  check(retained[5] == 3.5, "Numeric list view did not retain its owning message");
}

void bounds()
{
  expect_length_error([] {rpp::message_segment_words(std::numeric_limits<std::uint64_t>::max());});
  expect_length_error([] {FixedSample sample(std::numeric_limits<std::uint64_t>::max());});
  expect_length_error([] {rpp::List<double> values(std::size_t{1} << 29);});
  expect_length_error([] {rpp::Data values(std::size_t{1} << 29);});
}

void annotated_hints()
{
  const std::string name(15, 'x');
  ObserveAllocations construction;
  rpp_schema::rpp_testing::AllocationHintedSample sample;
  // Root + five pointer slots + hinted payloads: 31 words in total.
  check_initial_segment(construction.stop(), 248);

  const auto initial = sample.as_const();
  check(initial.values().empty() && initial.flags().empty() && initial.history().empty(),
    "Capacity hints changed default list lengths");
  check(initial.name().empty() && initial.payload().empty(),
    "Capacity hints changed default blob values");

  ObserveAllocations filling;
  sample.name() = name;
  auto values = sample.values();
  values.resize(6);
  values[5] = 3.5;
  auto payload = sample.payload();
  payload.resize(9);
  payload[8] = 42;
  auto flags = sample.flags();
  flags.resize(65);
  flags[64] = true;
  auto history = sample.history();
  history.resize(2);
  auto last = history[1];
  last.f() = 7.0;
  check(filling.stop().segment_calls == 0, "Annotated payloads exceeded the prepared segment");
  check(sample.as_const().history()[1].f() == 7.0, "Annotated struct-list data was corrupted");

  ObserveAllocations growth;
  values.resize(100);
  check(growth.stop().segment_calls > 0, "hintSize unexpectedly limited field growth");
  check(sample.as_const().values().size() == 100, "A list could not exceed its allocation hint");
}
}  // namespace

extern "C" void * malloc(std::size_t size) noexcept
{
  if (recording) {++allocations.heap_calls;}
  return __libc_malloc(size);
}

extern "C" void * calloc(std::size_t count, std::size_t size) noexcept
{
  if (recording) {
    ++allocations.heap_calls;
    ++allocations.segment_calls;
    const auto bytes = count * size;
    if (bytes > allocations.largest_segment_bytes) {allocations.largest_segment_bytes = bytes;}
  }
  return __libc_calloc(count, size);
}

TEST(MessageAllocation, FixedStructUsesItsKnownSize)
{
  fixed_struct();
}

TEST(MessageAllocation, DynamicChildrenHonorHintsAndCanGrow)
{
  dynamic_children();
}

TEST(MessageAllocation, OwnedCopiesPreserveUnknownFields)
{
  owned_copy();
}

TEST(MessageAllocation, BorrowedViewsUseTheirOwnersStorage)
{
  borrowed_views();
}

TEST(MessageAllocation, NumericListsUsePackedWireSizes)
{
  numeric_lists();
}

TEST(MessageAllocation, OwnedViewsRetainDataAcrossMovesAndDestruction)
{
  owned_blobs();
}

TEST(MessageAllocation, OversizedRequestsFailBeforeAllocating)
{
  bounds();
}

TEST(MessageAllocation, FieldHintsReserveCapacityWithoutChangingDefaults)
{
  annotated_hints();
}

#else
TEST(MessageAllocation, AllocatorObservationRequiresGnuLibc)
{
  GTEST_SKIP() << "Allocation observer uses GNU libc allocator entry points";
}
#endif
