#pragma once

#include <cstdint>

#include "dccex/string_view.hpp"

namespace dccex {

// What a decoded message reports. Each value names the member of Event that
// holds its data.
enum class EventType : std::uint8_t {
  Unhandled,       // unhandled: not recognized, or recognized but malformed
  Loco,            // loco:      <l cab reg speedbyte functions>
  LocoForgotten,   // cab:       <- cab>
  PowerAll,        // power:     <p0> <p1>
  PowerTrack,      // power:     <p0 A> <p1 B> ...
  PowerRole,       // power:     <p1 MAIN> <p1 PROG> <p1 JOIN>
  Version,         // version:   <iDCC-EX V-x.y.z / ...>
  CvRead,          // cv:        <v cv value>
  CvWritten,       // cv:        <r cv value>
  AddressRead,     // address:   <r address>  <r LONG address UNSUPPORTED>
  AddressWritten,  // address:   <w address>
  Failed,          // (none):    <X>
  Diagnostic,      // text:      <* text *>
};

// Loco state from a broadcast. Speed is in the client's terms: 0 to 126,
// with emergency stop reported separately rather than as a speed.
struct LocoState {
  std::uint16_t cab;
  std::uint8_t speed;        // 0..126
  bool forward;
  bool emergency_stop;
  std::uint32_t functions;   // F0 in bit 0 .. F31 in bit 31
};

enum class Role : std::uint8_t { Main, Prog, Join };

struct Power {
  bool on;
  char track;   // 'A'..'H' for PowerTrack, '\0' otherwise
  Role role;    // meaningful for PowerRole only
};

struct Version {
  std::uint16_t major;
  std::uint16_t minor;
  std::uint16_t patch;
  StringView text;           // everything after the opcode, e.g. "DCC-EX V-5.6.6 / MEGA / ..."
};

// A CV result. A value of -1 means the station reported failure.
struct CvResult {
  std::uint16_t cv;
  std::int16_t value;
  constexpr bool ok() const noexcept { return value >= 0; }
};

// A loco address result. Zero or negative means the station reported failure.
struct AddressResult {
  std::int16_t address;
  bool long_unsupported;     // <r LONG n UNSUPPORTED>: decoder cannot hold a long address n
  constexpr bool ok() const noexcept { return address > 0 && !long_unsupported; }
};

// Why a message could not be decoded. The first two mean the library does
// not handle the message; the rest mean the message itself is bad.
enum class Reason : std::uint8_t {
  UnknownOpcode,      // opcode the library does not decode
  UnsupportedForm,    // valid reply the library does not support yet
  WrongFieldCount,    // too few or too many fields
  NotANumber,         // field should be a decimal integer (quoted numbers are text)
  OutOfRange,         // integer outside the protocol or DCC range
  UnknownKeyword,     // field should be one of a fixed set of words
  BadFormat,          // field has the wrong internal structure, e.g. a version not V-x.y.z
  UnterminatedQuote,  // a quoted field has no closing quote
};

struct Unhandled {
  static constexpr std::uint8_t kNoField = 0xFF;

  Reason reason;
  std::uint8_t field;        // zero-based index of the offending field, or kNoField

  // True when the message is bad, false when the library simply does not
  // handle it (UnknownOpcode, UnsupportedForm).
  constexpr bool malformed() const noexcept {
    return reason != Reason::UnknownOpcode && reason != Reason::UnsupportedForm;
  }
};

// One decoded message. Read the member named by `type`; the others are
// unspecified. `message` is always the full message text and, like every
// StringView here, points into the caller's buffer.
struct Event {
  EventType type;
  StringView message;
  union {
    Unhandled unhandled;
    LocoState loco;
    std::uint16_t cab;
    Power power;
    Version version;
    CvResult cv;
    AddressResult address;
    StringView text;
  };

  constexpr Event() noexcept
      : type(EventType::Unhandled), message(), unhandled{Reason::UnknownOpcode, Unhandled::kNoField} {}
};

// Decodes one message (the text between '<' and '>', as delivered by the
// Framer). Every message yields exactly one event; anything not recognized,
// or recognized with fields out of range, yields EventType::Unhandled.
Event decode(StringView message) noexcept;

// Fixed names for logging, e.g. "Loco" or "OutOfRange". Never null; "?" for a
// value outside the enumeration.
const char* name(EventType type) noexcept;
const char* name(Reason reason) noexcept;

}  // namespace dccex
