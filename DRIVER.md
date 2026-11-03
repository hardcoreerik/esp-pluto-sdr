# Experimental driver v0.1 development

An independent C11 legacy-IIOD client with an ESP-IDF component definition and
lwIP/POSIX TCP adapter. No OrcSDR dependency and no third-party source copied.

Implemented: bounded context retrieval, channel attribute writes, explicit RX
buffer open/read/close, chunked READBUF framing, single 32-bit channel mask,
partial IO, protocol validation, fault state and byte accounting. The caller
owns buffers and IP networking. The TCP adapter accepts numeric IPv4 and bounds
connect/per-call IO timeouts. It does not implement an overall transaction deadline
or concurrent cancellation. Serialize operations in one owning task.

Before streaming, retrieve context XML and verify the input device ID, scan mask,
layout and settings. XML parsing/discovery, normalized IQ, attribute readback,
automatic AD9361 identification, multiple mask words, USB RNDIS initialization,
async tasks and TX API are not implemented. RX routines operate on the explicitly
selected device; the caller must never pass a TX device ID. Attribute writes are
low-level controls and require the correct discovered channel/direction.

Any malformed/oversized framing or IO failure poisons the connection. Disconnect
and reinitialize before retrying; never reuse a partially consumed stream.
Negative remote errors are returned; local errors are PLUTO_* values. A read may
return a partial block when the server sends zero; inspect the returned byte count.
Do not infer RF loss counters from TCP byte accounting.

## Host verification

Run `sh tests/run.sh` with a C compiler and Python 3. Protocol checks use a
fragmenting transport; TCP checks use a real loopback socket and fragmented peer.
ASan/UBSan are enabled on the core checks. LeakSanitizer is disabled because this
sandbox disallows its process tracing (the core itself performs no allocation).
These tests establish framing behavior, not daemon/firmware interoperability.

## ESP-IDF integration

Add this repo as `components/esp-pluto-sdr`, bring up an application-owned IP netif,
then use `pluto_tcp_connect`, `pluto_init` and `pluto_context`. Use attribute writes
for the discovered radio channels before opening a stream. The `network_rx` example
shows the call sequence after networking is ready; app_main intentionally does
not guess a board's networking setup. Component folder name must be esp-pluto-sdr
for that example's REQUIRES declaration.

No ESP-IDF cross-build, actual IIOD session or physical RF capture has run yet.
Hardware compatibility and sustainable rates remain unverified. Project license
selection is still pending; no component-registry release is made.

## API documentation

See the [complete API reference](docs/API.md) for parameter units, state rules,
error recovery, memory ownership and a compilable usage example. Public headers
also carry Doxygen comments for IDE navigation.
