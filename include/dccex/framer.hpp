#pragma once

#include <cstddef>
#include <cstdint>

#include "dccex/string_view.hpp"

namespace dccex {

// Splits a byte stream into DCC-EX messages delimited by '<' and '>'.
//
// Feed it bytes exactly as they arrive from the transport, in chunks of any
// size. Each complete message is passed to the callback without its
// delimiters. Framing rules:
//
//   - Bytes outside a message (newlines, noise, a stray '>') are discarded.
//   - A '<' inside an open message abandons it and starts a new one.
//   - A message longer than MaxMessage is dropped whole; framing resumes at
//     the next '<'. An empty message "<>" is delivered as an empty view.
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
  // messages are discarded without being counted as dropped.
  void feed(const char* data, std::size_t len, Callback on_message, void* ctx) noexcept {
    for (std::size_t i = 0; i < len; ++i) {
      const char c = data[i];

      if (c == '<') {
        if (state_ == State::InMessage) count_drop();  // abandoned partial
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
            count_drop();
          }
          break;
      }
    }
  }

  // Messages discarded because they overflowed or were abandoned by a new '<'.
  // Saturates at the maximum value instead of wrapping.
  std::uint32_t dropped() const noexcept { return dropped_; }

  // Returns to the initial state: discards any partial message and clears
  // the dropped count.
  void reset() noexcept {
    state_ = State::Idle;
    length_ = 0;
    dropped_ = 0;
  }

private:
  enum class State : std::uint8_t { Idle, InMessage };

  void count_drop() noexcept {
    if (dropped_ != UINT32_MAX) ++dropped_;
  }

  char buffer_[MaxMessage];
  std::size_t length_ = 0;
  std::uint32_t dropped_ = 0;
  State state_ = State::Idle;
};

}  // namespace dccex
