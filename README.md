# libdccex

A platform-independent C++17 client library for the [DCC-EX](https://dcc-ex.com) native protocol.

libdccex splits an incoming byte stream into protocol messages, decodes them into typed events,
and encodes commands into caller-provided buffers. It does no I/O, allocates no memory, starts no
threads, and uses only the freestanding subset of the C++ standard library, so it can be embedded
in anything from a bare-metal microcontroller to a desktop tool.

This is not an official DCC-EX project. It is written from the published DCC-EX documentation
and the EX-CommandStation source, and aims to be compatible with DCC-EX.

## Status

Early design. See the specification in [`docs/spec.typ`](docs/spec.typ).

Build the PDF with [Typst](https://typst.app):

```sh
typst compile docs/spec.typ
```
