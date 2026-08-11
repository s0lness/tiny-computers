# devlink

A USB-serial link so an agent can see the screen and drive the touchscreen
without a human. Two halves:

- `firmware/devlink.c` / `firmware/devlink.h`: a small command interpreter
  that reads lines from the same USB CDC port `main.c` already prints debug
  output to, and answers them.
- `tools/dev.ts`: a `bun` CLI that talks to it.

**Status: the host side (`tools/dev.ts`) has not been run against real
hardware.** It was written while the board's serial port was in use by
another process and could not be opened. The firmware side (`devlink.c`)
does compile cleanly against the real pico-sdk headers (verified with
`ninja CMakeFiles/main.dir/devlink.c.obj` in `firmware/build`), and the
host-side logic that does not touch the serial port (the line reader, the
RLE decoder, the PNG encoder) was verified end to end against a fake device
process that speaks the exact same protocol over stdio. What is genuinely
untested is the PowerShell `System.IO.Ports.SerialPort` bridge itself.
Treat first use as a bring-up session, not a known-good tool.

## Wire protocol

One command per line, ASCII, case-insensitive, terminated by `\n` or `\r`
(the firmware accepts either and treats a run of them as one terminator).
Replies are `\r\n`-terminated, matching the rest of `main.c`'s output on
this port. Because devlink shares the port with `main.c`'s existing debug
prints (`prof ...`, `INT high ...`, etc.), a reader has to be prepared to
see those lines interleaved with devlink replies at any time; they are not
part of this protocol and should be treated as ignorable noise by anything
parsing devlink's own replies (`tools/dev.ts` does this naturally: it only
ever looks for the specific reply shape it is waiting for, at the point in
the exchange it expects it).

### Commands

| Command | Reply |
|---|---|
| `PING` | `OK devlink <version> <w> <h>` |
| `SHOT` | see below |
| `DOWN <x> <y>` | `OK` (or `ERR args` if x/y do not parse) |
| `MOVE <x> <y>` | `OK` (or `ERR args`) |
| `UP` | `OK` |
| `TAP <x> <y>` | `OK` (internally: DOWN then UP) |
| `ERASE` | `OK` |
| anything else | `ERR unknown <cmd>` |

`<version>` is the devlink protocol version (currently `1`), independent of
firmware version. `<w>` and `<h>` are the framebuffer dimensions in pixels
(368x448 on this board).

Coordinates are panel pixel coordinates, `x` in `[0, w)`, `y` in `[0, h)`.
Whoever wires up the hooks in `main.c` is responsible for clamping (see the
integration notes below); devlink itself does not.

### SHOT

Requests a screenshot. The panel is neutral grey throughout (white paper,
black ink), so an 8-bit greyscale image loses nothing that matters, and
compresses very well with plain run-length encoding.

Reply shape:

```
SHOT <w> <h> <rle_byte_count>
<base64, wrapped at 76 chars per line>
...
END
```

`<rle_byte_count>` is the exact length in bytes of the RLE stream *before*
base64 (i.e. `2 * number_of_runs`). It is known up front because the
firmware walks the framebuffer twice: once just to count, once to stream
the actual bytes; it never buffers the whole RLE stream in RAM (there is
already a 330KB framebuffer sitting in 520KB of SRAM, so headroom is
tight).

**Getting an 8-bit grey level from a stored pixel.** The framebuffer is
`uint16_t` RGB565, but stored byte-swapped relative to how the CPU holds a
`uint16_t`, because the panel wants the opposite byte order and the buffer
is DMA'd out raw (see `main.c`'s `px_swap` for the full story: this only
started mattering once anti-aliasing introduced non-palindromic pixel
values). To recover the grey level:

```c
uint16_t v = (px >> 8) | (px << 8);       // undo the storage byte-swap
uint8_t  g = (uint8_t)(((v >> 5) & 0x3F) << 2); // 6-bit green -> 8-bit grey
```

**RLE.** The framebuffer is walked in row-major order (index `y*w+x`, i.e.
exactly the flat layout of the `fb` pointer). Each run is emitted as two
bytes: `(value, count)`, `count` in `1..255`. A run of 256+ identical
pixels is split into multiple `(value, 255)` pairs. There is no
end-of-stream marker inside the RLE bytes themselves; the byte count in the
`SHOT` header is what tells a decoder when to stop. A blank white screen
(368*448 = 164864 pixels, all the same value) encodes as a handful of
`(255, 255)` pairs, i.e. well under 100 bytes before base64: "tiny payload"
in the spec is not an exaggeration.

**Base64.** Standard alphabet (`A-Za-z0-9+/`, `=` padding), applied to the
raw RLE byte stream, wrapped at 76 characters per output line. The last
line may be shorter. `END` on its own line closes the block. A decoder
should: concatenate every base64 line between the `SHOT ...` header and
`END`, base64-decode the result, then RLE-decode using the byte count from
the header as a sanity check (it should exactly match the decoded byte
length; `tools/dev.ts` warns if it does not, since that means the transfer
was corrupted or truncated).

### Framing and non-blocking behaviour

`devlink_poll()` (called once per main-loop iteration) never blocks reading
input: it drains `getchar_timeout_us(0)` in a loop until that returns
`PICO_ERROR_TIMEOUT`, dispatching each complete line as it is found. The
only place it spends non-trivial time is finishing a `SHOT` reply, since
that involves two full passes over the framebuffer and printing
potentially tens of KB of base64; there is no way to make emitting a
screenshot instantaneous, but it never waits on anything external (no
retries, no polling a peripheral), so it cannot stall the main loop
indefinitely the way e.g. a blocked I2C transaction could.

Lines longer than 95 characters are dropped and the parser resyncs on the
next line terminator, rather than acting on a truncated command. No real
command comes close to that length.

## Firmware integration

`devlink_init()` takes a `devlink_hooks_t` (see `firmware/devlink.h`):
the framebuffer pointer + dimensions (read-only, for `SHOT`), and four
function pointers owned by `main.c` that turn `DOWN`/`MOVE`/`UP`/`ERASE`
into actual screen changes. Call `devlink_init()` once, after the
framebuffer is allocated; call `devlink_poll()` once per loop iteration.

The hooks are intentionally *not* wired into `main.c`'s own
`fingerDown`/`lastRawX`/`lastRawY` touch state machine (glitch rejection,
stall watchdog, etc.) — that state is local to `main()` and reworking it
was out of scope for a non-invasive add-on. Instead the hooks call the
same drawing primitives real touch uses (`stroke_begin` / `stroke_sample`
/ `stroke_end` / `wipe_erase`) directly, with their own independent dirty
rect, and push it immediately. Practically: **do not drive a real finger
and devlink at the same time**; if that ever needs to change, the fix is
promoting `fb`, `patternShown`, `fingerDown`, `lastRawX`, `lastRawY` to a
single shared touch-input surface both paths feed into, which is a real
refactor, not a hook wiring change. See the diff below for exactly what
was chosen and why (search the accompanying report for the main.c diff;
it is not applied to this repo since editing `main.c` was off limits for
whoever generated this module).

## Host CLI (`tools/dev.ts`)

```
bun tools/dev.ts ping
bun tools/dev.ts shot out.png
bun tools/dev.ts tap 184 224
bun tools/dev.ts draw 20,20 60,40 100,30 140,60
bun tools/dev.ts erase
bun tools/dev.ts log 15
```

Port and baud are overridable: `DEVLINK_PORT` (default `COM4`),
`DEVLINK_BAUD` (default `115200`).

### Why a spawned PowerShell process

Bun has no built-in serial port API on Windows, and this repo carries no
native serial dependency (deliberately: this machine is Windows on ARM64,
where prebuilt native npm serial bindings are a frequent source of pain,
see `AGENTS.md`'s toolchain notes for the same problem in a different
guise). `System.IO.Ports.SerialPort` via PowerShell is already the
pattern this repo's own gotcha list points at (`AGENTS.md`: "Serial needs
DTR"), so `dev.ts` spawns a small PowerShell script
(`rp2350-devlink-bridge.ps1`, written to a temp file at run time from a
string constant in `dev.ts`, not committed separately) that:

1. Opens the port with `DtrEnable = $true` set **before** `Open()`. Getting
   this order wrong is the single most likely reason a first real run
   would fail silently (the port opens, but the device never responds,
   and it looks exactly like a dead board).
2. Relays serial bytes to its own stdout on a background PowerShell
   runspace (so reading the port never blocks writing to it).
3. Reads lines from its own stdin on the main thread and writes each as a
   line to the port.

`dev.ts` talks to that child process exactly like it would talk to the
device directly: write a command line to the child's stdin, read reply
lines from the child's stdout. All of the protocol logic (line framing,
RLE/base64 decoding, PNG encoding) is independent of that transport and
was verified against a fake stand-in process; only the three bullet points
above (real `SerialPort` behaviour) are unverified.

### PNG encoding

No image library. `encodeGreyPNG()` in `dev.ts` builds a minimal 8-bit
greyscale PNG by hand: one `IDAT` chunk, filter type `None` on every
scanline, compressed with `Bun.deflateSync`. One wrinkle: `Bun.deflateSync`
produces raw DEFLATE (RFC 1951), but PNG's `IDAT` requires a zlib-wrapped
stream (RFC 1950: a 2-byte header plus a 4-byte Adler-32 trailer), so those
are added by hand around the compressed bytes. This was verified during
development by decoding the resulting PNG independently through Node's
`zlib.inflateSync` (a decoder this repo did not write) and comparing every
recovered pixel back to the source image; it is not a claim taken on
faith.

### Known limitations

- Untested against real hardware (see Status above).
- `draw` paces `MOVE` commands 20ms apart; real touch reports arrive at
  roughly 58Hz (~17ms) per `main.c`'s profiling notes, so this is in the
  right ballpark but not tuned against a real stroke.
- `log` has no way to distinguish "device idle" from "bridge died"; both
  show up as a read timeout. If output stops unexpectedly, check that the
  PowerShell child is still alive (Task Manager, or rerun with
  `DEVLINK_PORT` pointed at a definitely-wrong port name and confirm you
  get an immediate "failed to open" instead of a silent hang, as a smoke
  test of the error path).
