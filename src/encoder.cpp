#include "dccex/encoder.hpp"

#include <cstdint>

namespace dccex {

const char* name(EncodeError error) noexcept {
  switch (error) {
    case EncodeError::None: return "None";
    case EncodeError::BufferTooSmall: return "BufferTooSmall";
    case EncodeError::OutOfRange: return "OutOfRange";
  }
  return "?";
}

namespace encode {
namespace {

// DCC ranges enforced on arguments.
constexpr std::uint16_t kMaxCab = 10239;
constexpr std::uint16_t kMaxCv = 1024;
constexpr std::uint8_t kMaxSpeed = 126;
constexpr std::uint16_t kMaxFunction = 32767;

constexpr bool valid_cab(std::uint16_t cab) noexcept { return cab >= 1 && cab <= kMaxCab; }
constexpr bool valid_cv(std::uint16_t cv) noexcept { return cv >= 1 && cv <= kMaxCv; }

// Builds a command in scratch space, then copies it to the caller's buffer
// only if it fits, so a failed call never leaves a partial command behind.
class Command {
public:
  explicit Command(char opcode) noexcept { put('<'); put(opcode); }

  Command& word(const char* text) noexcept {
    put(' ');
    while (*text != '\0') put(*text++);
    return *this;
  }

  Command& number(std::int32_t value) noexcept {
    put(' ');
    std::uint32_t magnitude;
    if (value < 0) {
      put('-');
      magnitude = static_cast<std::uint32_t>(-(value + 1)) + 1u;
    } else {
      magnitude = static_cast<std::uint32_t>(value);
    }
    char digits[10];
    std::size_t n = 0;
    do {
      digits[n++] = static_cast<char>('0' + magnitude % 10u);
      magnitude /= 10u;
    } while (magnitude != 0);
    while (n > 0) put(digits[--n]);
    return *this;
  }

  // Appends "1" or "0".
  Command& flag(bool on) noexcept { return number(on ? 1 : 0); }

  // Finishes the command and copies it to `out`.
  Encoded finish(Buffer out) noexcept {
    put('>');
    put('\n');
    if (length_ > out.capacity) return {0, EncodeError::BufferTooSmall};
    for (std::size_t i = 0; i < length_; ++i) out.data[i] = scratch_[i];
    return {length_, EncodeError::None};
  }

private:
  // The longest command is well under kMaxCommandSize; anything past the end
  // is dropped rather than written out of bounds.
  void put(char c) noexcept {
    if (length_ < sizeof scratch_) scratch_[length_++] = c;
  }

  char scratch_[kMaxCommandSize];
  std::size_t length_ = 0;
};

constexpr Encoded out_of_range() noexcept { return {0, EncodeError::OutOfRange}; }

}  // namespace

Encoded power(Buffer out, PowerTarget target, bool on) noexcept {
  Command c(on ? '1' : '0');
  switch (target) {
    case PowerTarget::All: break;
    case PowerTarget::Main: c.word("MAIN"); break;
    case PowerTarget::Prog: c.word("PROG"); break;
    default: return out_of_range();
  }
  return c.finish(out);
}

Encoded join_prog_to_main(Buffer out) noexcept { return Command('1').word("JOIN").finish(out); }

Encoded status(Buffer out) noexcept { return Command('s').finish(out); }

Encoded throttle(Buffer out, std::uint16_t cab, std::uint8_t speed, bool forward) noexcept {
  if (!valid_cab(cab) || speed > kMaxSpeed) return out_of_range();
  return Command('t').number(cab).number(speed).flag(forward).finish(out);
}

Encoded emergency_stop(Buffer out, std::uint16_t cab, bool forward) noexcept {
  if (!valid_cab(cab)) return out_of_range();
  return Command('t').number(cab).number(-1).flag(forward).finish(out);
}

Encoded emergency_stop_all(Buffer out) noexcept { return Command('!').finish(out); }

Encoded query_loco(Buffer out, std::uint16_t cab) noexcept {
  if (!valid_cab(cab)) return out_of_range();
  return Command('t').number(cab).finish(out);
}

Encoded function(Buffer out, std::uint16_t cab, std::uint16_t fn, bool on) noexcept {
  if (!valid_cab(cab) || fn > kMaxFunction) return out_of_range();
  return Command('F').number(cab).number(fn).flag(on).finish(out);
}

Encoded forget_loco(Buffer out, std::uint16_t cab) noexcept {
  if (!valid_cab(cab)) return out_of_range();
  return Command('-').number(cab).finish(out);
}

Encoded forget_all_locos(Buffer out) noexcept { return Command('-').number(0).finish(out); }

Encoded read_cv(Buffer out, std::uint16_t cv) noexcept {
  if (!valid_cv(cv)) return out_of_range();
  return Command('R').number(cv).finish(out);
}

Encoded read_address(Buffer out) noexcept { return Command('R').finish(out); }

Encoded write_cv(Buffer out, std::uint16_t cv, std::uint8_t value) noexcept {
  if (!valid_cv(cv)) return out_of_range();
  return Command('W').number(cv).number(value).finish(out);
}

Encoded write_address(Buffer out, std::uint16_t address) noexcept {
  if (!valid_cab(address)) return out_of_range();
  return Command('W').number(address).finish(out);
}

Encoded write_cv_main(Buffer out, std::uint16_t cab, std::uint16_t cv, std::uint8_t value) noexcept {
  if (!valid_cab(cab) || !valid_cv(cv)) return out_of_range();
  return Command('w').number(cab).number(cv).number(value).finish(out);
}

}  // namespace encode
}  // namespace dccex
