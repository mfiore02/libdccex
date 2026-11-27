#pragma once

#include <cstddef>
#include <limits>
#include <type_traits>

#include "dccex/string_view.hpp"

namespace dccex {

// One field of a DCC-EX message. The text views into the original message.
struct Field {
  StringView text;
  bool quoted = false;  // came from a "quoted string"; quotes are not in text

  // Exact, case-sensitive comparison of the field text.
  constexpr bool equals(StringView other) const noexcept { return text == other; }

  // Parses the field as a plain decimal integer into T: an optional '-' (signed
  // types only) followed by one or more digits, nothing else. No '+', no hex,
  // no whitespace. Fails if the value does not fit T or the field is quoted.
  // On failure, out is left unchanged.
  template <typename T>
  constexpr bool to_integer(T& out) const noexcept {
    static_assert(std::is_integral<T>::value && !std::is_same<T, bool>::value,
                  "to_integer needs a non-bool integral type");
    using U = typename std::make_unsigned<T>::type;

    if (quoted || text.empty()) return false;

    std::size_t i = 0;
    bool negative = false;
    if (text[0] == '-') {
      if (!std::is_signed<T>::value) return false;
      negative = true;
      i = 1;
    }
    if (i == text.size()) return false;  // "-" alone

    // Largest magnitude allowed: |min| for negatives, max otherwise.
    const U limit = negative
        ? static_cast<U>(static_cast<U>(std::numeric_limits<T>::max()) + 1u)
        : static_cast<U>(std::numeric_limits<T>::max());

    U magnitude = 0;
    for (; i < text.size(); ++i) {
      const char c = text[i];
      if (c < '0' || c > '9') return false;
      const U digit = static_cast<U>(c - '0');
      if (magnitude > static_cast<U>((limit - digit) / 10u)) return false;  // overflow
      magnitude = static_cast<U>(magnitude * 10u + digit);
    }

    if (!negative) {
      out = static_cast<T>(magnitude);
    } else if (magnitude == 0) {
      out = 0;
    } else {
      // -(magnitude) without overflowing when magnitude == |min|.
      out = static_cast<T>(-static_cast<T>(magnitude - 1u) - 1);
    }
    return true;
  }
};

// Splits one DCC-EX message (the text between '<' and '>') into an opcode and
// fields. Holds no storage of its own: fields are views into the message, which
// must outlive the tokenizer.
//
//   - The opcode is the first character. Fields start right after it, so in
//     "p1 MAIN" the opcode is 'p' and the fields are "1" and "MAIN".
//   - Fields are separated by runs of spaces, tabs, CR or LF.
//   - A field that starts with '"' runs to the next '"' and may contain
//     whitespace. Quotes elsewhere are ordinary characters. There are no escapes.
//   - An unterminated quote ends iteration and marks the message malformed.
//
// Other structure inside a field, such as "1|2|3" callback tags, is left to
// the decoder.
class Tokenizer {
public:
  explicit constexpr Tokenizer(StringView message) noexcept
      : message_(message), pos_(first_field_pos(message)) {}

  // First character of the message, or '\0' for an empty message.
  constexpr char opcode() const noexcept { return message_.empty() ? '\0' : message_[0]; }

  // Everything after the opcode with leading whitespace removed. For messages
  // that are free text rather than fields, such as diagnostics and the
  // version string.
  constexpr StringView rest() const noexcept {
    std::size_t start = first_field_pos(message_);
    while (start < message_.size() && is_space(message_[start])) ++start;
    return StringView(message_.data() + start, message_.size() - start);
  }

  // Advances to the next field. Returns false at the end of the message or at
  // an unterminated quote (see malformed()).
  constexpr bool next(Field& out) noexcept {
    while (pos_ < message_.size() && is_space(message_[pos_])) ++pos_;
    if (pos_ >= message_.size()) return false;

    if (message_[pos_] == '"') {
      const std::size_t open = pos_;
      std::size_t close = open + 1;
      while (close < message_.size() && message_[close] != '"') ++close;
      if (close >= message_.size()) {
        malformed_ = true;
        pos_ = message_.size();
        return false;
      }
      out.text = StringView(message_.data() + open + 1, close - open - 1);
      out.quoted = true;
      pos_ = close + 1;
      return true;
    }

    const std::size_t start = pos_;
    while (pos_ < message_.size() && !is_space(message_[pos_])) ++pos_;
    out.text = StringView(message_.data() + start, pos_ - start);
    out.quoted = false;
    return true;
  }

  // Restarts iteration at the first field.
  constexpr void rewind() noexcept { pos_ = first_field_pos(message_); }

  // Number of fields in the whole message, independent of the iteration
  // position. Stops counting at an unterminated quote.
  constexpr std::size_t count() const noexcept {
    Tokenizer scan(message_);
    Field f;
    std::size_t n = 0;
    while (scan.next(f)) ++n;
    return n;
  }

  // Field by zero-based index, independent of the iteration position.
  constexpr bool at(std::size_t index, Field& out) const noexcept {
    Tokenizer scan(message_);
    Field f;
    for (std::size_t i = 0; scan.next(f); ++i) {
      if (i == index) {
        out = f;
        return true;
      }
    }
    return false;
  }

  // True once iteration has hit an unterminated quote.
  constexpr bool malformed() const noexcept { return malformed_; }

private:
  static constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  }

  static constexpr std::size_t first_field_pos(StringView message) noexcept {
    return message.empty() ? 0 : 1;
  }

  StringView message_;
  std::size_t pos_ = 0;
  bool malformed_ = false;
};

}  // namespace dccex
