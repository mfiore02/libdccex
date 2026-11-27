#include <doctest/doctest.h>

#include <cstddef>
#include <random>
#include <string>
#include <vector>

#include "dccex/framer.hpp"
#include "fixtures/captures.hpp"

namespace {

// Collects every message the framer emits.
struct Sink {
  std::vector<std::string> messages;

  static void on_message(void* ctx, dccex::StringView m) {
    static_cast<Sink*>(ctx)->messages.emplace_back(m.data(), m.size());
  }
};

template <std::size_t N>
void feed(dccex::Framer<N>& framer, Sink& sink, const std::string& bytes) {
  framer.feed(bytes.data(), bytes.size(), &Sink::on_message, &sink);
}

template <std::size_t N = 256>
std::vector<std::string> frame_all(const std::string& bytes) {
  dccex::Framer<N> framer;
  Sink sink;
  feed(framer, sink, bytes);
  return sink.messages;
}

using Messages = std::vector<std::string>;

}  // namespace

TEST_SUITE("framer") {

// --- Basic framing ----------------------------------------------------------

TEST_CASE("a complete message is delivered without its delimiters") {
  CHECK(frame_all("<p1>") == Messages{"p1"});
}

TEST_CASE("several messages in one chunk are delivered in order") {
  CHECK(frame_all("<p1 A><p1 B>\n<p1>\n") == Messages{"p1 A", "p1 B", "p1"});
}

TEST_CASE("bytes outside a message are discarded") {
  CHECK(frame_all("garbage\r\n<a>\n  noise <b>tail") == Messages{"a", "b"});
}

TEST_CASE("a closing delimiter outside a message is ignored") {
  CHECK(frame_all("> <a> >>") == Messages{"a"});
}

TEST_CASE("an empty message is delivered as an empty view") {
  CHECK(frame_all("<>") == Messages{""});
}

TEST_CASE("message content is passed through byte for byte") {
  const std::string with_nul("<a\0b>", 5);
  CHECK(frame_all(with_nul) == Messages{std::string("a\0b", 3)});
  CHECK(frame_all("<* x | y \t z *>") == Messages{"* x | y \t z *"});
}

TEST_CASE("feeding nothing does nothing") {
  dccex::Framer<> framer;
  Sink sink;
  framer.feed(nullptr, 0, &Sink::on_message, &sink);
  CHECK(sink.messages.empty());
  CHECK(framer.overflowed() == 0);
  CHECK(framer.abandoned() == 0);
}

// --- Messages split across feed() calls ---------------------------------------

TEST_CASE("a message split across chunks is reassembled") {
  dccex::Framer<> framer;
  Sink sink;
  feed(framer, sink, "<p1");
  CHECK(sink.messages.empty());
  feed(framer, sink, " MA");
  feed(framer, sink, "IN>");
  CHECK(sink.messages == Messages{"p1 MAIN"});
}

TEST_CASE("an opening delimiter at the end of a chunk starts a message") {
  dccex::Framer<> framer;
  Sink sink;
  feed(framer, sink, "<a><");
  feed(framer, sink, "b>");
  CHECK(sink.messages == Messages{"a", "b"});
}

// --- Resynchronization ------------------------------------------------------

TEST_CASE("an opening delimiter inside a message abandons the partial message") {
  dccex::Framer<> framer;
  Sink sink;
  feed(framer, sink, "<abc<def>");
  CHECK(sink.messages == Messages{"def"});
  CHECK(framer.abandoned() == 1);
  CHECK(framer.overflowed() == 0);
}

TEST_CASE("an abandoned partial message is counted once per abandonment") {
  dccex::Framer<> framer;
  Sink sink;
  feed(framer, sink, "<a<b<c>");
  CHECK(sink.messages == Messages{"c"});
  CHECK(framer.abandoned() == 2);
}

// --- Capacity ----------------------------------------------------------------

TEST_CASE("a message exactly at capacity is delivered") {
  dccex::Framer<4> framer;
  Sink sink;
  feed(framer, sink, "<abcd>");
  CHECK(sink.messages == Messages{"abcd"});
  CHECK(framer.overflowed() == 0);
}

TEST_CASE("a message over capacity is dropped whole and counted") {
  dccex::Framer<4> framer;
  Sink sink;
  feed(framer, sink, "<abcde><ok>");
  CHECK(sink.messages == Messages{"ok"});
  CHECK(framer.overflowed() == 1);
  CHECK(framer.abandoned() == 0);
}

TEST_CASE("the tail of an overflowed message is discarded across chunks") {
  dccex::Framer<4> framer;
  Sink sink;
  feed(framer, sink, "<abc");
  feed(framer, sink, "defg");
  feed(framer, sink, "hij>");
  feed(framer, sink, "<ok>");
  CHECK(sink.messages == Messages{"ok"});
  CHECK(framer.overflowed() == 1);
}

TEST_CASE("an opening delimiter during overflow starts a new message") {
  dccex::Framer<4> framer;
  Sink sink;
  feed(framer, sink, "<abcdefg<ok>");
  CHECK(sink.messages == Messages{"ok"});
  CHECK(framer.overflowed() == 1);
  CHECK(framer.abandoned() == 0);  // the overflowed message was already counted
}

TEST_CASE("overflow and abandonment are counted separately") {
  dccex::Framer<4> framer;
  Sink sink;
  feed(framer, sink, "<toolong><a<b<cc><x>");
  CHECK(sink.messages == Messages{"cc", "x"});
  CHECK(framer.overflowed() == 1);
  CHECK(framer.abandoned() == 2);
}

TEST_CASE("capacity is reported") {
  CHECK(dccex::Framer<4>::capacity() == 4);
  CHECK(dccex::Framer<>::capacity() == 256);
}

// --- Callback and state management -------------------------------------------

TEST_CASE("a null callback consumes input without delivering it") {
  dccex::Framer<> framer;
  const std::string bytes = "<a><b>";
  framer.feed(bytes.data(), bytes.size(), nullptr, nullptr);
  CHECK(framer.overflowed() == 0);
  CHECK(framer.abandoned() == 0);

  Sink sink;
  feed(framer, sink, "<c>");
  CHECK(sink.messages == Messages{"c"});
}

TEST_CASE("the context pointer is passed to the callback unchanged") {
  struct Probe {
    void* seen = nullptr;
    static void on_message(void* ctx, dccex::StringView) {
      static_cast<Probe*>(ctx)->seen = ctx;
    }
  } probe;
  dccex::Framer<> framer;
  const std::string bytes = "<x>";
  framer.feed(bytes.data(), bytes.size(), &Probe::on_message, &probe);
  CHECK(probe.seen == &probe);
}

TEST_CASE("reset discards a partial message and clears both counters") {
  dccex::Framer<4> framer;
  Sink sink;
  feed(framer, sink, "<toolong>");
  feed(framer, sink, "<a<par");
  REQUIRE(framer.overflowed() == 1);
  REQUIRE(framer.abandoned() == 1);

  framer.reset();
  CHECK(framer.overflowed() == 0);
  CHECK(framer.abandoned() == 0);
  feed(framer, sink, "tial>");  // tail of the discarded message: outside any message now
  feed(framer, sink, "<ok>");
  CHECK(sink.messages == Messages{"ok"});
}

// --- Real traffic --------------------------------------------------------------

TEST_CASE("captured traffic frames correctly however it is chunked") {
  const fixtures::Capture captures[] = {
      fixtures::power_on_burst(),
      fixtures::loco_broadcasts(),
      fixtures::throttle_reply(),
  };

  for (const auto& capture : captures) {
    CAPTURE(capture.bytes);

    SUBCASE("in one chunk") {
      CHECK(frame_all(capture.bytes) == capture.messages);
    }

    SUBCASE("one byte at a time") {
      dccex::Framer<> framer;
      Sink sink;
      for (char c : capture.bytes) {
        framer.feed(&c, 1, &Sink::on_message, &sink);
      }
      CHECK(sink.messages == capture.messages);
      CHECK(framer.overflowed() == 0);
      CHECK(framer.abandoned() == 0);
    }

    SUBCASE("split into two chunks at every position") {
      for (std::size_t split = 0; split <= capture.bytes.size(); ++split) {
        CAPTURE(split);
        dccex::Framer<> framer;
        Sink sink;
        framer.feed(capture.bytes.data(), split, &Sink::on_message, &sink);
        framer.feed(capture.bytes.data() + split, capture.bytes.size() - split,
                    &Sink::on_message, &sink);
        CHECK(sink.messages == capture.messages);
      }
    }
  }
}

TEST_CASE("all captures back to back frame as one stream") {
  const auto a = fixtures::power_on_burst();
  const auto b = fixtures::loco_broadcasts();
  const auto c = fixtures::throttle_reply();

  Messages expected = a.messages;
  expected.insert(expected.end(), b.messages.begin(), b.messages.end());
  expected.insert(expected.end(), c.messages.begin(), c.messages.end());

  CHECK(frame_all(a.bytes + b.bytes + c.bytes) == expected);
}

// --- Properties ----------------------------------------------------------------

TEST_CASE("output never depends on how the stream is chunked") {
  // Random streams biased toward delimiters, so they exercise overflow,
  // abandonment, empty messages and stray '>' far more often than real traffic.
  std::mt19937 rng(20261127);  // fixed seed: failures are reproducible
  const char alphabet[] = "<<>>abc \n";
  std::uniform_int_distribution<std::size_t> pick(0, sizeof(alphabet) - 2);
  std::uniform_int_distribution<std::size_t> length(0, 200);

  for (int round = 0; round < 500; ++round) {
    std::string bytes(length(rng), '\0');
    for (char& c : bytes) c = alphabet[pick(rng)];
    CAPTURE(bytes);

    dccex::Framer<8> whole;
    Sink expected;
    feed(whole, expected, bytes);

    dccex::Framer<8> chunked;
    Sink actual;
    std::size_t pos = 0;
    while (pos < bytes.size()) {
      std::uniform_int_distribution<std::size_t> step(1, bytes.size() - pos);
      const std::size_t n = step(rng);
      chunked.feed(bytes.data() + pos, n, &Sink::on_message, &actual);
      pos += n;
    }

    CHECK(actual.messages == expected.messages);
    CHECK(chunked.overflowed() == whole.overflowed());
    CHECK(chunked.abandoned() == whole.abandoned());
    for (const auto& m : actual.messages) {
      CHECK(m.size() <= 8);
      CHECK(m.find('<') == std::string::npos);
      CHECK(m.find('>') == std::string::npos);
    }
  }
}

}  // TEST_SUITE
