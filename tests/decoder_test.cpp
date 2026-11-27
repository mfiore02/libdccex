#include <doctest/doctest.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "dccex/decoder.hpp"
#include "dccex/framer.hpp"
#include "fixtures/captures.hpp"

namespace {

using dccex::EventType;
using dccex::Reason;

dccex::Event dec(const std::string& message) {
  return dccex::decode(dccex::StringView(message.data(), message.size()));
}

std::string str(dccex::StringView v) { return std::string(v.data(), v.size()); }

// The message decodes to Unhandled with exactly this reason and field index.
void check_rejected(const std::string& message, dccex::Reason reason, std::uint8_t field) {
  CAPTURE(message);
  const auto e = dec(message);
  REQUIRE(e.type == EventType::Unhandled);
  CHECK(e.unhandled.reason == reason);
  CHECK(int(e.unhandled.field) == int(field));
}

}  // namespace

TEST_SUITE("decoder") {

// --- Loco broadcasts -------------------------------------------------------------

TEST_CASE("a loco broadcast decodes address, speed, direction and functions") {
  const auto e = dec("l 4014 0 134 0");  // captured reply to <t 4014 5 1>
  REQUIRE(e.type == EventType::Loco);
  CHECK(e.loco.cab == 4014);
  CHECK(e.loco.speed == 5);
  CHECK(e.loco.forward);
  CHECK_FALSE(e.loco.emergency_stop);
  CHECK(e.loco.functions == 0);
}

TEST_CASE("the register field is ignored, whatever its value") {
  for (const char* m : {"l 3 1 139 513", "l 3 0 139 513", "l 3 -1 139 513"}) {
    CAPTURE(m);
    const auto e = dec(m);
    REQUIRE(e.type == EventType::Loco);
    CHECK(e.loco.cab == 3);
    CHECK(e.loco.speed == 10);
    CHECK(e.loco.functions == 513);  // F0 and F9
  }
}

TEST_CASE("the speed byte maps to client speed, direction and emergency stop") {
  struct Case { const char* message; int speed; bool forward; bool estop; };
  const Case cases[] = {
      {"l 3 0 128 0", 0, true, false},     // stopped, forward
      {"l 3 0 0 0", 0, false, false},      // stopped, reverse
      {"l 3 0 129 0", 0, true, true},      // emergency stop, forward
      {"l 3 0 1 0", 0, false, true},       // emergency stop, reverse (also sent when forgotten)
      {"l 3 0 130 0", 1, true, false},     // slowest
      {"l 3 0 2 0", 1, false, false},
      {"l 3 0 255 0", 126, true, false},   // fastest
      {"l 3 0 127 0", 126, false, false},
  };
  for (const auto& c : cases) {
    CAPTURE(c.message);
    const auto e = dec(c.message);
    REQUIRE(e.type == EventType::Loco);
    CHECK(e.loco.speed == c.speed);
    CHECK(e.loco.forward == c.forward);
    CHECK(e.loco.emergency_stop == c.estop);
  }
}

TEST_CASE("all 32 function bits are carried") {
  const auto e = dec("l 3 0 128 4294967295");
  REQUIRE(e.type == EventType::Loco);
  CHECK(e.loco.functions == 0xFFFFFFFFu);
}

TEST_CASE("the full DCC address range is accepted") {
  CHECK(dec("l 1 0 128 0").loco.cab == 1);
  CHECK(dec("l 10239 0 128 0").loco.cab == 10239);
}

TEST_CASE("malformed loco broadcasts are unhandled") {
  check_rejected("l 3 0 128", Reason::WrongFieldCount, dccex::Unhandled::kNoField);             // missing functions
  check_rejected("l 3 0 128 0 9", Reason::WrongFieldCount, dccex::Unhandled::kNoField);         // extra field
  check_rejected("l 0 -1 128 0", Reason::OutOfRange, 0);          // no loco
  check_rejected("l 10240 0 128 0", Reason::OutOfRange, 0);       // beyond DCC range
  check_rejected("l 3 0 256 0", Reason::OutOfRange, 2);           // speed byte out of range
  check_rejected("l 3 0 -1 0", Reason::OutOfRange, 2);
  check_rejected("l 3 0 128 -1", Reason::OutOfRange, 3);          // functions out of range
  check_rejected("l 3 0 128 4294967296", Reason::OutOfRange, 3);
  check_rejected("l three 0 128 0", Reason::NotANumber, 0);
  check_rejected("l 3 x 128 0", Reason::NotANumber, 1);           // register must still be a number
}

TEST_CASE("a forgotten loco is reported by address") {
  const auto e = dec("- 3");
  REQUIRE(e.type == EventType::LocoForgotten);
  CHECK(e.cab == 3);
}

TEST_CASE("malformed forget messages are unhandled") {
  check_rejected("-", Reason::WrongFieldCount, dccex::Unhandled::kNoField);
  check_rejected("- 0", Reason::OutOfRange, 0);
  check_rejected("- 10240", Reason::OutOfRange, 0);
  check_rejected("- 3 4", Reason::WrongFieldCount, dccex::Unhandled::kNoField);
}

// --- Power ---------------------------------------------------------------------

TEST_CASE("aggregate power state") {
  auto e = dec("p1");
  REQUIRE(e.type == EventType::PowerAll);
  CHECK(e.power.on);
  e = dec("p0");
  REQUIRE(e.type == EventType::PowerAll);
  CHECK_FALSE(e.power.on);
}

TEST_CASE("per-track power state names the track") {
  auto e = dec("p1 A");
  REQUIRE(e.type == EventType::PowerTrack);
  CHECK(e.power.on);
  CHECK(e.power.track == 'A');
  e = dec("p0 H");
  REQUIRE(e.type == EventType::PowerTrack);
  CHECK_FALSE(e.power.on);
  CHECK(e.power.track == 'H');
}

TEST_CASE("per-role power state names the role") {
  struct Case { const char* message; dccex::Role role; bool on; };
  const Case cases[] = {
      {"p1 MAIN", dccex::Role::Main, true},
      {"p1 PROG", dccex::Role::Prog, true},
      {"p1 JOIN", dccex::Role::Join, true},
      {"p0 MAIN", dccex::Role::Main, false},
  };
  for (const auto& c : cases) {
    CAPTURE(c.message);
    const auto e = dec(c.message);
    REQUIRE(e.type == EventType::PowerRole);
    CHECK(e.power.role == c.role);
    CHECK(e.power.on == c.on);
  }
}

TEST_CASE("malformed power messages are unhandled") {
  check_rejected("p", Reason::WrongFieldCount, dccex::Unhandled::kNoField);
  check_rejected("p2", Reason::OutOfRange, 0);          // mixed state is never sent as a message
  check_rejected("p1 I", Reason::UnknownKeyword, 1);        // tracks are A..H
  check_rejected("p1 a", Reason::UnknownKeyword, 1);
  check_rejected("p1 AB", Reason::UnknownKeyword, 1);
  check_rejected("p1 main", Reason::UnknownKeyword, 1);     // keywords are case-sensitive
  check_rejected("p1 A B", Reason::WrongFieldCount, dccex::Unhandled::kNoField);
}

// --- Version ---------------------------------------------------------------------

TEST_CASE("the version line yields the version number and full text") {
  const std::string message = "iDCC-EX V-5.6.6 / MEGA / EX8874 G-c389fe9";  // views point into it
  const auto e = dec(message);
  REQUIRE(e.type == EventType::Version);
  CHECK(e.version.major == 5);
  CHECK(e.version.minor == 6);
  CHECK(e.version.patch == 6);
  CHECK(str(e.version.text) == "DCC-EX V-5.6.6 / MEGA / EX8874 G-c389fe9");
}

TEST_CASE("a suffix after the patch number is allowed") {
  const auto e = dec("iDCC-EX V-5.2.40-Devel / MEGA / STANDARD_MOTOR_SHIELD G-abc");
  REQUIRE(e.type == EventType::Version);
  CHECK(e.version.major == 5);
  CHECK(e.version.minor == 2);
  CHECK(e.version.patch == 40);
}

TEST_CASE("malformed version lines are unhandled") {
  check_rejected("i", Reason::WrongFieldCount, dccex::Unhandled::kNoField);
  check_rejected("iSomething else", Reason::UnknownKeyword, 0);
  check_rejected("iDCC-EX", Reason::WrongFieldCount, dccex::Unhandled::kNoField);
  check_rejected("iDCC-EX V-5.6", Reason::BadFormat, 1);
  check_rejected("iDCC-EX V-5..6", Reason::BadFormat, 1);
  check_rejected("iDCC-EX 5.6.6", Reason::BadFormat, 1);
  check_rejected("iDCC++ V-5.6.6 / MEGA", Reason::UnknownKeyword, 0);   // right shape, wrong product
}

// --- Programming results -----------------------------------------------------------

TEST_CASE("a CV read result") {
  auto e = dec("v 1 3");
  REQUIRE(e.type == EventType::CvRead);
  CHECK(e.cv.cv == 1);
  CHECK(e.cv.value == 3);
  CHECK(e.cv.ok());

  e = dec("v 29 -1");
  REQUIRE(e.type == EventType::CvRead);
  CHECK(e.cv.cv == 29);
  CHECK_FALSE(e.cv.ok());
}

TEST_CASE("a CV write result") {
  auto e = dec("r 3 10");
  REQUIRE(e.type == EventType::CvWritten);
  CHECK(e.cv.cv == 3);
  CHECK(e.cv.value == 10);
  CHECK(e.cv.ok());

  e = dec("r 3 -1");
  REQUIRE(e.type == EventType::CvWritten);
  CHECK_FALSE(e.cv.ok());
}

TEST_CASE("CV results outside DCC ranges are malformed") {
  check_rejected("v 0 3", Reason::OutOfRange, 0);
  check_rejected("v 1025 3", Reason::OutOfRange, 0);
  check_rejected("v 1 256", Reason::OutOfRange, 1);
  check_rejected("v 1 -2", Reason::OutOfRange, 1);
  check_rejected("r 0 3", Reason::OutOfRange, 0);
  check_rejected("r 1 256", Reason::OutOfRange, 1);
}

TEST_CASE("an address read result") {
  auto e = dec("r 4014");
  REQUIRE(e.type == EventType::AddressRead);
  CHECK(e.address.address == 4014);
  CHECK_FALSE(e.address.long_unsupported);
  CHECK(e.address.ok());

  e = dec("r -1");
  REQUIRE(e.type == EventType::AddressRead);
  CHECK_FALSE(e.address.ok());
}

TEST_CASE("a long address the decoder cannot hold") {
  const auto e = dec("r LONG 50 UNSUPPORTED");
  REQUIRE(e.type == EventType::AddressRead);
  CHECK(e.address.address == 50);
  CHECK(e.address.long_unsupported);
  CHECK_FALSE(e.address.ok());
}

TEST_CASE("an address write result") {
  auto e = dec("w 4014");
  REQUIRE(e.type == EventType::AddressWritten);
  CHECK(e.address.address == 4014);
  CHECK(e.address.ok());

  e = dec("w -3");
  REQUIRE(e.type == EventType::AddressWritten);
  CHECK_FALSE(e.address.ok());
}

TEST_CASE("malformed address results are unhandled") {
  check_rejected("r", Reason::WrongFieldCount, dccex::Unhandled::kNoField);
  check_rejected("r 40000", Reason::OutOfRange, 0);
  check_rejected("r LONG x UNSUPPORTED", Reason::NotANumber, 1);
  check_rejected("r LONG 50", Reason::WrongFieldCount, dccex::Unhandled::kNoField);
  check_rejected("r LONG 50 SUPPORTED", Reason::UnknownKeyword, 2);
  check_rejected("w", Reason::WrongFieldCount, dccex::Unhandled::kNoField);
  check_rejected("w 4014 1", Reason::WrongFieldCount, dccex::Unhandled::kNoField);
}

TEST_CASE("valid replies the library does not support yet are unrecognized, not malformed") {
  check_rejected("v 1 2 1", Reason::UnsupportedForm, dccex::Unhandled::kNoField);             // bit verify
  check_rejected("r1|2|3 4", Reason::UnsupportedForm, 0);            // callback-tagged result
  check_rejected("r LOCOID 3", Reason::UnsupportedForm, 0);
  check_rejected("r CONSIST 3", Reason::UnsupportedForm, 0);
  check_rejected("w CONSIST 5", Reason::UnsupportedForm, 0);
  check_rejected("w CONSIST 5 REVERSE", Reason::UnsupportedForm, 0);
}

// --- Failure and diagnostics -----------------------------------------------------------

TEST_CASE("a command failure") {
  CHECK(dec("X").type == EventType::Failed);
  check_rejected("X 1", Reason::WrongFieldCount, dccex::Unhandled::kNoField);
}

TEST_CASE("diagnostics carry their text without the markers") {
  struct Case { const char* message; const char* text; };
  const Case cases[] = {
      {"* TRACK A ALERT FAULT *", "TRACK A ALERT FAULT"},
      {"* TRACK A NORMAL (after 20msec/103msec) 30mA *", "TRACK A NORMAL (after 20msec/103msec) 30mA"},
      {"*no spaces*", "no spaces"},
      {"* no closing marker", "no closing marker"},
      {"*", ""},
      {"* *", ""},
  };
  for (const auto& c : cases) {
    CAPTURE(c.message);
    const std::string message = c.message;  // e.text points into it
    const auto e = dec(message);
    REQUIRE(e.type == EventType::Diagnostic);
    CHECK(str(e.text) == c.text);
  }
}

// --- Unknown messages -------------------------------------------------------------------

TEST_CASE("unknown and unsupported opcodes are unrecognized") {
  check_rejected("", Reason::UnknownOpcode, dccex::Unhandled::kNoField);
  check_rejected("Q", Reason::UnknownOpcode, dccex::Unhandled::kNoField);
  check_rejected("jR 3 4014", Reason::UnknownOpcode, dccex::Unhandled::kNoField);
  check_rejected("!PAUSED", Reason::UnknownOpcode, dccex::Unhandled::kNoField);
  check_rejected("H 1 1", Reason::UnknownOpcode, dccex::Unhandled::kNoField);
}

TEST_CASE("every event carries the original message") {
  for (const char* m : {"l 3 0 128 0", "p1", "X", "* hi *", "Q", "l bad"}) {
    const std::string message = m;
    const auto e = dec(message);
    CHECK(e.message.data() == message.data());
    CHECK(e.message.size() == message.size());
  }
}

// --- Rejection reasons -------------------------------------------------------------

TEST_CASE("quoted numbers are not numbers") {
  check_rejected(R"(l "3" 0 128 0)", Reason::NotANumber, 0);
}

TEST_CASE("an unterminated quote is reported at the field where it starts") {
  check_rejected(R"(l 3 "x 128 0)", Reason::UnterminatedQuote, 1);
  check_rejected(R"(p1 "MAIN)", Reason::UnterminatedQuote, 1);
}

TEST_CASE("quotes do not affect free-text or unknown messages") {
  const std::string diag = R"(* say "hi *)";
  const auto e = dec(diag);
  REQUIRE(e.type == EventType::Diagnostic);
  CHECK(str(e.text) == R"(say "hi)");
  check_rejected(R"(Q "x)", Reason::UnknownOpcode, dccex::Unhandled::kNoField);
}

TEST_CASE("the first problem in a message is the one reported") {
  check_rejected("l x y 999 -5", Reason::NotANumber, 0);
  check_rejected("l 3 0 999 x", Reason::OutOfRange, 2);
  check_rejected("r abc", Reason::NotANumber, 0);
  check_rejected("r 1 2 3", Reason::WrongFieldCount, dccex::Unhandled::kNoField);
}

TEST_CASE("malformed() separates bad data from messages the library does not handle") {
  struct Case { Reason reason; bool malformed; };
  const Case cases[] = {
      {Reason::UnknownOpcode, false},  {Reason::UnsupportedForm, false},
      {Reason::WrongFieldCount, true}, {Reason::NotANumber, true},
      {Reason::OutOfRange, true},      {Reason::UnknownKeyword, true},
      {Reason::BadFormat, true},       {Reason::UnterminatedQuote, true},
  };
  for (const auto& c : cases) {
    dccex::Unhandled u{c.reason, dccex::Unhandled::kNoField};
    CHECK(u.malformed() == c.malformed);
  }
}

TEST_CASE("a default event is an unhandled unknown opcode") {
  const dccex::Event e;
  CHECK(e.type == EventType::Unhandled);
  CHECK(e.unhandled.reason == Reason::UnknownOpcode);
  CHECK(int(e.unhandled.field) == int(dccex::Unhandled::kNoField));
}

// --- Names for logging -------------------------------------------------------------

TEST_CASE("event types have readable names") {
  CHECK(std::string(dccex::name(EventType::Loco)) == "Loco");
  CHECK(std::string(dccex::name(EventType::Unhandled)) == "Unhandled");
  CHECK(std::string(dccex::name(EventType::AddressWritten)) == "AddressWritten");
}

TEST_CASE("reasons have readable names") {
  CHECK(std::string(dccex::name(Reason::OutOfRange)) == "OutOfRange");
  CHECK(std::string(dccex::name(Reason::UnterminatedQuote)) == "UnterminatedQuote");
}

TEST_CASE("every enumerator has its own name") {
  const EventType types[] = {
      EventType::Unhandled, EventType::Loco, EventType::LocoForgotten, EventType::PowerAll,
      EventType::PowerTrack, EventType::PowerRole, EventType::Version, EventType::CvRead,
      EventType::CvWritten, EventType::AddressRead, EventType::AddressWritten,
      EventType::Failed, EventType::Diagnostic,
  };
  const Reason reasons[] = {
      Reason::UnknownOpcode, Reason::UnsupportedForm, Reason::WrongFieldCount,
      Reason::NotANumber, Reason::OutOfRange, Reason::UnknownKeyword,
      Reason::BadFormat, Reason::UnterminatedQuote,
  };
  std::vector<std::string> names;
  for (auto t : types) names.emplace_back(dccex::name(t));
  for (auto r : reasons) names.emplace_back(dccex::name(r));
  for (const auto& n : names) {
    CAPTURE(n);
    CHECK_FALSE(n.empty());
    CHECK(n != "?");
  }
  for (std::size_t i = 0; i < names.size(); ++i) {
    for (std::size_t j = i + 1; j < names.size(); ++j) CHECK(names[i] != names[j]);
  }
}

TEST_CASE("an invalid enumerator value gets a placeholder name") {
  CHECK(std::string(dccex::name(static_cast<EventType>(200))) == "?");
  CHECK(std::string(dccex::name(static_cast<Reason>(200))) == "?");
}

// --- Real traffic ------------------------------------------------------------------------

TEST_CASE("captured power-on burst decodes as diagnostics and power states") {
  const auto capture = fixtures::power_on_burst();
  std::vector<EventType> types;
  for (const auto& m : capture.messages) types.push_back(dec(m).type);
  CHECK(types == std::vector<EventType>{
      EventType::Diagnostic, EventType::Diagnostic,
      EventType::PowerTrack, EventType::PowerTrack,
      EventType::PowerAll,
      EventType::PowerRole, EventType::PowerRole,
      EventType::Diagnostic, EventType::Diagnostic, EventType::Diagnostic, EventType::Diagnostic,
  });
}

TEST_CASE("captured loco broadcasts decode end to end through the framer") {
  const auto capture = fixtures::loco_broadcasts();
  struct Seen { std::vector<dccex::LocoState> locos; int other = 0; } seen;

  dccex::Framer<> framer;
  framer.feed(capture.bytes.data(), capture.bytes.size(),
              [](void* ctx, dccex::StringView m) {
                auto* s = static_cast<Seen*>(ctx);
                const auto e = dccex::decode(m);
                if (e.type == EventType::Loco) s->locos.push_back(e.loco);
                else ++s->other;
              },
              &seen);

  REQUIRE(seen.locos.size() == capture.messages.size());
  CHECK(seen.other == 0);
  for (const auto& l : seen.locos) CHECK(l.cab == 3);
  CHECK(seen.locos.front().speed == 0);
  CHECK(seen.locos.front().functions == 512);     // F9
  CHECK(seen.locos[4].functions == 516);          // F2 (whistle) with F9
  CHECK(seen.locos.back().speed == 5);
  CHECK(seen.locos.back().forward);
}

// --- Properties ---------------------------------------------------------------------------

TEST_CASE("random messages always decode to exactly one well-formed event") {
  std::mt19937 rng(20261127);
  const char alphabet[] = "lprvwXi*-01239 AMN\"|-";
  std::uniform_int_distribution<std::size_t> pick(0, sizeof(alphabet) - 2);
  std::uniform_int_distribution<std::size_t> length(0, 24);

  for (int round = 0; round < 5000; ++round) {
    std::string m(length(rng), '\0');
    for (char& c : m) c = alphabet[pick(rng)];
    CAPTURE(m);
    const auto e = dec(m);
    CHECK(e.message.data() == m.data());
    if (e.type == EventType::Loco) {
      CHECK(e.loco.cab >= 1);
      CHECK(e.loco.cab <= 10239);
      CHECK(e.loco.speed <= 126);
    }
    if (e.type == EventType::CvRead || e.type == EventType::CvWritten) {
      CHECK(e.cv.cv >= 1);
      CHECK(e.cv.cv <= 1024);
      CHECK(e.cv.value >= -1);
      CHECK(e.cv.value <= 255);
    }
  }
}

}  // TEST_SUITE
