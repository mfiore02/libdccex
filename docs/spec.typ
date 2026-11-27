// libdccex — library specification
// Compile: typst compile docs/spec.typ   (or: typst watch docs/spec.typ)

#set document(title: "libdccex specification", author: "Mike Fiore")
#set page(paper: "us-letter", margin: (x: 2.2cm, y: 2.2cm), numbering: "1")
#set text(size: 10.5pt)
#set heading(numbering: "1.1")
#set par(justify: true)
#show raw.where(block: false): box.with(fill: luma(240), inset: (x: 2pt, y: 0pt), outset: (y: 2pt), radius: 2pt)
#show raw.where(block: true): block.with(fill: luma(245), inset: 8pt, radius: 4pt, width: 100%)
#show table.cell.where(y: 0): strong

#let status(s) = {
  let c = if s == "v0.1" { rgb("#0f6e56") } else if s == "later" { rgb("#854f0b") } else { rgb("#5f5e5a") }
  text(fill: c, weight: "bold", s)
}
#let open(body) = block(fill: rgb("#faeeda"), inset: 8pt, radius: 4pt, width: 100%)[*Open question.* #body]

#align(center)[
  #text(size: 20pt, weight: "bold")[libdccex]
  #v(-6pt)
  #text(size: 12pt)[A platform-independent C++ client library for the DCC-EX native protocol]
  #v(2pt)
  #text(size: 10pt, fill: luma(90))[Specification · Draft 0.1 · 27 November 2026]
]

#outline(depth: 2, indent: auto)
#pagebreak()

= Purpose and scope

libdccex implements the client side of the DCC-EX native protocol: the text protocol spoken by an EX-CommandStation over serial or TCP, and relayed by JMRI's DCC++ over TCP server. It is written to be embedded in anything that compiles C++17: a handheld throttle on a microcontroller, a desktop tool, a test harness. Nothing in the library depends on a particular application, platform, or transport.

== In scope
- Splitting an incoming byte stream into protocol messages.
- Decoding messages into typed events.
- Encoding commands into caller-provided buffers.

== Out of scope
- Any I/O: sockets, serial ports, WiFi, timers.
- Threading, locking, scheduling. The application owns these.
- The command-station side of the protocol.
- The WiThrottle protocol.
- Application state: tracking which locos are in use, caching loco state, user interface, roster management, persistence.

== Non-affiliation
libdccex is not an official DCC-EX project. It is written from the published DCC-EX documentation and the EX-CommandStation source, and aims to be compatible with DCC-EX. It does not use the `EX-` prefix that DCC-EX reserves for its own products.

= Design constraints

These are baseline decisions. Changing one requires updating this section.

#table(
  columns: (auto, 1fr),
  stroke: 0.5pt + luma(200),
  inset: 6pt,
  [Constraint], [Rule],
  [Language], [C++17.],
  [Standard library], [Freestanding only. Permitted headers: `<cstddef>`, `<cstdint>`, `<limits>`, `<type_traits>`, `<new>`. Utility types (fixed array, string view, span) are implemented in-library.],
  [Memory], [No heap. The library never calls `new`, `malloc`, or anything that allocates. All storage is static, on the stack, or provided by the caller. Capacities are compile-time template parameters.],
  [Error handling], [No exceptions. Functions return status codes or result types.],
  [RTTI], [Not used. Must build with `-fno-rtti`.],
  [Threads], [The library starts no threads and takes no locks. An instance is used from one thread at a time.],
  [I/O], [None. Bytes in through `feed()`, events out through callbacks, commands written into caller buffers.],
  [Callbacks], [A function pointer plus a `void*` context. No `std::function`, no virtual dispatch required of the caller.],
  [Enforcement], [CI builds the library with `-ffreestanding -fno-exceptions -fno-rtti`, fails on any include outside the permitted list, and fails if the linked test binary references `operator new`, `malloc`, or related symbols.],
)


= Sources of truth

In order of authority:

+ *DCC-EX native command reference* (dcc-ex.com): command syntax, arguments, reply formats.
+ *DCC-EX throttle developer guide* (dcc-ex.com): client obligations.
+ *EX-CommandStation source*, chiefly `DCCEXParser.cpp` and `CommandDistributor.cpp`: the definitive behavior when the documents are unclear or lag. This draft was checked against version 5.6.6.
+ *Captured traffic* from real command stations, directly and through JMRI. Every supported message gets at least one real capture as a test fixture.

Third-party client libraries are not a source of truth. They may be read for comparison only.

= Architecture

The library is a set of layers. Each layer is usable on its own, and each depends only on the layers above it in this list.

#table(
  columns: (auto, 1fr, auto),
  stroke: 0.5pt + luma(200),
  inset: 6pt,
  [Layer], [Responsibility], [State],
  [Framer], [Accepts arbitrary byte chunks. Emits complete messages delimited by `<` and `>`. Discards bytes outside a message. Enforces a maximum message length and recovers from overflow.], [Fixed buffer],
  [Tokenizer], [Splits one message into an opcode and fields, including quoted strings. Parses fields as integers on request. Structure inside a field, such as `|`-joined callback tags, is left to the decoder.], [None],
  [Decoder], [Turns tokenized messages into typed events and calls the handler. Unknown or unsupported messages go to a catch-all handler, never an error.], [None],
  [Encoder], [Writes commands into a caller-provided buffer. Validates arguments against protocol ranges before writing.], [None],
)

#v(4pt)
Data flow on a client:

```
socket/serial bytes ──▶ Framer ──▶ Tokenizer ──▶ Decoder ──▶ handler callbacks
app calls ──▶ Encoder ──▶ caller buffer ──▶ app writes to socket/serial
```

= Wire protocol

== Framing

- A message starts with `<` and ends with the next `>`. The command station terminates replies with a newline; the library must not depend on it.
- One read from the transport may contain several messages, part of one, or both. The framer must handle every split, down to one byte per `feed()` call.
- Bytes outside a message (newlines, noise) are discarded.
- A `>` outside a message is discarded.
- An empty message, `<>`, is delivered as an empty message.
- A message longer than the framer capacity is dropped whole. The framer resynchronizes at the next `<` and counts the drop.
- A `<` inside an open message abandons the partial message, counts the drop, and starts a new message at that byte.
- Message content is passed through byte for byte. The framer does not interpret it.
- Commands sent by the library are written as `<...>` followed by a newline.

Diagnostic messages have the form `<* text *>`. They are human-readable, may contain any characters except `>`, and are reported to the application as raw text.

== Client obligations

From the throttle developer guide and the command station source:

- *Ignore what you do not understand.* Any message the decoder does not recognize is passed to the catch-all handler and otherwise ignored. It is never treated as a protocol error.
- *No session.* There is no connect or disconnect handshake. A client may send commands at any time after the transport is up.
- *Broadcasts are shared.* Loco and power broadcasts report changes made by every client, including other throttles. A client must treat them as the source of truth, not as acknowledgements of its own commands.
- *One programming operation at a time.* The command station holds a single slot for pending programming-track and loco-ID operations. A second request while one is pending fails with `<X>`. The library neither queues nor tracks pending operations. The application waits for the reply, or for `<X>`, before sending the next one.
- *Programming replies are asynchronous.* They arrive after the decoder answers, typically hundreds of milliseconds to a few seconds later, possibly interleaved with broadcasts.

== Commands (client to command station)

#table(
  columns: (auto, auto, 1fr, auto),
  stroke: 0.5pt + luma(200),
  inset: 5pt,
  [Purpose], [Syntax], [Notes], [Status],
  [Track power on], [`<1>` `<1 MAIN>` `<1 PROG>` `<1 JOIN>`], [No argument powers all tracks. `JOIN` connects PROG to MAIN; the encoder exposes it under an explicitly named function.], status("v0.1"),
  [Track power off], [`<0>` `<0 MAIN>` `<0 PROG>`], [Turning anything off also clears JOIN.], status("v0.1"),
  [Status], [`<s>`], [Replies with version, then power state, turnouts, sensors. The only way to query power state.], status("v0.1"),
  [Throttle], [`<t cab speed dir>`], [Speed `-1` (emergency stop) or `0`..`126`. Direction `1` forward, `0` reverse. The four-field form with a leading register is also accepted by the station; the library emits the three-field form.], status("v0.1"),
  [Query loco], [`<t cab>`], [Requests a loco broadcast for one address.], status("v0.1"),
  [Function], [`<F cab fn state>`], [`state` `1` on, `0` off. F0 to F28 use standard function packets, which the station keeps refreshing. F29 and up use DCC binary state control, sent once and not refreshed.], status("v0.1"),
  [Emergency stop all], [`<!>`], [Sets every known loco to emergency stop and broadcasts.], status("v0.1"),
  [Forget loco], [`<- cab>`], [`<- 0>` forgets all. Releases the station's refresh slot.], status("v0.1"),
  [Read CV (prog track)], [`<R cv>`], [Reply is a verify reply, `<v cv value>`, not `<r ...>`. See notes.], status("v0.1"),
  [Read loco address (prog)], [`<R>`], [Reply `<r address>`.], status("v0.1"),
  [Write CV (prog track)], [`<W cv value>`], [Reply `<r cv value>`, value `-1` on failure.], status("v0.1"),
  [Write loco address (prog)], [`<W address>`], [Writes short or long address, clears consist. Reply `<w address>`.], status("v0.1"),
  [Verify CV (prog track)], [`<V cv value>`], [Reply `<v cv value>`.], status("later"),
  [Write CV bit (prog)], [`<B cv bit value>`], [], status("later"),
  [Write CV on main], [`<w cab cv value>`], [No reply. Cannot be verified without RailCom.], status("v0.1"),
  [Write CV bit on main], [`<b cab cv bit value>`], [No reply.], status("later"),
  [Roster and lists], [`<J R>` and related `J` queries], [Roster comes from the station's EXRAIL definitions, not from JMRI.], status("later"),
  [Track manager], [`<= ...>`], [], status("later"),
  [Turnouts, sensors, outputs], [`<T ...>` `<Q>` `<Z ...>`], [], status("later"),
)

== Replies and broadcasts (command station to client)

#table(
  columns: (auto, auto, 1fr, auto),
  stroke: 0.5pt + luma(200),
  inset: 5pt,
  [Event], [Syntax], [Notes], [Status],
  [Loco state], [`<l cab reg speedbyte functions>`], [Broadcast on any change by any client. See speed byte and function map below. The `reg` field is legacy: the station sends `0`, and `-1` for an unknown loco, but traffic through JMRI has been seen with `1`. The library ignores it.], status("v0.1"),
  [Loco forgotten], [`<- cab>`], [Preceded by a loco broadcast with speed byte `1`.], status("v0.1"),
  [Power, per track], [`<p1 A>` `<p0 B>` ...], [One per track that reports state. Track letters A to H.], status("v0.1"),
  [Power, aggregate], [`<p0>` `<p1>`], [`<p1>` only when every track is on; `<p0>` when none is. When mixed, no aggregate message is sent.], status("v0.1"),
  [Power, by role], [`<p1 MAIN>` `<p1 PROG>` `<p1 JOIN>`], [Only "on" states are reported by role. Off is inferred from per-track and aggregate messages.], status("v0.1"),
  [Version], [`<iDCC-EX V-x.y.z / board / shield G-sha>`], [First line of the `<s>` reply. The library extracts the version number and keeps the rest as text.], status("v0.1"),
  [CV read result], [`<v cv value>`], [Reply to `<R cv>` and `<V cv value>`. Value `-1` means the read failed.], status("v0.1"),
  [CV write result / address], [`<r cv value>` · `<r address>`], [Two shapes under one opcode, told apart by field count. Value `-1` means failure. The address form may read `<r LONG n UNSUPPORTED>` for a long address the decoder cannot hold.], status("v0.1"),
  [Callback-tagged result], [`<rN|M|cv value>`], [Reply to the older forms of `<R>`, `<W>`, `<B>` that carry callback numbers. Decoded for completeness, never sent by the library.], status("v0.1"),
  [Address written], [`<w address>`], [Reply to `<W address>`. Negative on failure.], status("v0.1"),
  [Command failed], [`<X>`], [Sent for any command the station could not parse or accept, including a programming request while another is pending. Not correlated to a specific command.], status("v0.1"),
  [Diagnostic], [`<* text *>`], [Human-readable. Passed through as text.], status("v0.1"),
  [Turnout, sensor, other], [various], [Passed to the catch-all handler until supported.], status("later"),
)

== Speed byte

The speed byte in a loco broadcast is the DCC speed and direction byte, not the value the client sent:

- Bit 7: direction, `1` forward, `0` reverse.
- Bits 0 to 6: DCC speed code. `0` stop, `1` emergency stop, `2` to `127` running.

A client speed of `s` from `1` to `126` appears as code `s + 1`. Client speed `-1` appears as code `1`.

#table(
  columns: (auto, auto, auto, auto),
  stroke: 0.5pt + luma(200),
  inset: 5pt,
  [Sent], [Speed byte], [Direction], [Client speed],
  [`<t 4014 5 1>`], [`134`], [forward], [5],
  [(another client, forward 10)], [`139`], [forward], [10],
  [(stopped, forward)], [`128`], [forward], [0],
  [(forgotten)], [`1`], [reverse], [emergency stop],
)

The first three rows come from captured traffic.

== Function map

The functions field is an unsigned decimal bitmask, F0 in bit 0. The station stores 32 bits of function state, so broadcasts report F0 to F31. Functions above F31 can be set with `<F>`, but the station keeps no state for them and never broadcasts them. Decoded function state is therefore a 32-bit word.

Captured example: `513` is F0 and F9 on; `517` adds F2.


= Relay through JMRI

Clients may connect through JMRI's DCC++ over TCP server instead of directly to the station. The library must work in both cases. Observed through JMRI so far:

- Commands sent by the client reach the station and take effect.
- Power and loco broadcasts reach the client, including those caused by other throttles connected to JMRI over WiThrottle.
- Diagnostic messages are relayed.
- The `reg` field of loco broadcasts can differ from what the station itself sends.

Not yet verified through JMRI: replies to programming-track commands (`<v>`, `<r>`, `<w>`), and whether they reach only the requesting client or every client. The library must not assume either. The integration test plan includes both.

= Programming interface (C++)

Names are provisional. This section fixes the shape, not the spelling.

== Framer

```cpp
namespace dccex {

template <std::size_t MaxMessage = 256>
class Framer {
public:
  using Callback = void (*)(void* ctx, StringView message);

  static constexpr std::size_t capacity();   // == MaxMessage

  // Consumes all len bytes. Calls on_message once per complete message with
  // the text between '<' and '>' (exclusive). on_message may be null.
  void feed(const char* data, std::size_t len, Callback on_message, void* ctx);

  // Messages discarded for overflow or abandoned by a new '<'. Saturates.
  std::uint32_t dropped() const;

  // Back to the initial state: partial message discarded, count cleared.
  void reset();
};

}
```

The view passed to the callback points into the framer's buffer and is valid only until the callback returns. The callback must not call `feed()` or `reset()` on the same framer. Storage is `MaxMessage` bytes plus a few words of state.

== Tokenizer

The opcode is the first character of a message, and fields begin immediately after it. In `p1 MAIN` the opcode is `p` and the fields are `1` and `MAIN`; in `jR 3` the fields are `R` and `3`.

- Fields are separated by runs of spaces, tabs, CR or LF.
- A field starting with `"` runs to the next `"` and may contain whitespace. The quotes are not part of the field. A quote anywhere else is an ordinary character. There are no escapes, matching the station, which never escapes quotes.
- An unterminated quote ends iteration and marks the message malformed.
- `rest()` returns everything after the opcode, for messages that are free text: diagnostics and the version string.

```cpp
struct Field {
  StringView text;     // view into the message, quotes excluded
  bool quoted;

  bool equals(StringView other) const;    // exact, case-sensitive
  template <typename T> bool to_integer(T& out) const;
};

class Tokenizer {
public:
  explicit Tokenizer(StringView message);

  char opcode() const;          // '\0' for an empty message
  StringView rest() const;      // after the opcode, leading whitespace removed
  bool next(Field& out);        // false at end or unterminated quote
  void rewind();
  std::size_t count() const;    // independent of iteration position
  bool at(std::size_t index, Field& out) const;
  bool malformed() const;       // set once iteration hits an unterminated quote
};
```

The tokenizer stores only a view and a position. Fields are views into the message, so the message must outlive them; in practice the decoder tokenizes inside the framer callback. `count()` and `at()` rescan from the start, which is cheap at DCC-EX message sizes.

`to_integer` accepts a plain decimal integer: an optional `-` for signed types, then one or more digits. It rejects `+`, hexadecimal, embedded whitespace, quoted fields, and any value that does not fit the target type, leaving `out` unchanged. The target type is the caller's choice: `std::uint32_t` for function maps, `std::int16_t` for CV values that may be `-1`.

== Decoder and handler

```cpp
struct LocoState   { std::uint16_t cab; std::int8_t speed; bool forward;
                     bool estop; std::uint32_t functions; };
struct PowerState  { char track; bool on; };          // 'A'..'H'
struct CvResult    { std::int16_t cv; std::int16_t value; bool ok; };
struct Version     { std::uint8_t major, minor, patch; StringView detail; };

struct Handler {
  void* ctx;
  void (*loco)(void* ctx, const LocoState&);
  void (*power_track)(void* ctx, const PowerState&);
  void (*power_all)(void* ctx, bool on);
  void (*power_role)(void* ctx, Role role);           // MAIN, PROG, JOIN on
  void (*cv_read)(void* ctx, const CvResult&);
  void (*cv_written)(void* ctx, const CvResult&);
  void (*address_read)(void* ctx, std::int16_t address);
  void (*address_written)(void* ctx, std::int16_t address);
  void (*version)(void* ctx, const Version&);
  void (*failed)(void* ctx);                          // <X>
  void (*diagnostic)(void* ctx, StringView text);
  void (*unhandled)(void* ctx, StringView raw);       // everything else
};

Result decode(StringView msg, const Handler& h);
```

Any null callback is skipped. A message that matches a known opcode but has malformed fields goes to `unhandled` and is counted; it is never silently reinterpreted.

== Encoder

```cpp
// Each returns the number of bytes written, or an error if the arguments are
// out of range or the buffer is too small. Nothing is written on error.
Result<std::size_t> throttle(Span<char> out, std::uint16_t cab,
                             std::int8_t speed, bool forward);
Result<std::size_t> function(Span<char> out, std::uint16_t cab,
                             std::uint8_t fn, bool on);
Result<std::size_t> power(Span<char> out, PowerTarget target, bool on);
Result<std::size_t> read_cv(Span<char> out, std::uint16_t cv);
Result<std::size_t> write_cv(Span<char> out, std::uint16_t cv, std::uint8_t value);
Result<std::size_t> write_cv_main(Span<char> out, std::uint16_t cab,
                                  std::uint16_t cv, std::uint8_t value);
Result<std::size_t> emergency_stop_all(Span<char> out);
// ... one function per supported command
```

Argument ranges enforced by the encoder:

#table(
  columns: (auto, auto, 1fr),
  stroke: 0.5pt + luma(200),
  inset: 5pt,
  [Argument], [Range], [Source],
  [cab], [1 to 10239], [DCC long address range; the station rejects larger values],
  [speed], [-1 to 126], [Station rejects others],
  [function], [0 to 32767], [Station accepts any non-negative number; F29 and up use binary state control],
  [cv], [1 to 1024], [DCC CV range],
  [value], [0 to 255], [One byte],
)

= Errors

All fallible operations return a small result type carrying either a value or one of:

#table(
  columns: (auto, 1fr),
  stroke: 0.5pt + luma(200),
  inset: 5pt,
  [Code], [Meaning],
  [`BufferTooSmall`], [Encoder output buffer cannot hold the command.],
  [`OutOfRange`], [An encoder argument is outside its protocol range.],
  [`Malformed`], [A message with a known opcode had unparseable fields.],
)

Framing problems are not errors either: the framer counts dropped messages and keeps going.

Protocol-level failures reported by the station (`<X>`, a `-1` CV value) are events, not library errors.

= Testing

- *Host unit tests* for every layer, built with the host compiler.
- *Captured traffic as fixtures.* Real byte streams from the layout, replayed through the framer and decoder, with expected events. Initial captures: the power-on burst after `<1>`, loco broadcasts from a WiThrottle client, and the `<t>` reply to a direct throttle command.
- *Split-point tests.* Every fixture is also fed one byte at a time, and at every possible split into two chunks.
- *Chunking property.* Random delimiter-heavy streams must produce identical messages and drop counts whether fed whole or in random chunks.
- *Fuzzing.* The framer and decoder are fuzzed with libFuzzer on the host. The only requirement is no crash, no out-of-bounds access, and no unbounded loop.
- *Freestanding check.* A CI job compiles the library with `-ffreestanding -fno-exceptions -fno-rtti` and an include allow-list.
- *Embedded smoke build.* A CI job cross-compiles for at least one bare-metal target (for example Cortex-M with `arm-none-eabi-g++`) to catch toolchain differences early.

= Build and packaging

- CMake, minimum version to be chosen with the first scaffold. One library target, `dccex::dccex`.
- Consumable through `FetchContent`, a git submodule, or an installed CMake package.
- Semantic versioning from 0.1.0. Breaking changes are expected before 1.0.
- Licensed under MIT. DCC-EX itself is GPL-3.0; libdccex is an independent implementation written from the published protocol documentation and contains no DCC-EX code.

= Open questions

#table(
  columns: (auto, 1fr),
  stroke: 0.5pt + luma(200),
  inset: 5pt,
  [Topic], [Question],
  [C interface], [An `extern "C"` wrapper for use from other languages. Deferred.],
  [Programming replies via JMRI], [Are `<v>`, `<r>`, `<w>` relayed, and to whom.],
  [Roster], [Station roster (`<J R>`) versus JMRI roster (web API) as the throttle's source of loco names.],
)
