# devlink

A USB-serial link so an agent can see the screen and drive the touchscreen
without a human. Two halves:

- `firmware/devlink.c` / `firmware/devlink.h`: a small command interpreter
  that reads lines from the same USB CDC port the runtime already prints
  debug output to, and answers them.
- `tools/dev.ts`: a `bun` CLI that talks to it.

**Status: `tools/dev.ts` has now been run against real hardware, and the
claim below that used to stand here (that the host "only ever looks for the
specific reply shape it is waiting for" and therefore tolerates the shared
port) was false at the time it was written.** It was written while the
board's serial port was in use by another process and could not be opened,
and it showed: the very first real run (`bun tools/dev.ts shot out.png`)
failed immediately with `unexpected reply to SHOT: prof app=chrono
switch=15287us | loops=217088/s | touch reads=0 ...`, because every read
in `tools/dev.ts` actually just took the next line off the wire and assumed
it was the answer. On a board that also prints a profiler line once a
second on the same port (see "Framing and non-blocking behaviour" below),
that assumption breaks on first contact.

This has since been fixed: every read loop in `tools/dev.ts` (`expectLine()`)
now keeps discarding lines until one actually matches the reply shape it is
waiting for, within an overall timeout, rather than trusting the first line
back. The multi-line `SHOT` body gets the same treatment at the line level:
a real payload line is a run of complete base64 groups (checked with
`isBase64Line()`), so a profiler tick landing between two payload lines is
recognised as not-base64-shaped and skipped without corrupting the RLE
stream. This was verified for real, repeatedly, against the running board
on `COM4`: `ping`, `app`, `switch`, `tap`, `erase` and `shot` all work, and
several back-to-back `shot` captures all decoded to the same correct
368x448, non-blank image (the board was running the stopwatch, reading
`00:00:00`; see "Bring-up notes" below for what that screenshot actually
shows and why it looks sideways).

A second, unrelated bug turned up in the same bring-up session: `Bridge.close()`
killed the PowerShell child but returned before it had actually exited, so
two `bun tools/dev.ts` invocations run back to back (as `a && b`, or by any
script driving several commands) could race for the exclusive `COM4` handle
and the second one would fail to open the port. `close()` now awaits the
child's exit (bounded, in case it is ever wedged) before returning, and
every call site awaits `close()` in its `finally` block.

Treat this as a working tool now, not a bring-up session, though see
"Bring-up notes" for the one open question (a re-enumeration hiccup during
this session that turned out to be an unrelated board reflash, not a
`tools/dev.ts` bug).

## Wire protocol

One command per line, ASCII, case-insensitive, terminated by `\n` or `\r`
(the firmware accepts either and treats a run of them as one terminator).
Replies are `\r\n`-terminated, matching the rest of the runtime's output on
this port. Because devlink shares the port with the runtime's existing debug
prints (`prof ...`, `switch ...`, `stroke start ...`, etc.), a reader has to be prepared to
see those lines interleaved with devlink replies at any time; they are not
part of this protocol and should be treated as ignorable noise by anything
parsing devlink's own replies. `tools/dev.ts` does this by construction, not
by accident: every read loop matches the reply shape it is waiting for
(`expectLine()` in `dev.ts`) and discards anything else, rather than
trusting whatever line comes back first. See "Why this is a host-side fix,
not a firmware one" below for why the noise is left alone at the source.

The one thing this asks of a reader: match the *expected shape*, not "not
a known noise prefix". This firmware already carries more than one source of
unrelated `printf`: the profiler (`runtime.c`), the sketchpad's stroke
tracing (`apps/sketch.c`), and a `BOOT ...` self-test poll (`bootbtn.c`,
`bootbtn_selftest_poll()`) that is not wired into the main loop today but
exists in the tree and could be re-enabled without anyone thinking of it as
a devlink change. A matcher built out of a blacklist of today's noise
prefixes breaks the moment any of that changes; a matcher built out of "is
this what a `PING` reply / a `SHOT` header / a base64 payload line looks
like" does not.

### Why this is a host-side fix, not a firmware one

Two firmware-side options were considered instead: suppressing the profiler
while a devlink exchange is in progress, or prefixing every devlink reply
line with a marker byte noise can never produce. Neither was implemented,
for reasons specific to how this firmware is put together:

- Every `printf` in this firmware, including the profiler tick, `devlink.c`'s
  own replies, and the sketchpad's stroke-debug lines, runs on core0, inside
  the single-threaded main loop (core1 never touches stdio at all; see
  `sensors.c`'s ownership-rule banner). A `SHOT` reply is emitted by one
  blocking call to `devlink_send_shot()` from inside a single call to
  `devlink_poll()`, itself one step of that same loop, so nothing else in
  this firmware can print *into the middle of* an in-flight `SHOT` reply.
  The failure this task actually reproduced (`unexpected reply to SHOT: prof
  app=chrono ...`) was a profiler tick landing where the reply was expected
  to *start*, in the gap between sending the command and the board getting
  around to answering it, not a tick splicing itself into an already-started
  reply. Suppressing the profiler "during an exchange" would not close that
  gap, since the gap exists before the firmware even knows an exchange has
  started.
- A reader that trusts the first line back is broken by *any* unrelated
  print sharing the port, not just the profiler: `BOOT ...` (a physical
  button poll) and `stroke ...`/`erase (shake)` (the sketchpad) are exactly
  as capable of landing in that same gap. Silencing the profiler specifically
  would still leave the same class of bug for every other print, and for
  whatever gets added after this. The fix needs to be general regardless of
  what firmware changes; once it is general, it is also sufficient by
  itself.
- The shared port carries real value as a human-readable console: someone
  driving the board by hand while `tools/dev.ts` (or another future client)
  drives it programmatically still wants to see `prof`/`stroke`/`BOOT` lines
  live, per this file's own design going back to `devlink_poll()`'s "never
  blocks" contract. Suppressing or marking every line changes what a human
  watching the console sees, for the benefit of a problem the host side can
  solve without touching the wire format at all.
- Practically: a firmware change made in this session cannot be verified
  against the running board (it is explicitly not to be reflashed while
  another change is in flight elsewhere in the tree), where the host-side
  fix was verified directly, repeatedly, against the real board on `COM4`.

The host-side fix (`expectLine()` plus `isBase64Line()`) fully covers both
failure shapes seen in practice (noise where a reply should start, and by
construction, noise that would land inside a multi-line body) without
changing the wire format `tools/dev.ts` already documents below, and without
giving up the shared console's usefulness to a human.

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
| `APP` | `APP <index> <name>` |
| `SWITCH <index>` | `OK`, `ERR args` (index does not parse), or `ERR range` |
| anything else | `ERR unknown <cmd>` |

`<version>` is the devlink protocol version (currently `1`), independent of
firmware version. `<w>` and `<h>` are the framebuffer dimensions in pixels
(368x448 on this board).

Coordinates are panel pixel coordinates, `x` in `[0, w)`, `y` in `[0, h)`.
Whoever wires up the hooks in `runtime.c` is responsible for clamping (see
the integration notes below); devlink itself does not.

### APP and SWITCH

An agent that can only screenshot whichever app happens to be running can
only ever verify one screen. These two commands exist so it can also see
and change which app that is.

`APP` reports the running app: `<index>` is its position in the runtime's
`g_apps[]` table (what `SWITCH` takes), and `<name>` is the same short name
shown in the on-device menu and printed to the console on every switch (e.g.
`draw`, `chrono`, `timer`). If the menu overlay itself is open rather than
any app, `<index>` is a negative sentinel (the runtime's internal
`APP_INDEX_MENU`) and `<name>` is `menu`; devlink does not otherwise
interpret this value, it just reports whatever `app_current()` returns.

`SWITCH <index>` requests a switch to `g_apps[index]`, applied at the end of
the current frame, same as a real menu tap (see `app_switch_to()` in
`app.h`). `index` must be a valid `g_apps[]` position; `ERR range` covers
both an out-of-range number and the menu's own sentinel index, since the
menu is the shell a child uses to pick an app, not one of the apps devlink
is meant to drive to. There is no separate "open the menu" command.

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
is DMA'd out raw (see `runtime/gfx.h`'s `px_swap` for the full story: this only
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

`devlink_init()` takes a `devlink_hooks_t` (see `firmware/devlink.h`): the
framebuffer pointer + dimensions (read-only, for `SHOT`), four function
pointers that turn `DOWN`/`MOVE`/`UP`/`ERASE` into actual input, and three
more for `APP`/`SWITCH`. All seven are owned by `firmware/runtime/runtime.c`
(see its "devlink wiring" section), the only file that wires devlink to the
rest of the runtime; `devlink.c` itself does not know what an app or a touch
queue is. `devlink_init()` is called once, right after `gfx_init()`
succeeds; `devlink_poll()` is called once per main-loop iteration.

**Touch injection is a synthetic producer on the real touch queue, not a
separate drawing path.** Before the single-binary runtime, `DOWN`/`MOVE`/`UP`
called straight into the sketchpad's own `stroke_begin`/`stroke_sample`/
`stroke_end`, so only the sketchpad ever saw an injected touch. Touch now
arrives everywhere through `sensors_touch_next()`, fed by a queue core1 owns
exclusively (see `runtime/sensors.h`'s ownership rule), so the hooks instead
call `sensors_inject_touch()`, which every app reads exactly like a real
finger. Making core0 push straight into that queue would make it a second
producer of a ring whose only safety argument is "core1 is the sole
writer", so injected samples go into their own core0-owned ring instead,
merged into the real stream by timestamp inside `sensors_touch_next()` (see
`sensors.c` for the full argument). Practically, this means devlink now
drives every app, not just the sketchpad, and driving a real finger and
devlink at the same time no longer corrupts anything, though the two
streams interleaving is still confusing to watch and there is no reason to
do it.

**`ERASE` triggers a synthetic shake, not a direct wipe.** The old hook
called the sketchpad's `wipe_erase()` directly; there is no cross-app
equivalent of "erase" now, and inventing a global erase verb would
contradict `sensors.h`'s own reasoning for why shake is opt-in per app (see
that file). `ERASE` now bumps the same counter a real shake does
(`sensors_inject_erase()` / `sensors_erase_seq()`), so it only reaches an
app that opted in with `wantsShake` (today: the sketchpad). On any other
app it is a no-op, same as physically shaking the device would be.

## Host CLI (`tools/dev.ts`)

```
bun tools/dev.ts ping
bun tools/dev.ts shot out.png
bun tools/dev.ts app
bun tools/dev.ts switch 0
bun tools/dev.ts tap 184 224
bun tools/dev.ts draw 20,20 60,40 100,30 140,60
bun tools/dev.ts erase
bun tools/dev.ts log 15
```

`app` and `switch` exist in the wire protocol (see "APP and SWITCH" above)
but were not wired up as CLI subcommands until this task; they are now.

Port and baud are overridable: `DEVLINK_PORT` (default `COM4`),
`DEVLINK_BAUD` (default `115200`).

### Bring-up notes (first real run against hardware)

All seven subcommands (`ping`, `shot`, `app`, `switch`, `tap`, `erase`,
`draw`) were run against the board on `COM4` running the stopwatch app,
repeatedly, including several `shot` captures back to back and a chain of
`app` / `switch 0` / `app` / `tap` / `erase` run one after another. All of
it works.

**`PING` looked broken and was not.** The bug report that started this
bring-up said a `PING` sent right after opening the port got no visible
reply within 3 seconds, then `APP` worked right after. Investigated by
running `ping` repeatedly, fresh port open each time (exactly the scenario
described): with the fixed `expectLine()`-based reader, it came back
`OK devlink 1 368 448` on every attempt. The most likely explanation is the
same bug class as the `SHOT` failure that opened this investigation: the old
reader took the first line back as the reply, and if a `prof ...` tick (or
any other debug line) happened to arrive in that window instead, the old
code either printed that line (which does not start with `OK`, so it would
have looked like failure) or, if nothing at all had arrived within the
hard-coded 3000ms `readLineWithTimeout`, threw an unhandled promise
rejection, which is consistent with "no visible reply" if the failure
scrolled past or the process exited before the error was noticed. `PING`
itself is not broken; nothing about it needs a longer settle time or a
retry beyond what `expectLine()` already does for every command.

**The `SHOT` screenshot looks sideways, and that is correct.** The board
reports the framebuffer as 368 wide by 448 tall (`w`, `h` in the `SHOT`
header, matching `PING`'s report), which is a portrait-shaped buffer, while
the stopwatch app draws its landscape UI into that same native buffer. A raw
`SHOT` dump is not rotated to match how the physically-mounted panel is
viewed, so the decoded PNG shows the same content a human sees, just turned
90 degrees. Six 0-height rectangle outlines in two groups of two, separated
by two small square pairs, is what "00:00:00" looks like turned on its
side, which is what the captured screenshot shows.

**A `COM4` re-enumeration during this session was a reflash, not a bug.**
Partway through bring-up, `ping` and `shot` started failing with "the port
does not exist" and then "access to the port is denied". This was a real
board reflash landing mid-session from unrelated work in flight elsewhere in
the tree, not a `tools/dev.ts` defect: the port came back and every command
worked again once the new firmware had finished booting. Anyone hitting
this: retry after a few seconds before assuming the tool or the board is
broken.

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
RLE/base64 decoding, PNG encoding) was originally verified against a fake
stand-in process, and the three bullet points above (real `SerialPort`
behaviour) have since been verified against the real board too (see
"Bring-up notes" above).

One thing this bring-up found that was not about `SerialPort` behaviour at
all: `Bridge.close()` called `proc.kill()` and returned without waiting for
the child to actually exit. `COM4` is exclusive, so two `bun tools/dev.ts`
invocations run back to back could race for it: the first one's PowerShell
child could still be in the process of dying (and still holding the port
handle) when the second one tried to open it. This is the likely explanation
for one bring-up failure (`switch` timing out mid-chain, right after `app`,
in `bun tools/dev.ts app && bun tools/dev.ts switch 0 && ...`; retried alone
it worked immediately), though it was not cleanly isolated from other noise
in the same session (see the next paragraph). `close()` now awaits
`proc.exited` (bounded to 2s, in case the child is ever wedged) before
returning, and every call site awaits `close()` in its `finally` block; the
same chained sequence was reliable across several repeats after this fix.
This is a real, fixed bug regardless of whether it was the exact cause of
that one timeout: the old `close()` provided no guarantee the port was free
when it returned, which is worth fixing on its own given `COM4`'s exclusive
access.

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

- All six subcommands (`ping`, `shot`, `app`, `switch`, `tap`, `erase`,
  `draw`) have been run against real hardware (see "Bring-up notes" above).
  `draw` was only checked for a clean `DOWN`/`MOVE`/`MOVE`/`UP` round trip
  against the running stopwatch app, which does not react to touch visually;
  it was not checked against the sketchpad, so a real stroke's visual result
  (as opposed to the reply sequence) is still unverified.
- `draw` paces `MOVE` commands 20ms apart; real touch reports arrive at
  roughly 58Hz (~17ms) per the runtime's profiling notes, so this is in the
  right ballpark but not tuned against a real stroke.
- Injected samples are merged into the real touch stream by timestamp, with
  ties broken toward the injected one; see `sensors.c`'s injection-ring
  comment. Driving `devlink` and a real finger at once no longer corrupts
  anything, but the interleaved result is still confusing to watch and there
  is no reason to do it.
- `log` has no way to distinguish "device idle" from "bridge died"; both
  show up as a read timeout. If output stops unexpectedly, check that the
  PowerShell child is still alive (Task Manager, or rerun with
  `DEVLINK_PORT` pointed at a definitely-wrong port name and confirm you
  get an immediate "failed to open" instead of a silent hang, as a smoke
  test of the error path).
