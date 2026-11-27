#pragma once

// Traffic captured from a real EX-CommandStation (5.x, Arduino Mega with
// EX-MotorShield8874) through JMRI's DCC++ over TCP server.
//
// The captures were logged as individual frames, so the byte streams below
// rebuild them with the station's "\n" terminator after each message.

#include <string>
#include <vector>

namespace fixtures {

struct Capture {
  std::string bytes;                  // raw stream as received
  std::vector<std::string> messages;  // expected framer output, delimiters stripped
};

inline std::string join_lines(const std::vector<std::string>& messages) {
  std::string out;
  for (const auto& m : messages) {
    out += '<';
    out += m;
    out += ">\n";
  }
  return out;
}

inline Capture make(std::vector<std::string> messages) {
  Capture c;
  c.bytes = join_lines(messages);
  c.messages = std::move(messages);
  return c;
}

// Reply to <1> (track power on): eleven messages within about 300 ms,
// mixing diagnostics, per-track, aggregate, and per-role power states.
inline Capture power_on_burst() {
  return make({
      "* Track A sensOffset=0 *",
      "* Track B sensOffset=0 *",
      "p1 A",
      "p1 B",
      "p1",
      "p1 MAIN",
      "p1 PROG",
      "* LCD2:Power On *",
      "* TRACK A ALERT FAULT *",
      "* TRACK A FAULT PIN (100msec ignore) *",
      "* TRACK A NORMAL (after 20msec/103msec) 30mA *",
  });
}

// Loco broadcasts caused by a WiThrottle client (another throttle) driving
// address 3: function flicker during startup, a whistle, then speed changes.
inline Capture loco_broadcasts() {
  return make({
      "l 3 1 128 512",
      "l 3 1 128 768",
      "l 3 1 128 512",
      "l 3 1 128 768",
      "l 3 1 128 516",
      "l 3 1 128 512",
      "l 3 1 128 513",
      "l 3 1 139 513",
      "l 3 1 139 517",
      "l 3 1 139 513",
      "l 3 1 149 513",
      "l 3 1 154 513",
      "l 3 1 134 513",
  });
}

// Broadcast after this client sent <t 4014 5 1>.
inline Capture throttle_reply() {
  return make({"l 4014 0 134 0"});
}

}  // namespace fixtures
