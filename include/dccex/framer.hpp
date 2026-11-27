#pragma once

#include <cstddef>
#include <cstdint>

#include "dccex/string_view.hpp"

namespace dccex {

// Splits a byte stream into DCC-EX messages delimited by '<' and '>'.
//
// Feed it bytes exactly as they arrive from the transport, in chunks of any
// size: a message may span several chunks, and one chunk may hold many
// messages. Each complete message of up to MaxMessage bytes is passed to the
// callback without its delimiters. Framing rules:
//
//   - Bytes outside a message (newlines, noise, a stray '>') are discarded.
//   - A '<' inside an open message abandons it and starts a new one
//     (counted by abandoned()).
//   - A message longer than MaxMessage is dropped whole (counted by
//     overflowed()); framing resumes at the next '<'.
//   - An empty message "<>" is delivered as an empty view.
//
// The StringView passed to the callback points into the framer's internal
// buffer and is valid only until the callback returns. The callback must not
// call feed() or reset() on the same framer.
//
// No allocation, no exceptions; not thread-safe. Storage is MaxMessage bytes.
template <std::size_t MaxMessage = 256>
class Framer {
  static_assert(MaxMessage > 0, "Framer capacity must be at least 1");

public:
  using Callback = void (*)(void* ctx, StringView message);

  // Largest message content, in bytes, that will be delivered.
  static constexpr std::size_t capacity() noexcept { return MaxMessage; }

  // Consumes all len bytes. on_message may be null, in which case complete
  // messages are discarded without being counted by either counter.
  void feed(const char* data, std::size_t len, Callback on_message, void* ctx) noexcept {
    for (std::size_t i = 0; i < len; ++i) {
      const char c = data[i];

      if (c == '<') {
        if (state_ == State::InMessage) increment(abandoned_);
        state_ = State::InMessage;
        length_ = 0;
        continue;
      }

      switch (state_) {
        case State::Idle:
          break;

        case State::InMessage:
          if (c == '>') {
            state_ = State::Idle;
            if (on_message != nullptr) on_message(ctx, StringView(buffer_, length_));
          } else if (length_ < MaxMessage) {
            buffer_[length_++] = c;
          } else {
            // Too long: drop it. Idle discards the rest up to the next '<'.
            state_ = State::Idle;
            increment(overflowed_);
          }
          break;
      }
    }
  }

  // Messages dropped because they were longer than MaxMessage. A rising count
  // suggests the buffer is too small for the traffic. Saturates, never wraps.
  std::uint32_t overflowed() const noexcept { return overflowed_; }

  // Partial messages abandoned because a new '<' arrived before their '>'.
  // A rising count suggests bytes are being lost or mangled in transit.
  // Saturates, never wraps.
  std::uint32_t abandoned() const noexcept { return abandoned_; }

  // Returns to the initial state: discards any partial message and clears
  // both counters.
  void reset() noexcept {
    state_ = State::Idle;
    length_ = 0;
    overflowed_ = 0;
    abandoned_ = 0;
  }

private:
  enum class State : std::uint8_t { Idle, InMessage };

  static void increment(std::uint32_t& counter) noexcept {
    if (counter != UINT32_MAX) ++counter;
  }

  char buffer_[MaxMessage];
  std::size_t length_ = 0;
  std::uint32_t overflowed_ = 0;
  std::uint32_t abandoned_ = 0;
  State state_ = State::Idle;
};

}  // namespace dccex
