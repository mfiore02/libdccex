#include <doctest/doctest.h>

#include <cstdint>
#include <ostream>
#include <random>
#include <string>
#include <vector>

#include "dccex/tokenizer.hpp"

namespace {

struct F {
  std::string text;
  bool quoted;
  bool operator==(const F& o) const { return text == o.text && quoted == o.quoted; }
};

std::ostream& operator<<(std::ostream& os, const F& f) {
  return os << (f.quoted ? "\"" : "") << f.text << (f.quoted ? "\"" : "");
}

dccex::StringView sv(const std::string& s) { return dccex::StringView(s.data(), s.size()); }

std::string str(dccex::StringView v) { return std::string(v.data(), v.size()); }

// All fields from a fresh tokenizer, via next().
std::vector<F> fields_of(const std::string& message) {
  dccex::Tokenizer t(sv(message));
  std::vector<F> out;
  dccex::Field f;
  while (t.next(f)) out.push_back({str(f.text), f.quoted});
  return out;
}

F u(const char* s) { return {s, false}; }  // unquoted
F q(const char* s) { return {s, true}; }   // quoted

using Fields = std::vector<F>;

template <typename T>
bool parse(const std::string& text, T& out, bool quoted = false) {
  dccex::Field f;
  f.text = sv(text);
  f.quoted = quoted;
  return f.to_integer(out);
}

}  // namespace

TEST_SUITE("tokenizer") {

// --- Opcode --------------------------------------------------------------------

TEST_CASE("the opcode is the first character of the message") {
  const std::string m = "l 3 1 128 512";
  CHECK(dccex::Tokenizer(sv(m)).opcode() == 'l');
}

TEST_CASE("an empty message has a null opcode and no fields") {
  const std::string m;
  dccex::Tokenizer t(sv(m));
  CHECK(t.opcode() == '\0');
  CHECK(t.count() == 0);
  CHECK(t.rest().empty());
  dccex::Field f;
  CHECK_FALSE(t.next(f));
}

TEST_CASE("a message with only an opcode has no fields") {
  CHECK(fields_of("X").empty());
  const std::string m = "X";
  CHECK(dccex::Tokenizer(sv(m)).count() == 0);
}

// --- Fields --------------------------------------------------------------------

TEST_CASE("fields are separated by spaces") {
  CHECK(fields_of("l 3 1 128 512") == Fields{u("3"), u("1"), u("128"), u("512")});
}

TEST_CASE("the first field may follow the opcode without a space") {
  CHECK(fields_of("p1") == Fields{u("1")});
  CHECK(fields_of("p1 MAIN") == Fields{u("1"), u("MAIN")});
  CHECK(fields_of("jR 3 4014") == Fields{u("R"), u("3"), u("4014")});
}

TEST_CASE("runs of whitespace, tabs, CR and LF all separate fields") {
  CHECK(fields_of("l  3\t1 \r\n128   512  ") == Fields{u("3"), u("1"), u("128"), u("512")});
}

TEST_CASE("pipe-joined callback tags stay one field") {
  CHECK(fields_of("r1|2|3 4") == Fields{u("1|2|3"), u("4")});
}

TEST_CASE("count does not disturb iteration") {
  const std::string m = "l 3 1 128 512";
  dccex::Tokenizer t(sv(m));
  dccex::Field f;
  REQUIRE(t.next(f));
  CHECK(t.count() == 4);
  REQUIRE(t.next(f));
  CHECK(str(f.text) == "1");
}

TEST_CASE("rewind restarts iteration at the first field") {
  const std::string m = "p1 A";
  dccex::Tokenizer t(sv(m));
  dccex::Field f;
  REQUIRE(t.next(f));
  REQUIRE(t.next(f));
  CHECK_FALSE(t.next(f));
  t.rewind();
  REQUIRE(t.next(f));
  CHECK(str(f.text) == "1");
}

TEST_CASE("fields can be fetched by index") {
  const std::string m = "l 4014 0 134 0";
  dccex::Tokenizer t(sv(m));
  dccex::Field f;
  REQUIRE(t.at(0, f));
  CHECK(str(f.text) == "4014");
  REQUIRE(t.at(2, f));
  CHECK(str(f.text) == "134");
  CHECK_FALSE(t.at(4, f));
}

TEST_CASE("field views point into the original message") {
  const std::string m = "p1 MAIN";
  dccex::Tokenizer t(sv(m));
  dccex::Field f;
  REQUIRE(t.at(1, f));
  CHECK(f.text.data() == m.data() + 3);
  CHECK(f.text.size() == 4);
}

// --- Quoted strings --------------------------------------------------------------

TEST_CASE("a quoted string is one field without its quotes") {
  CHECK(fields_of(R"(jR 4014 "UP 4014 Big Boy" "Lights/Bell/*Whistle")") ==
        Fields{u("R"), u("4014"), q("UP 4014 Big Boy"), q("Lights/Bell/*Whistle")});
}

TEST_CASE("an empty quoted string is an empty quoted field") {
  CHECK(fields_of(R"(jR 3 "" "")") == Fields{u("R"), u("3"), q(""), q("")});
}

TEST_CASE("a quote only opens a string at the start of a field") {
  CHECK(fields_of(R"(x a"b c")") == Fields{u("a\"b"), u("c\"")});
}

TEST_CASE("a closing quote ends the field even without a following space") {
  CHECK(fields_of(R"(x "ab"cd)") == Fields{q("ab"), u("cd")});
}

TEST_CASE("quoted text keeps inner whitespace exactly") {
  CHECK(fields_of("x \"  two  spaces \"") == Fields{q("  two  spaces ")});
}

// --- Malformed input ------------------------------------------------------------

TEST_CASE("an unterminated quote stops iteration and marks the message malformed") {
  const std::string m = R"(jR 3 "no end)";
  dccex::Tokenizer t(sv(m));
  CHECK_FALSE(t.malformed());

  dccex::Field f;
  REQUIRE(t.next(f));
  CHECK(str(f.text) == "R");
  REQUIRE(t.next(f));
  CHECK(str(f.text) == "3");
  CHECK_FALSE(t.next(f));
  CHECK(t.malformed());
}

TEST_CASE("count reports the fields before an unterminated quote") {
  const std::string m = R"(jR 3 "no end)";
  CHECK(dccex::Tokenizer(sv(m)).count() == 2);
}

// --- Rest of message (free text) -------------------------------------------------

TEST_CASE("rest is everything after the opcode, leading whitespace removed") {
  const std::string version = "iDCC-EX V-5.6.6 / MEGA / EX8874 G-c389fe9";
  CHECK(str(dccex::Tokenizer(sv(version)).rest()) == "DCC-EX V-5.6.6 / MEGA / EX8874 G-c389fe9");

  const std::string diag = "* TRACK A ALERT FAULT *";
  CHECK(str(dccex::Tokenizer(sv(diag)).rest()) == "TRACK A ALERT FAULT *");
}

TEST_CASE("rest of an opcode-only message is empty") {
  const std::string m = "X";
  CHECK(dccex::Tokenizer(sv(m)).rest().empty());
}

// --- Field helpers -----------------------------------------------------------------

TEST_CASE("equals compares field text exactly") {
  const std::string m = "p1 MAIN";
  dccex::Tokenizer t(sv(m));
  dccex::Field f;
  REQUIRE(t.at(1, f));
  CHECK(f.equals("MAIN"));
  CHECK_FALSE(f.equals("main"));
  CHECK_FALSE(f.equals("MAI"));
  CHECK_FALSE(f.equals("MAINS"));
}

TEST_CASE("decimal integers parse") {
  std::int32_t v = 99;
  CHECK(parse("0", v));
  CHECK(v == 0);
  CHECK(parse("126", v));
  CHECK(v == 126);
  CHECK(parse("-1", v));
  CHECK(v == -1);
  CHECK(parse("007", v));
  CHECK(v == 7);
  CHECK(parse("-0", v));
  CHECK(v == 0);
}

TEST_CASE("integers parse at the exact limits of the target type") {
  std::int32_t i32 = 0;
  CHECK(parse("2147483647", i32));
  CHECK(i32 == INT32_MAX);
  CHECK(parse("-2147483648", i32));
  CHECK(i32 == INT32_MIN);

  std::uint32_t u32 = 0;
  CHECK(parse("4294967295", u32));
  CHECK(u32 == UINT32_MAX);

  std::int8_t i8 = 0;
  CHECK(parse("-128", i8));
  CHECK(i8 == -128);
  CHECK(parse("127", i8));
  CHECK(i8 == 127);

  std::uint16_t u16 = 0;
  CHECK(parse("65535", u16));
  CHECK(u16 == 65535);
}

TEST_CASE("integers out of range for the target type are rejected") {
  std::int32_t i32 = 42;
  CHECK_FALSE(parse("2147483648", i32));
  CHECK_FALSE(parse("-2147483649", i32));
  CHECK_FALSE(parse("99999999999999999999999", i32));
  CHECK(i32 == 42);  // unchanged on failure

  std::uint32_t u32 = 42;
  CHECK_FALSE(parse("4294967296", u32));
  CHECK_FALSE(parse("-1", u32));
  CHECK(u32 == 42);

  std::int8_t i8 = 1;
  CHECK_FALSE(parse("128", i8));
  CHECK_FALSE(parse("-129", i8));
}

TEST_CASE("text that is not a plain decimal integer is rejected") {
  std::int32_t v = 42;
  CHECK_FALSE(parse("", v));
  CHECK_FALSE(parse("-", v));
  CHECK_FALSE(parse("+5", v));
  CHECK_FALSE(parse("12a", v));
  CHECK_FALSE(parse("1.5", v));
  CHECK_FALSE(parse("0x10", v));
  CHECK_FALSE(parse("--1", v));
  CHECK_FALSE(parse("1|2|3", v));
  CHECK(v == 42);
}

TEST_CASE("a quoted field is text, never an integer") {
  std::int32_t v = 42;
  CHECK_FALSE(parse("5", v, /*quoted=*/true));
  CHECK(v == 42);
}

// --- Properties ----------------------------------------------------------------

TEST_CASE("fields are consistent for random messages") {
  std::mt19937 rng(20261127);  // fixed seed: failures are reproducible
  const char alphabet[] = "ab1-\"  \t|";
  std::uniform_int_distribution<std::size_t> pick(0, sizeof(alphabet) - 2);
  std::uniform_int_distribution<std::size_t> length(0, 40);

  for (int round = 0; round < 2000; ++round) {
    std::string m(length(rng), '\0');
    for (char& c : m) c = alphabet[pick(rng)];
    CAPTURE(m);

    dccex::Tokenizer t(sv(m));
    dccex::Field f;
    std::size_t n = 0;
    const char* last_end = m.data() + (m.empty() ? 0 : 1);
    while (t.next(f)) {
      // Every field lies inside the message, after the previous one.
      CHECK(f.text.data() >= last_end);
      CHECK(f.text.data() + f.text.size() <= m.data() + m.size());
      last_end = f.text.data() + f.text.size();

      if (f.quoted) {
        CHECK(str(f.text).find('"') == std::string::npos);
      } else {
        CHECK_FALSE(f.text.empty());
        CHECK(str(f.text).find_first_of(" \t\r\n") == std::string::npos);
      }

      dccex::Field indexed;
      REQUIRE(t.at(n, indexed));
      CHECK(indexed.text.data() == f.text.data());
      ++n;
    }
    CHECK(t.count() == n);
  }
}

}  // TEST_SUITE
