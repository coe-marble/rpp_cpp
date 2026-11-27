#pragma once

#include <capnp/generated-header-support.h>
#include <capnp/message.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace rpp
{
// Content sizes exclude the root pointer. Hints include the entire first segment.
inline unsigned int message_segment_words(
  std::uint64_t content_words, std::uint64_t initial_segment_words = 0)
{
  constexpr auto maximum = std::numeric_limits<unsigned int>::max();
  if (content_words >= maximum || initial_segment_words > maximum) {
    throw std::length_error("Message exceeds Cap'n Proto segment capacity");
  }
  return static_cast<unsigned int>(std::max(content_words + 1, initial_segment_words));
}

template<typename Schema>
unsigned int struct_message_segment_words(std::uint64_t initial_segment_words = 0)
{
  return message_segment_words(capnp::sizeInWords<Schema>(), initial_segment_words);
}

template<typename Element>
unsigned int list_message_segment_words(std::size_t size)
{
  // A list pointer stores its element count in 29 bits.
  constexpr std::uint64_t maximum_elements = (std::uint64_t{1} << 29) - 1;
  if (size > maximum_elements) {
    throw std::length_error("List exceeds Cap'n Proto element capacity");
  }
  const auto count = static_cast<std::uint64_t>(size);
  constexpr auto word_bytes = sizeof(capnp::word);
  std::uint64_t content_words = 0;
  if constexpr (capnp::kind<Element>() == capnp::Kind::STRUCT) {
    // Inline composite lists include a tag before their fixed-size elements.
    content_words = 1 + count * capnp::sizeInWords<Element>();
  } else if constexpr (std::is_same<Element, capnp::Void>::value) {
    // Void elements require no payload storage.
  } else if constexpr (std::is_same<Element, bool>::value) {
    constexpr auto word_bits = word_bytes * 8;
    content_words = (count + word_bits - 1) / word_bits;
  } else if constexpr (capnp::kind<Element>() == capnp::Kind::PRIMITIVE) {
    content_words = (count * sizeof(Element) + word_bytes - 1) / word_bytes;
  } else if constexpr (capnp::kind<Element>() == capnp::Kind::ENUM) {
    content_words = (count * sizeof(std::uint16_t) + word_bytes - 1) / word_bytes;
  } else {
    // Pointer slots are fixed; their text/list/struct targets can grow later.
    content_words = count;
  }
  return message_segment_words(content_words);
}
}  // namespace rpp
