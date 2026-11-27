#include "dccex/decoder.hpp"

#include <cstdint>

#include "dccex/tokenizer.hpp"

namespace dccex {
namespace {

// DCC ranges enforced on decoded values.
constexpr std::uint16_t kMaxCab = 10239;
constexpr std::uint16_t kMaxCv = 1024;

constexpr std::uint8_t kNoField = Unhandled::kNoField;

// Outcome of decoding one message or checking one field: success, or the
// reason it failed and the field responsible.
struct Verdict {
  bool ok;
  Reason reason;
  std::uint8_t field;
};

constexpr Verdict pass() noexcept { return {true, Reason::UnknownOpcode, kNoField}; }

constexpr Verdict reject(Reason reason, std::size_t field = kNoField) noexcept {
  return {false, reason, field < kNoField ? static_cast<std::uint8_t>(field) : kNoField};
}

// True if the field has the shape of a decimal integer, whether or not the
// value fits a particular type. Tells NotANumber apart from OutOfRange.
bool looks_like_integer(const Field& f) noexcept {
  if (f.quoted || f.text.empty()) return false;
  std::size_t i = f.text[0] == '-' ? 1 : 0;
  if (i == f.text.size()) return false;
  for (; i < f.text.size(); ++i) {
    if (f.text[i] < '0' || f.text[i] > '9') return false;
  }
  return true;
}

// Field `index` as an integer of type T within [lo, hi].
template <typename T>
Verdict int_field(const Tokenizer& t, std::size_t index, T lo, T hi, T& out) noexcept {
  Field f;
  if (!t.at(index, f)) return reject(Reason::WrongFieldCount);
  if (!looks_like_integer(f)) return reject(Reason::NotANumber, index);
  T value{};
  if (!f.to_integer(value) || value < lo || value > hi) return reject(Reason::OutOfRange, index);
  out = value;
  return pass();
}

// Field `index` is the unquoted word `keyword`.
Verdict keyword_field(const Tokenizer& t, std::size_t index, StringView keyword) noexcept {
  Field f;
  if (!t.at(index, f)) return reject(Reason::WrongFieldCount);
  if (f.quoted || !f.equals(keyword)) return reject(Reason::UnknownKeyword, index);
  return pass();
}

bool is_keyword(const Tokenizer& t, std::size_t index, StringView keyword) noexcept {
  return keyword_field(t, index, keyword).ok;
}

Verdict field_count(const Tokenizer& t, std::size_t expected) noexcept {
  return t.count() == expected ? pass() : reject(Reason::WrongFieldCount);
}

// Evaluates a Verdict-returning expression and returns early if it failed.
#define DCCEX_CHECK(expr)          \
  do {                             \
    const Verdict v_ = (expr);     \
    if (!v_.ok) return v_;         \
  } while (false)

// <l cab reg speedbyte functions>
Verdict decode_loco(const Tokenizer& t, Event& e) noexcept {
  std::uint16_t cab = 0;
  std::int32_t reg = 0;
  std::uint8_t speed_byte = 0;
  std::uint32_t functions = 0;
  DCCEX_CHECK(field_count(t, 4));
  DCCEX_CHECK(int_field<std::uint16_t>(t, 0, 1, kMaxCab, cab));
  DCCEX_CHECK(int_field<std::int32_t>(t, 1, INT32_MIN, INT32_MAX, reg));  // legacy, ignored
  DCCEX_CHECK(int_field<std::uint8_t>(t, 2, 0, 255, speed_byte));
  DCCEX_CHECK(int_field<std::uint32_t>(t, 3, 0, UINT32_MAX, functions));

  // Bit 7: direction. Bits 0-6: DCC speed code, 0 stop, 1 emergency stop,
  // 2..127 running, which is client speed + 1.
  const std::uint8_t code = speed_byte & 0x7F;
  e.type = EventType::Loco;
  e.loco.cab = cab;
  e.loco.forward = (speed_byte & 0x80) != 0;
  e.loco.emergency_stop = code == 1;
  e.loco.speed = code >= 2 ? static_cast<std::uint8_t>(code - 1) : 0;
  e.loco.functions = functions;
  return pass();
}

// <- cab>
Verdict decode_forget(const Tokenizer& t, Event& e) noexcept {
  std::uint16_t cab = 0;
  DCCEX_CHECK(field_count(t, 1));
  DCCEX_CHECK(int_field<std::uint16_t>(t, 0, 1, kMaxCab, cab));
  e.type = EventType::LocoForgotten;
  e.cab = cab;
  return pass();
}

// <p0> <p1> | <p1 A>..<p1 H> | <p1 MAIN> <p1 PROG> <p1 JOIN>
Verdict decode_power(const Tokenizer& t, Event& e) noexcept {
  const std::size_t n = t.count();
  if (n != 1 && n != 2) return reject(Reason::WrongFieldCount);

  std::uint8_t state = 0;
  DCCEX_CHECK(int_field<std::uint8_t>(t, 0, 0, 1, state));
  e.power.on = state == 1;
  e.power.track = '\0';
  e.power.role = Role::Main;

  if (n == 1) {
    e.type = EventType::PowerAll;
    return pass();
  }

  Field what;
  t.at(1, what);
  if (!what.quoted && what.text.size() == 1 && what.text[0] >= 'A' && what.text[0] <= 'H') {
    e.type = EventType::PowerTrack;
    e.power.track = what.text[0];
    return pass();
  }
  if (is_keyword(t, 1, "MAIN")) e.power.role = Role::Main;
  else if (is_keyword(t, 1, "PROG")) e.power.role = Role::Prog;
  else if (is_keyword(t, 1, "JOIN")) e.power.role = Role::Join;
  else return reject(Reason::UnknownKeyword, 1);
  e.type = EventType::PowerRole;
  return pass();
}

// Parses digits at text[pos], advancing pos. Fails on no digits or overflow.
bool parse_number(StringView text, std::size_t& pos, std::uint16_t& out) noexcept {
  const std::size_t start = pos;
  while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
  Field digits;
  digits.text = StringView(text.data() + start, pos - start);
  return digits.to_integer(out);
}

// <iDCC-EX V-major.minor.patch[suffix] / board / shield G-sha>
Verdict decode_version(const Tokenizer& t, Event& e) noexcept {
  if (t.count() < 2) return reject(Reason::WrongFieldCount);
  DCCEX_CHECK(keyword_field(t, 0, "DCC-EX"));

  Field version;
  t.at(1, version);
  const StringView v = version.text;
  std::size_t pos = 2;
  std::uint16_t major = 0, minor = 0, patch = 0;
  if (version.quoted || v.size() < 2 || v[0] != 'V' || v[1] != '-' ||
      !parse_number(v, pos, major) || pos >= v.size() || v[pos++] != '.' ||
      !parse_number(v, pos, minor) || pos >= v.size() || v[pos++] != '.' ||
      !parse_number(v, pos, patch)) {
    return reject(Reason::BadFormat, 1);
  }

  e.type = EventType::Version;
  e.version.major = major;
  e.version.minor = minor;
  e.version.patch = patch;
  e.version.text = t.rest();
  return pass();
}

// Two integer fields: cv in 1..1024, value in -1..255.
Verdict cv_fields(const Tokenizer& t, CvResult& out) noexcept {
  DCCEX_CHECK(int_field<std::uint16_t>(t, 0, 1, kMaxCv, out.cv));
  DCCEX_CHECK(int_field<std::int16_t>(t, 1, -1, 255, out.value));
  return pass();
}

// <v cv value>
Verdict decode_verify(const Tokenizer& t, Event& e) noexcept {
  const std::size_t n = t.count();
  if (n == 3) return reject(Reason::UnsupportedForm);  // <v cv bit value>: bit verify
  if (n != 2) return reject(Reason::WrongFieldCount);
  DCCEX_CHECK(cv_fields(t, e.cv));
  e.type = EventType::CvRead;
  return pass();
}

// <r cv value> | <r address> | <r LONG address UNSUPPORTED>
Verdict decode_read(const Tokenizer& t, Event& e) noexcept {
  Field first;
  if (!t.at(0, first)) return reject(Reason::WrongFieldCount);

  // Forms the library does not send: callback-tagged results and
  // <R LOCOID>/<R CONSIST> replies.
  for (char c : first.text) {
    if (c == '|') return reject(Reason::UnsupportedForm, 0);
  }
  if (is_keyword(t, 0, "LOCOID") || is_keyword(t, 0, "CONSIST")) {
    return reject(Reason::UnsupportedForm, 0);
  }

  const std::size_t n = t.count();
  if (is_keyword(t, 0, "LONG")) {
    DCCEX_CHECK(field_count(t, 3));
    DCCEX_CHECK(int_field<std::int16_t>(t, 1, INT16_MIN, INT16_MAX, e.address.address));
    DCCEX_CHECK(keyword_field(t, 2, "UNSUPPORTED"));
    e.type = EventType::AddressRead;
    e.address.long_unsupported = true;
    return pass();
  }
  if (n == 1) {
    DCCEX_CHECK(int_field<std::int16_t>(t, 0, INT16_MIN, INT16_MAX, e.address.address));
    e.type = EventType::AddressRead;
    e.address.long_unsupported = false;
    return pass();
  }
  if (n == 2) {
    DCCEX_CHECK(cv_fields(t, e.cv));
    e.type = EventType::CvWritten;
    return pass();
  }
  return reject(Reason::WrongFieldCount);
}

// <w address> | <w CONSIST ...>
Verdict decode_write(const Tokenizer& t, Event& e) noexcept {
  if (is_keyword(t, 0, "CONSIST")) return reject(Reason::UnsupportedForm, 0);
  DCCEX_CHECK(field_count(t, 1));
  DCCEX_CHECK(int_field<std::int16_t>(t, 0, INT16_MIN, INT16_MAX, e.address.address));
  e.type = EventType::AddressWritten;
  e.address.long_unsupported = false;
  return pass();
}

// <X>
Verdict decode_failed(const Tokenizer& t, Event& e) noexcept {
  DCCEX_CHECK(field_count(t, 0));
  e.type = EventType::Failed;
  return pass();
}

#undef DCCEX_CHECK

constexpr bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// <* text *>: free text, closing marker and surrounding whitespace removed.
Verdict decode_diagnostic(const Tokenizer& t, Event& e) noexcept {
  const StringView rest = t.rest();
  std::size_t end = rest.size();
  if (end > 0 && rest[end - 1] == '*') --end;
  while (end > 0 && is_space(rest[end - 1])) --end;
  e.type = EventType::Diagnostic;
  e.text = StringView(rest.data(), end);
  return pass();
}

// For messages made of fields: an unterminated quote, reported at the field
// where it starts.
Verdict check_quotes(const Tokenizer& t) noexcept {
  Tokenizer scan = t;
  scan.rewind();
  Field f;
  std::size_t n = 0;
  while (scan.next(f)) ++n;
  return scan.malformed() ? reject(Reason::UnterminatedQuote, n) : pass();
}

using Decoder = Verdict (*)(const Tokenizer&, Event&) noexcept;

// Decoders for messages made of fields. Free-text messages are handled apart.
Decoder field_decoder(char opcode) noexcept {
  switch (opcode) {
    case 'l': return decode_loco;
    case '-': return decode_forget;
    case 'p': return decode_power;
    case 'i': return decode_version;
    case 'v': return decode_verify;
    case 'r': return decode_read;
    case 'w': return decode_write;
    case 'X': return decode_failed;
    default: return nullptr;
  }
}

}  // namespace

Event decode(StringView message) noexcept {
  const Tokenizer t(message);
  Event e;  // Unhandled, UnknownOpcode until a decoder succeeds

  Verdict verdict = reject(Reason::UnknownOpcode);
  if (t.opcode() == '*') {
    verdict = decode_diagnostic(t, e);
  } else if (const Decoder decoder = field_decoder(t.opcode())) {
    verdict = check_quotes(t);
    if (verdict.ok) verdict = decoder(t, e);
  }

  if (!verdict.ok) {
    e.type = EventType::Unhandled;
    e.unhandled.reason = verdict.reason;
    e.unhandled.field = verdict.field;
  }
  e.message = message;
  return e;
}

const char* name(EventType type) noexcept {
  switch (type) {
    case EventType::Unhandled: return "Unhandled";
    case EventType::Loco: return "Loco";
    case EventType::LocoForgotten: return "LocoForgotten";
    case EventType::PowerAll: return "PowerAll";
    case EventType::PowerTrack: return "PowerTrack";
    case EventType::PowerRole: return "PowerRole";
    case EventType::Version: return "Version";
    case EventType::CvRead: return "CvRead";
    case EventType::CvWritten: return "CvWritten";
    case EventType::AddressRead: return "AddressRead";
    case EventType::AddressWritten: return "AddressWritten";
    case EventType::Failed: return "Failed";
    case EventType::Diagnostic: return "Diagnostic";
  }
  return "?";
}

const char* name(Reason reason) noexcept {
  switch (reason) {
    case Reason::UnknownOpcode: return "UnknownOpcode";
    case Reason::UnsupportedForm: return "UnsupportedForm";
    case Reason::WrongFieldCount: return "WrongFieldCount";
    case Reason::NotANumber: return "NotANumber";
    case Reason::OutOfRange: return "OutOfRange";
    case Reason::UnknownKeyword: return "UnknownKeyword";
    case Reason::BadFormat: return "BadFormat";
    case Reason::UnterminatedQuote: return "UnterminatedQuote";
  }
  return "?";
}

}  // namespace dccex
