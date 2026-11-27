#include "dccex/decoder.hpp"

#include <cstdint>

#include "dccex/tokenizer.hpp"

namespace dccex {
namespace {

// DCC ranges enforced on decoded values.
constexpr std::uint16_t kMaxCab = 10239;
constexpr std::uint16_t kMaxCv = 1024;

// Result of decoding one opcode: either an event or a reason it is unhandled.
enum class Outcome : std::uint8_t { Decoded, Unrecognized, Malformed };

// Parses field `index` as an integer in [lo, hi].
template <typename T>
bool int_field(const Tokenizer& t, std::size_t index, T lo, T hi, T& out) noexcept {
  Field f;
  T value{};
  if (!t.at(index, f) || !f.to_integer(value)) return false;
  if (value < lo || value > hi) return false;
  out = value;
  return true;
}

bool keyword_field(const Tokenizer& t, std::size_t index, StringView keyword) noexcept {
  Field f;
  return t.at(index, f) && !f.quoted && f.equals(keyword);
}

// <l cab reg speedbyte functions>
Outcome decode_loco(const Tokenizer& t, Event& e) noexcept {
  std::uint16_t cab = 0;
  std::int32_t reg = 0;
  std::uint8_t speed_byte = 0;
  std::uint32_t functions = 0;
  if (t.count() != 4 ||
      !int_field<std::uint16_t>(t, 0, 1, kMaxCab, cab) ||
      !int_field<std::int32_t>(t, 1, INT32_MIN, INT32_MAX, reg) ||  // legacy, ignored
      !int_field<std::uint8_t>(t, 2, 0, 255, speed_byte) ||
      !int_field<std::uint32_t>(t, 3, 0, UINT32_MAX, functions)) {
    return Outcome::Malformed;
  }

  // Bit 7: direction. Bits 0-6: DCC speed code, 0 stop, 1 emergency stop,
  // 2..127 running, which is client speed + 1.
  const std::uint8_t code = speed_byte & 0x7F;
  e.type = EventType::Loco;
  e.loco.cab = cab;
  e.loco.forward = (speed_byte & 0x80) != 0;
  e.loco.emergency_stop = code == 1;
  e.loco.speed = code >= 2 ? static_cast<std::uint8_t>(code - 1) : 0;
  e.loco.functions = functions;
  return Outcome::Decoded;
}

// <- cab>
Outcome decode_forget(const Tokenizer& t, Event& e) noexcept {
  std::uint16_t cab = 0;
  if (t.count() != 1 || !int_field<std::uint16_t>(t, 0, 1, kMaxCab, cab)) return Outcome::Malformed;
  e.type = EventType::LocoForgotten;
  e.cab = cab;
  return Outcome::Decoded;
}

// <p0> <p1> | <p1 A>..<p1 H> | <p1 MAIN> <p1 PROG> <p1 JOIN>
Outcome decode_power(const Tokenizer& t, Event& e) noexcept {
  std::uint8_t state = 0;
  if (!int_field<std::uint8_t>(t, 0, 0, 1, state)) return Outcome::Malformed;
  e.power.on = state == 1;
  e.power.track = '\0';
  e.power.role = Role::Main;

  const std::size_t n = t.count();
  if (n == 1) {
    e.type = EventType::PowerAll;
    return Outcome::Decoded;
  }
  if (n != 2) return Outcome::Malformed;

  Field what;
  t.at(1, what);
  if (what.quoted) return Outcome::Malformed;
  if (what.text.size() == 1 && what.text[0] >= 'A' && what.text[0] <= 'H') {
    e.type = EventType::PowerTrack;
    e.power.track = what.text[0];
    return Outcome::Decoded;
  }
  if (what.equals("MAIN")) e.power.role = Role::Main;
  else if (what.equals("PROG")) e.power.role = Role::Prog;
  else if (what.equals("JOIN")) e.power.role = Role::Join;
  else return Outcome::Malformed;
  e.type = EventType::PowerRole;
  return Outcome::Decoded;
}

// Parses "<digits>" at text[pos], advancing pos. Fails on no digits or overflow.
bool parse_number(StringView text, std::size_t& pos, std::uint16_t& out) noexcept {
  const std::size_t start = pos;
  while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
  Field digits;
  digits.text = StringView(text.data() + start, pos - start);
  return digits.to_integer(out);
}

// <iDCC-EX V-major.minor.patch[suffix] / board / shield G-sha>
Outcome decode_version(const Tokenizer& t, Event& e) noexcept {
  Field product;
  Field version;
  if (!t.at(0, product) || !product.equals("DCC-EX") || !t.at(1, version)) return Outcome::Malformed;

  const StringView v = version.text;
  if (v.size() < 2 || v[0] != 'V' || v[1] != '-') return Outcome::Malformed;

  std::size_t pos = 2;
  std::uint16_t major = 0, minor = 0, patch = 0;
  if (!parse_number(v, pos, major) || pos >= v.size() || v[pos++] != '.' ||
      !parse_number(v, pos, minor) || pos >= v.size() || v[pos++] != '.' ||
      !parse_number(v, pos, patch)) {
    return Outcome::Malformed;
  }

  e.type = EventType::Version;
  e.version.major = major;
  e.version.minor = minor;
  e.version.patch = patch;
  e.version.text = t.rest();
  return Outcome::Decoded;
}

// Two integer fields: cv in 1..1024, value in -1..255.
bool cv_fields(const Tokenizer& t, CvResult& out) noexcept {
  return int_field<std::uint16_t>(t, 0, 1, kMaxCv, out.cv) &&
         int_field<std::int16_t>(t, 1, -1, 255, out.value);
}

// <v cv value>
Outcome decode_verify(const Tokenizer& t, Event& e) noexcept {
  const std::size_t n = t.count();
  if (n == 3) return Outcome::Unrecognized;  // <v cv bit value>: bit verify, not yet supported
  if (n != 2 || !cv_fields(t, e.cv)) return Outcome::Malformed;
  e.type = EventType::CvRead;
  return Outcome::Decoded;
}

// <r cv value> | <r address> | <r LONG address UNSUPPORTED>
Outcome decode_read(const Tokenizer& t, Event& e) noexcept {
  Field first;
  if (!t.at(0, first)) return Outcome::Malformed;

  // Forms the library does not send: callback-tagged results and
  // <R LOCOID>/<R CONSIST> replies.
  for (char c : first.text) {
    if (c == '|') return Outcome::Unrecognized;
  }
  if (first.equals("LOCOID") || first.equals("CONSIST")) return Outcome::Unrecognized;

  const std::size_t n = t.count();
  if (first.equals("LONG")) {
    if (n != 3 || !keyword_field(t, 2, "UNSUPPORTED") ||
        !int_field<std::int16_t>(t, 1, INT16_MIN, INT16_MAX, e.address.address)) {
      return Outcome::Malformed;
    }
    e.type = EventType::AddressRead;
    e.address.long_unsupported = true;
    return Outcome::Decoded;
  }

  if (n == 1) {
    if (!int_field<std::int16_t>(t, 0, INT16_MIN, INT16_MAX, e.address.address)) return Outcome::Malformed;
    e.type = EventType::AddressRead;
    e.address.long_unsupported = false;
    return Outcome::Decoded;
  }
  if (n == 2) {
    if (!cv_fields(t, e.cv)) return Outcome::Malformed;
    e.type = EventType::CvWritten;
    return Outcome::Decoded;
  }
  return Outcome::Malformed;
}

// <w address> | <w CONSIST ...>
Outcome decode_write(const Tokenizer& t, Event& e) noexcept {
  if (keyword_field(t, 0, "CONSIST")) return Outcome::Unrecognized;
  if (t.count() != 1 ||
      !int_field<std::int16_t>(t, 0, INT16_MIN, INT16_MAX, e.address.address)) {
    return Outcome::Malformed;
  }
  e.type = EventType::AddressWritten;
  e.address.long_unsupported = false;
  return Outcome::Decoded;
}

// <X>
Outcome decode_failed(const Tokenizer& t, Event& e) noexcept {
  if (t.count() != 0) return Outcome::Malformed;
  e.type = EventType::Failed;
  return Outcome::Decoded;
}

constexpr bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// <* text *>: free text, closing marker and surrounding whitespace removed.
Outcome decode_diagnostic(const Tokenizer& t, Event& e) noexcept {
  const StringView rest = t.rest();
  std::size_t end = rest.size();
  if (end > 0 && rest[end - 1] == '*') --end;
  while (end > 0 && is_space(rest[end - 1])) --end;
  e.type = EventType::Diagnostic;
  e.text = StringView(rest.data(), end);
  return Outcome::Decoded;
}

}  // namespace

Event decode(StringView message) noexcept {
  const Tokenizer t(message);
  Event e;  // Unhandled until a decoder succeeds

  Outcome outcome = Outcome::Unrecognized;
  switch (t.opcode()) {
    case 'l': outcome = decode_loco(t, e); break;
    case '-': outcome = decode_forget(t, e); break;
    case 'p': outcome = decode_power(t, e); break;
    case 'i': outcome = decode_version(t, e); break;
    case 'v': outcome = decode_verify(t, e); break;
    case 'r': outcome = decode_read(t, e); break;
    case 'w': outcome = decode_write(t, e); break;
    case 'X': outcome = decode_failed(t, e); break;
    case '*': outcome = decode_diagnostic(t, e); break;
    default: break;
  }

  if (outcome != Outcome::Decoded) {
    e.type = EventType::Unhandled;
    e.unhandled.malformed = outcome == Outcome::Malformed;
  }
  e.message = message;
  return e;
}

}  // namespace dccex
