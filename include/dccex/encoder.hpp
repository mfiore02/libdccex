#pragma once

#include <cstddef>
#include <cstdint>

namespace dccex {

// Caller-provided output buffer. Converts implicitly from a char array, so
// `char buf[kMaxCommandSize]; encode::status(buf);` works.
struct Buffer {
  char* data;
  std::size_t capacity;

  constexpr Buffer(char* d, std::size_t c) noexcept : data(d), capacity(c) {}
  template <std::size_t N>
  constexpr Buffer(char (&array)[N]) noexcept : data(array), capacity(N) {}  // NOLINT
};

// A buffer of this size holds any command the library builds.
constexpr std::size_t kMaxCommandSize = 32;

enum class EncodeError : std::uint8_t {
  None,
  BufferTooSmall,   // the command does not fit; nothing was written
  OutOfRange,       // an argument is outside its protocol range; nothing was written
};

struct Encoded {
  std::size_t length;   // bytes written, including the trailing '\n'; 0 on error
  EncodeError error;
  constexpr bool ok() const noexcept { return error == EncodeError::None; }
};

// Fixed name for logging, e.g. "OutOfRange". "?" for an invalid value.
const char* name(EncodeError error) noexcept;

enum class PowerTarget : std::uint8_t { All, Main, Prog };

// Each function writes one complete command, "<...>\n", at the start of `out`
// and returns its length. No terminating '\0' is written. On error nothing is
// written and the result says why. Arguments are checked against DCC ranges:
// loco addresses 1..10239, CVs 1..1024, CV values 0..255, speed 0..126,
// functions 0..32767.
namespace encode {

// <1> <0> <1 MAIN> <0 PROG> ...
Encoded power(Buffer out, PowerTarget target, bool on) noexcept;

// <1 JOIN>: drive the programming track as part of the main. Turning any
// power off ends the join.
Encoded join_prog_to_main(Buffer out) noexcept;

// <s>: version, then power, turnout and sensor states.
Encoded status(Buffer out) noexcept;

// <t cab speed dir>
Encoded throttle(Buffer out, std::uint16_t cab, std::uint8_t speed, bool forward) noexcept;

// <t cab -1 dir>: emergency stop one loco.
Encoded emergency_stop(Buffer out, std::uint16_t cab, bool forward) noexcept;

// <!>: emergency stop every loco.
Encoded emergency_stop_all(Buffer out) noexcept;

// <t cab>: ask for a loco broadcast.
Encoded query_loco(Buffer out, std::uint16_t cab) noexcept;

// <F cab fn state>
Encoded function(Buffer out, std::uint16_t cab, std::uint16_t fn, bool on) noexcept;

// <- cab> / <- 0>: release the station's slot for one loco, or for all.
Encoded forget_loco(Buffer out, std::uint16_t cab) noexcept;
Encoded forget_all_locos(Buffer out) noexcept;

// Programming track. Replies arrive later as CvRead, AddressRead, CvWritten
// and AddressWritten events. Only one may be pending at a time.
Encoded read_cv(Buffer out, std::uint16_t cv) noexcept;                        // <R cv>
Encoded read_address(Buffer out) noexcept;                                     // <R>
Encoded write_cv(Buffer out, std::uint16_t cv, std::uint8_t value) noexcept;   // <W cv value>
Encoded write_address(Buffer out, std::uint16_t address) noexcept;             // <W address>

// <w cab cv value>: write a CV on the main track. There is no reply.
Encoded write_cv_main(Buffer out, std::uint16_t cab, std::uint16_t cv, std::uint8_t value) noexcept;

}  // namespace encode
}  // namespace dccex
