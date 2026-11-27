# libdccex

A platform-independent C++17 client library for the [DCC-EX](https://dcc-ex.com) native protocol.

libdccex splits an incoming byte stream into protocol messages, decodes them into typed events,
and encodes commands into caller-provided buffers. It does no I/O, allocates no memory, starts no
threads, and uses only the freestanding subset of the C++ standard library, so it can be embedded
in anything from a bare-metal microcontroller to a desktop tool.

This is not an official DCC-EX project. It is written from the published DCC-EX documentation
and the EX-CommandStation source, and aims to be compatible with DCC-EX.

## Status

Early development. Implemented so far:

- **Framer:** splits a byte stream into messages.
- **Tokenizer:** splits a message into an opcode and fields.
- **Decoder:** turns a message into a typed event (loco state, power, CV and address
  results, version, diagnostics), one event per message.

Not yet: the encoder, for building commands.
The design is in [`docs/spec.typ`](docs/spec.typ); build the PDF with
[Typst](https://typst.app): `typst compile docs/spec.typ`.

## Building and testing

Requires CMake 3.21+ and a C++17 compiler. Tests use [doctest](https://github.com/doctest/doctest),
fetched automatically.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

Besides the unit tests, `ctest` runs two design-rule checks:

- `check.include_allowlist`: library headers may only include freestanding standard headers.
- `check.no_allocation`: the library compiled with `-ffreestanding -fno-exceptions -fno-rtti`
  must not reference `operator new`, `malloc`, or exception runtime symbols.

## Using it

The library target is `dccex::dccex`. With CMake `FetchContent`:

```cmake
FetchContent_Declare(libdccex GIT_REPOSITORY https://github.com/mfiore02/libdccex.git GIT_TAG main)
FetchContent_MakeAvailable(libdccex)
target_link_libraries(your_app PRIVATE dccex::dccex)
```

## License

MIT. See [LICENSE](LICENSE).
