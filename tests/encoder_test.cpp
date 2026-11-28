#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "dccex/encoder.hpp"
#include "dccex/framer.hpp"
#include "dccex/tokenizer.hpp"

namespace encode = dccex::encode;
using dccex::Buffer;
using dccex::EncodeError;
using dccex::kMaxCommandSize;
using dccex::PowerTarget;

namespace {

constexpr char kSentinel = '#';

// Runs an encoder into a sentinel-filled buffer and returns the command text.
// Checks that nothing beyond the reported length was touched.
template <typename F>
std::string build(F encoder) {
  char buf[kMaxCommandSize];
  std::memset(buf, kSentinel, sizeof buf);
  const dccex::Encoded r = encoder(Buffer(buf));
  REQUIRE(r.ok());
  REQUIRE(r.length <= sizeof buf);
  for (std::size_t i = r.length; i < sizeof buf; ++i) {
    CAPTURE(i);
    CHECK(buf[i] == kSentinel);
  }
  return std::string(buf, r.length);
}

// Runs an encoder expected to fail. Checks the result and that the buffer
// was left untouched.
template <typename F>
EncodeError failure(F encoder, std::size_t capacity = kMaxCommandSize) {
  char buf[kMaxCommandSize];
  std::memset(buf, kSentinel, sizeof buf);
  const dccex::Encoded r = encoder(Buffer(buf, capacity));
  CHECK_FALSE(r.ok());
  CHECK(r.length == 0);
  for (char c : buf) CHECK(c == kSentinel);
  return r.error;
}

std::string sv(dccex::StringView v) { return std::string(v.data(), v.size()); }

}  // namespace

TEST_SUITE("encoder") {

// --- Each command --------------------------------------------------------------

TEST_CASE("power") {
  CHECK(build([](Buffer b) { return encode::power(b, PowerTarget::All, true); }) == "<1>\n");
  CHECK(build([](Buffer b) { return encode::power(b, PowerTarget::All, false); }) == "<0>\n");
  CHECK(build([](Buffer b) { return encode::power(b, PowerTarget::Main, true); }) == "<1 MAIN>\n");
  CHECK(build([](Buffer b) { return encode::power(b, PowerTarget::Main, false); }) == "<0 MAIN>\n");
  CHECK(build([](Buffer b) { return encode::power(b, PowerTarget::Prog, true); }) == "<1 PROG>\n");
  CHECK(build([](Buffer b) { return encode::power(b, PowerTarget::Prog, false); }) == "<0 PROG>\n");
}

TEST_CASE("an invalid power target is out of range") {
  CHECK(failure([](Buffer b) { return encode::power(b, static_cast<PowerTarget>(9), true); }) ==
        EncodeError::OutOfRange);
}

TEST_CASE("join") {
  CHECK(build([](Buffer b) { return encode::join_prog_to_main(b); }) == "<1 JOIN>\n");
}

TEST_CASE("status") {
  CHECK(build([](Buffer b) { return encode::status(b); }) == "<s>\n");
}

TEST_CASE("throttle") {
  CHECK(build([](Buffer b) { return encode::throttle(b, 4014, 5, true); }) == "<t 4014 5 1>\n");
  CHECK(build([](Buffer b) { return encode::throttle(b, 3, 0, false); }) == "<t 3 0 0>\n");
  CHECK(build([](Buffer b) { return encode::throttle(b, 1, 126, true); }) == "<t 1 126 1>\n");
  CHECK(build([](Buffer b) { return encode::throttle(b, 10239, 1, false); }) == "<t 10239 1 0>\n");
}

TEST_CASE("throttle arguments out of range") {
  CHECK(failure([](Buffer b) { return encode::throttle(b, 0, 5, true); }) == EncodeError::OutOfRange);
  CHECK(failure([](Buffer b) { return encode::throttle(b, 10240, 5, true); }) == EncodeError::OutOfRange);
  CHECK(failure([](Buffer b) { return encode::throttle(b, 3, 127, true); }) == EncodeError::OutOfRange);
  CHECK(failure([](Buffer b) { return encode::throttle(b, 3, 255, true); }) == EncodeError::OutOfRange);
}

TEST_CASE("emergency stop") {
  CHECK(build([](Buffer b) { return encode::emergency_stop(b, 3, true); }) == "<t 3 -1 1>\n");
  CHECK(build([](Buffer b) { return encode::emergency_stop(b, 3, false); }) == "<t 3 -1 0>\n");
  CHECK(failure([](Buffer b) { return encode::emergency_stop(b, 0, true); }) == EncodeError::OutOfRange);
  CHECK(build([](Buffer b) { return encode::emergency_stop_all(b); }) == "<!>\n");
}

TEST_CASE("query loco") {
  CHECK(build([](Buffer b) { return encode::query_loco(b, 3); }) == "<t 3>\n");
  CHECK(failure([](Buffer b) { return encode::query_loco(b, 0); }) == EncodeError::OutOfRange);
}

TEST_CASE("function") {
  CHECK(build([](Buffer b) { return encode::function(b, 3, 2, true); }) == "<F 3 2 1>\n");
  CHECK(build([](Buffer b) { return encode::function(b, 4014, 0, false); }) == "<F 4014 0 0>\n");
  CHECK(build([](Buffer b) { return encode::function(b, 3, 32767, true); }) == "<F 3 32767 1>\n");
  CHECK(failure([](Buffer b) { return encode::function(b, 3, 32768, true); }) == EncodeError::OutOfRange);
  CHECK(failure([](Buffer b) { return encode::function(b, 0, 2, true); }) == EncodeError::OutOfRange);
}

TEST_CASE("forget") {
  CHECK(build([](Buffer b) { return encode::forget_loco(b, 3); }) == "<- 3>\n");
  CHECK(failure([](Buffer b) { return encode::forget_loco(b, 0); }) == EncodeError::OutOfRange);
  CHECK(build([](Buffer b) { return encode::forget_all_locos(b); }) == "<- 0>\n");
}

TEST_CASE("programming track") {
  CHECK(build([](Buffer b) { return encode::read_cv(b, 29); }) == "<R 29>\n");
  CHECK(build([](Buffer b) { return encode::read_address(b); }) == "<R>\n");
  CHECK(build([](Buffer b) { return encode::write_cv(b, 3, 10); }) == "<W 3 10>\n");
  CHECK(build([](Buffer b) { return encode::write_cv(b, 1024, 255); }) == "<W 1024 255>\n");
  CHECK(build([](Buffer b) { return encode::write_cv(b, 1, 0); }) == "<W 1 0>\n");
  CHECK(build([](Buffer b) { return encode::write_address(b, 4014); }) == "<W 4014>\n");
  CHECK(build([](Buffer b) { return encode::write_address(b, 3); }) == "<W 3>\n");
}

TEST_CASE("programming arguments out of range") {
  CHECK(failure([](Buffer b) { return encode::read_cv(b, 0); }) == EncodeError::OutOfRange);
  CHECK(failure([](Buffer b) { return encode::read_cv(b, 1025); }) == EncodeError::OutOfRange);
  CHECK(failure([](Buffer b) { return encode::write_cv(b, 0, 1); }) == EncodeError::OutOfRange);
  CHECK(failure([](Buffer b) { return encode::write_address(b, 0); }) == EncodeError::OutOfRange);
  CHECK(failure([](Buffer b) { return encode::write_address(b, 10240); }) == EncodeError::OutOfRange);
}

TEST_CASE("write CV on main") {
  CHECK(build([](Buffer b) { return encode::write_cv_main(b, 3, 3, 10); }) == "<w 3 3 10>\n");
  CHECK(build([](Buffer b) { return encode::write_cv_main(b, 10239, 1024, 255); }) == "<w 10239 1024 255>\n");
  CHECK(failure([](Buffer b) { return encode::write_cv_main(b, 0, 3, 10); }) == EncodeError::OutOfRange);
  CHECK(failure([](Buffer b) { return encode::write_cv_main(b, 3, 1025, 10); }) == EncodeError::OutOfRange);
}

// --- Buffers -----------------------------------------------------------------------

TEST_CASE("a buffer exactly the command length is enough") {
  char buf[13];  // "<t 4014 5 1>\n"
  const auto r = encode::throttle(buf, 4014, 5, true);
  REQUIRE(r.ok());
  CHECK(std::string(buf, r.length) == "<t 4014 5 1>\n");
}

TEST_CASE("a buffer one byte short fails and is left untouched") {
  CHECK(failure([](Buffer b) { return encode::throttle(b, 4014, 5, true); }, 12) ==
        EncodeError::BufferTooSmall);
  CHECK(failure([](Buffer b) { return encode::status(b); }, 3) == EncodeError::BufferTooSmall);
}

TEST_CASE("an empty buffer fails without touching memory") {
  const auto r = encode::status(Buffer(nullptr, 0));
  CHECK(r.error == EncodeError::BufferTooSmall);
  CHECK(r.length == 0);
}

TEST_CASE("range errors take priority over buffer size") {
  CHECK(failure([](Buffer b) { return encode::throttle(b, 0, 5, true); }, 1) == EncodeError::OutOfRange);
}

TEST_CASE("the longest commands fit kMaxCommandSize") {
  const std::string longest[] = {
      build([](Buffer b) { return encode::write_cv_main(b, 10239, 1024, 255); }),
      build([](Buffer b) { return encode::function(b, 10239, 32767, true); }),
      build([](Buffer b) { return encode::throttle(b, 10239, 126, true); }),
      build([](Buffer b) { return encode::emergency_stop(b, 10239, true); }),
      build([](Buffer b) { return encode::write_cv(b, 1024, 255); }),
  };
  for (const auto& c : longest) CHECK(c.size() <= kMaxCommandSize);
}

// --- Names -----------------------------------------------------------------------

TEST_CASE("encode errors have readable names") {
  CHECK(std::string(dccex::name(EncodeError::None)) == "None");
  CHECK(std::string(dccex::name(EncodeError::BufferTooSmall)) == "BufferTooSmall");
  CHECK(std::string(dccex::name(EncodeError::OutOfRange)) == "OutOfRange");
  CHECK(std::string(dccex::name(static_cast<EncodeError>(200))) == "?");
}

// --- Consistency with the rest of the library ----------------------------------------

TEST_CASE("every command frames as exactly one message") {
  const std::string commands[] = {
      build([](Buffer b) { return encode::power(b, PowerTarget::Main, true); }),
      build([](Buffer b) { return encode::throttle(b, 4014, 5, true); }),
      build([](Buffer b) { return encode::emergency_stop_all(b); }),
      build([](Buffer b) { return encode::read_address(b); }),
  };
  for (const auto& c : commands) {
    CAPTURE(c);
    std::vector<std::string> messages;
    dccex::Framer<> framer;
    framer.feed(c.data(), c.size(),
                [](void* ctx, dccex::StringView m) {
                  static_cast<std::vector<std::string>*>(ctx)->push_back(sv(m));
                },
                &messages);
    REQUIRE(messages.size() == 1);
    CHECK("<" + messages[0] + ">\n" == c);
  }
}

TEST_CASE("encoded numbers read back exactly, and only in-range arguments succeed") {
  std::mt19937 rng(20261127);
  std::uniform_int_distribution<int> cab(0, 10300);
  std::uniform_int_distribution<int> speed(0, 255);
  std::uniform_int_distribution<int> fn(0, 33000);

  for (int round = 0; round < 2000; ++round) {
    const auto c = static_cast<std::uint16_t>(cab(rng));
    const auto s = static_cast<std::uint8_t>(speed(rng));
    const auto f = static_cast<std::uint16_t>(fn(rng));
    CAPTURE(c);
    CAPTURE(int(s));
    CAPTURE(f);

    char buf[kMaxCommandSize];
    const auto t = encode::throttle(buf, c, s, true);
    CHECK(t.ok() == (c >= 1 && c <= 10239 && s <= 126));
    if (t.ok()) {
      const dccex::Tokenizer tok(dccex::StringView(buf + 1, t.length - 3));  // strip "<", ">\n"
      dccex::Field field;
      std::uint32_t v = 0;
      REQUIRE(tok.at(0, field));
      REQUIRE(field.to_integer(v));
      CHECK(v == c);
      REQUIRE(tok.at(1, field));
      REQUIRE(field.to_integer(v));
      CHECK(v == s);
    }

    const auto fr = encode::function(buf, c, f, false);
    CHECK(fr.ok() == (c >= 1 && c <= 10239 && f <= 32767));
    if (fr.ok()) {
      const dccex::Tokenizer tok(dccex::StringView(buf + 1, fr.length - 3));
      dccex::Field field;
      std::uint32_t v = 0;
      REQUIRE(tok.at(1, field));
      REQUIRE(field.to_integer(v));
      CHECK(v == f);
    }
  }
}

}  // TEST_SUITE
