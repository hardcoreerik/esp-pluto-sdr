# Driver API reference

Applies to the experimental `0.1.0-dev` implementation on
`chatgpt/esp-pluto-sdr-driver`. Public declarations live in
[esp_pluto_sdr.h](../include/esp_pluto_sdr.h) and
[esp_pluto_tcp.h](../include/esp_pluto_tcp.h). This is a development API:
function signatures and public struct layout may change before a stable release.

## Implemented contract

The component is a synchronous legacy-text IIOD client. It retrieves context XML,
writes channel attributes, and receives raw scan bytes from one selected device.
It has no OrcSDR, display or board-support dependency. Networking is supplied by
the application or the optional TCP adapter. There is no binary-protocol
negotiation, automatic discovery, XML parser, attribute readback, normalized IQ,
TX API, USB RNDIS setup or asynchronous worker in this version.

Device names and configuration must come from the **actual running IIO context**.
Do not assume a fixed `iio:deviceN`, scan mask or signed-16 layout from the chipset
name. The API does not verify that a selected device is an input device. The
caller is responsible for selecting RX only and for appropriate attribute writes.

## Lifecycle and ownership

1. Bring up the board's IP network outside the driver.
2. Connect a transport and initialize one `pluto_client`.
3. Retrieve context, inspect scan layout and configure the radio.
4. Open one receive buffer, read repeatedly, then close the remote buffer.
5. Reuse the connected client for another stream, or disconnect the socket.
6. On a fault, discard the connection and reconnect/reinitialize.

| Resource | Owner and lifetime |
|---|---|
| `pluto_client` | Caller allocates; initialize before any operation; do not copy a live client |
| `pluto_io` | Copied by value; callbacks and `user` pointee must remain valid |
| XML/RX buffers | Caller supplies and retains; functions do not save their pointers |
| Device/channel/attribute/value strings | Borrowed during the call; OPEN copies the device name |
| `pluto_tcp` | Caller allocates; initialize `fd=-1`; keep at a stable address until disconnected |
| Network interface | Application owns setup, addressing, power and teardown |

The core performs no heap allocation. Platform sockets may allocate internally.
There are no internal locks: use **one owning task per client/transport**. Do not
run control calls concurrently with RX or disconnect concurrently to cancel a
read. Context retrieval and writes are disallowed while streaming. Close before
retuning, then reopen; that sequence creates a data discontinuity.

## Functions

| Function | Required state | Success result and effect |
|---|---|---|
| `pluto_init(client, io)` | Fresh storage or discarded session | `void`; zeroes state/counters and copies transport; does not connect or close anything |
| `pluto_context(client, xml, capacity, length)` | Initialized, not streaming | `0`; NUL-terminated XML, `*length` excludes NUL |
| `pluto_attr_write(client, device, direction, channel, attribute, value)` | Initialized, not streaming | `0`; remote accepted exactly the payload length |
| `pluto_rx_open(client, device, samples, mask)` | Initialized, not streaming | `0`; remembers selected device/mask and sets `streaming` |
| `pluto_rx_read(client, buffer, capacity, length)` | Streaming | `0`; `*length` raw bytes, possibly less than capacity or zero |
| `pluto_rx_close(client)` | Streaming | `0`; returns to idle while retaining the transport |
| `pluto_tcp_connect(transport, ipv4, port, timeout_ms, io)` | Fresh/disconnected transport | `0`; connected socket and callbacks borrowing its owner |
| `pluto_tcp_disconnect(transport)` | Initialized socket owner | `void`; shutdown/close and `fd=-1`; no IIOD CLOSE |

### Parameters, units and bounds

- All operation pointers must be valid unless expressly documented otherwise.
  `pluto_init(NULL, io)` and `pluto_tcp_disconnect(NULL)` are no-ops.
- Context capacity includes room for a trailing NUL. If reported XML length is
  at least capacity, retrieval fails and faults the session. XML is not parsed
  or validated. On any error, ignore the XML buffer.
- Device, channel and attribute tokens are 1–63 characters: ASCII letters,
  digits, underscore, colon, hyphen or period. Whitespace is rejected.
- Attribute direction is exactly `INPUT` or `OUTPUT`. `OUTPUT` is an IIO
  channel direction, not a request to transmit: an RX LO can be an output channel.
- Attribute value is a NUL-terminated string of at most 128 characters, limited
  to lowercase ASCII letters, digits, underscore, plus, minus and period. The
  wire payload is `strlen(value)` bytes; its NUL is not sent. Empty values are
  forwarded and may be rejected by the server. No units or value ranges are
  enforced. Frequency/sample rate/bandwidth attributes commonly use Hz; gain
  attributes commonly use dB. Confirm the selected firmware's attribute contract.
- OPEN `samples` is a requested **scan count**, 1..`INT_MAX`, not a byte count.
  The driver does not validate device-specific limits.
- The channel mask is a nonzero 32-bit word for scalar scan channels, not a
  count of complex receivers. More than one mask word is unsupported.
- READBUF capacity is a **byte count**, 1..`INT_MAX`. The caller chooses a
  scan-aligned size. No relation to OPEN scan count is checked locally.
- TCP accepts numeric IPv4 only, nonzero port, timeout 1..60000 milliseconds.
  Hostnames, mDNS and IPv6 are unsupported; resolve them outside the driver.

### Samples and partial reads

The returned buffer is raw interleaved scan storage. Determine scan index,
endianness, storage/significant bits, shift, repeat count, padding and stride
from the context before interpreting it. ADC resolution does not establish the
wire layout. There is no automatic sign extension, deinterleaving or scaling.

One READBUF call can contain several chunks; only the first carries the mask.
This implementation requires the mask to equal the selected word as eight
lowercase hexadecimal digits. A zero chunk ends the call successfully; inspect
`*length` instead of assuming a full buffer. Preserve/reassemble any partial scan
in application code, or reject it explicitly.

`bytes_received` counts complete RX payload chunks since `pluto_init`, excluding
context XML, framing and control traffic. On failure it may already have advanced
for earlier chunks. It is not an RF/DMA loss detector or a complex-sample count.
On any read error, discard this call's buffer: the unfinished final chunk may
have written bytes beyond the reported `*length`. No valid-data guarantee is made
for that failed call.

## Errors and recovery

All `int` operations return 0 on success; there is no positive byte-count return.
Lengths are output parameters. These codes are driver values, **not esp_err_t**;
do not use `ESP_ERROR_CHECK()` as their error-reporting interface.

| Error | Meaning | Recovery |
|---|---|---|
| `PLUTO_INVALID` (-10001) | Invalid parameters, missing callbacks, or already-faulted client | Correct arguments; if faulted reconnect/reinitialize |
| `PLUTO_PROTOCOL` (-10002) | Bad framing, oversized XML/chunk, wrong mask or inconsistent write/status result | Session is faulted; discard and reconnect |
| `PLUTO_IO` (-10003) | Callback/socket failure, EOF, timeout or invalid callback count | Session is faulted for core IO errors; reconnect |
| `PLUTO_STATE` (-10004) | Operation requires idle/streaming state different from current state | Use the documented sequence |
| Other negative status | Remote IIOD error returned unchanged | See operation-specific rules below |

Negative remote context/write/OPEN statuses do not themselves fault an otherwise
synchronized session. Negative READBUF and CLOSE statuses **do** fault it.
A fault sets `faulted=true` and `streaming=false`; later core calls return
`PLUTO_INVALID`. Flags/fields are diagnostic; applications must not edit them.
Future error numbers and remote error namespaces are not a stable ABI guarantee.

`pluto_rx_close` on an idle client returns `PLUTO_STATE`; it is not idempotent.
Socket disconnect on an initialized `fd==-1` owner is idempotent. Do not call
connect again on a live owner: connect resets its stored descriptor without
closing the previous socket. Always disconnect first.

## Transport callback contract

`read(user, buffer, requested)` and `write(user, buffer, requested)` transfer
1..requested bytes. Short transfers are retried until the operation is satisfied.
A zero/negative return or count larger than requested becomes `PLUTO_IO`; custom
callback error values are not propagated. Callbacks must enforce bounded blocking.

The TCP adapter uses a bounded nonblocking connect wait and per-send/per-recv
socket timeouts. It does not implement a total transaction deadline: a slow peer
can extend a command through repeated short reads. It collapses socket errno,
EOF and timeout into `PLUTO_IO`, with no EINTR retry or timeout subtype exposed.
There is no TLS, authentication, automatic reconnect or concurrent cancellation.

## Example with explicit, previously verified RX layout

The application supplies the numeric endpoint, discovered RX device, mask and
scan stride. This example intentionally does not guess board pins or radio IDs.
It can be compiled against the public headers as ordinary C11.

```c
#include "esp_pluto_sdr.h"
#include "esp_pluto_tcp.h"

int capture_one(const char *ip, const char *rx_device,
                uint32_t scan_mask, size_t scan_stride) {
    if (!scan_stride || scan_stride > 16384) return PLUTO_INVALID;
    pluto_tcp tcp = { .fd = -1 };
    pluto_io io;
    int rc = pluto_tcp_connect(&tcp, ip, 30431, 2000, &io);
    if (rc) return rc;
    pluto_client client;
    pluto_init(&client, io);
    /* The application has already verified rx_device, scan_mask and layout.
       Apply any discovered channel controls here, before opening the buffer. */
    rc = pluto_rx_open(&client, rx_device, 4096, scan_mask);
    unsigned char data[16384];
    size_t received = 0;
    size_t capacity = sizeof(data) / scan_stride * scan_stride;
    if (!rc) rc = pluto_rx_read(&client, data, capacity, &received);
    if (!rc && received % scan_stride) rc = PLUTO_PROTOCOL;
    /* Consume data[0..received) here only if rc == 0. Never retain a pointer
       to this local buffer after returning. A zero-byte result is permitted. */
    if (client.streaming) {
        int close_rc = pluto_rx_close(&client);
        if (!rc) rc = close_rc;
    }
    pluto_tcp_disconnect(&tcp);
    return rc;
}
```

See [network_rx](../examples/network_rx/main/main.c) for context retrieval and
repeated calls after application-owned networking starts. Its app_main is a
bring-up placeholder; it does not initialize Ethernet or RNDIS. ESP-IDF
cross-compilation and actual Fishball interoperability remain unverified.

## API change discipline

Every public function should retain Doxygen purpose, state/preconditions,
parameter units/bounds, ownership, return values and failure recovery. New API
behavior must include tests and an update to this guide. Keep implemented,
proposed and hardware-verified capabilities distinct. Stable versioning and
license selection are still pending; no registry release is implied.
