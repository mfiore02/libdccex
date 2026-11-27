#include <doctest/doctest.h>

#include <random>
#include <string>
#include <vector>

#include "dccex/decoder.hpp"
#include "dccex/framer.hpp"
#include "fixtures/captures.hpp"

namespace {

using dccex::EventType;

dccex::Event dec(const std::string& message) {
  return dccex::decode(dccex::StringView(message.data(), message.size()));
}

std::string str(dccex::StringView v) { return std::string(v.data(), v.size()); }

void check_unhandled(const std::string& message, bool malformed) {
  CAPTURE(message);
  const auto e = dec(message);
  CHECK(e.type == EventType::Unhandled);
  CHECK(e.unhandled.malformed == malformed);
}

void check_malformed(const std::string& message) { check_unhandled(message, true); }
void check_unrecognized(const std::string& message) { check_unhandled(message, false); }

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
  check_malformed("l 3 0 128");             // missing functions
  check_malformed("l 3 0 128 0 9");         // extra field
  check_malformed("l 0 -1 128 0");          // no loco
  check_malformed("l 10240 0 128 0");       // beyond DCC range
  check_malformed("l 3 0 256 0");           // speed byte out of range
  check_malformed("l 3 0 -1 0");
  check_malformed("l 3 0 128 -1");          // functions out of range
  check_malformed("l 3 0 128 4294967296");
  check_malformed("l three 0 128 0");
  check_malformed("l 3 x 128 0");           // register must still be a number
}

TEST_CASE("a forgotten loco is reported by address") {
  const auto e = dec("- 3");
  REQUIRE(e.type == EventType::LocoForgotten);
  CHECK(e.cab == 3);
}

TEST_CASE("malformed forget messages are unhandled") {
  check_malformed("-");
  check_malformed("- 0");
  check_malformed("- 10240");
  check_malformed("- 3 4");
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
  check_malformed("p");
  check_malformed("p2");          // mixed state is never sent as a message
  check_malformed("p1 I");        // tracks are A..H
  check_malformed("p1 a");
  check_malformed("p1 AB");
  check_malformed("p1 main");     // keywords are case-sensitive
  check_malformed("p1 A B");
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
  check_malformed("i");
  check_malformed("iSomething else");
  check_malformed("iDCC-EX");
  check_malformed("iDCC-EX V-5.6");
  check_malformed("iDCC-EX V-5..6");
  check_malformed("iDCC-EX 5.6.6");
  check_malformed("iDCC++ V-5.6.6 / MEGA");   // right shape, wrong product
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
  check_malformed("v 0 3");
  check_malformed("v 1025 3");
  check_malformed("v 1 256");
  check_malformed("v 1 -2");
  check_malformed("r 0 3");
  check_malformed("r 1 256");
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
  check_malformed("r");
  check_malformed("r 40000");
  check_malformed("r LONG x UNSUPPORTED");
  check_malformed("r LONG 50");
  check_malformed("r LONG 50 SUPPORTED");
  check_malformed("w");
  check_malformed("w 4014 1");
}

TEST_CASE("valid replies the library does not support yet are unrecognized, not malformed") {
  check_unrecognized("v 1 2 1");             // bit verify
  check_unrecognized("r1|2|3 4");            // callback-tagged result
  check_unrecognized("r LOCOID 3");
  check_unrecognized("r CONSIST 3");
  check_unrecognized("w CONSIST 5");
  check_unrecognized("w CONSIST 5 REVERSE");
}

// --- Failure and diagnostics -----------------------------------------------------------

TEST_CASE("a command failure") {
  CHECK(dec("X").type == EventType::Failed);
  check_malformed("X 1");
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
  check_unrecognized("");
  check_unrecognized("Q");
  check_unrecognized("jR 3 4014");
  check_unrecognized("!PAUSED");
  check_unrecognized("H 1 1");
}

TEST_CASE("every event carries the original message") {
  for (const char* m : {"l 3 0 128 0", "p1", "X", "* hi *", "Q", "l bad"}) {
    const std::string message = m;
    const auto e = dec(message);
    CHECK(e.message.data() == message.data());
    CHECK(e.message.size() == message.size());
  }
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
