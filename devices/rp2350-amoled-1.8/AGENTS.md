# rp2350-amoled-1.8

Firmware for the **Waveshare RP2350-Touch-AMOLED-1.8**, a 368x448 AMOLED in a
small plastic puck.

**One binary, five apps, a menu.** This is a single-binary runtime
(`firmware/runtime/`) holding an app table (`firmware/apps/`): a stopwatch
(`chrono.c`, index 0, what boots), a sketchpad (`sketch.c`, "draw"), a
countdown timer (`timer.c`), Connect Four (`four.c`) and a bubble level
(`level.c`). Switching apps is a function call, not a reboot: holding BOOT
and PWR together until PWR's long-press verdict fires opens a picture menu
(`menu.c`) to pick another app; the same chord closes it again. See
`docs/decisions/0002-runtime-architecture.md` for why this replaced an
earlier two-flash-slot, reboot-to-switch design (`store/`, now a crash-recovery
fallback rather than how apps change).

The sketchpad is the oldest of the three and the most hardware-proven: draw on
the touchscreen with a finger, shake the puck to erase. Strokes are
anti-aliased with variable width, shaped by a tldraw-style pressure model, so
they read as ink rather than as pixels. See "The sketchpad" below.

Earlier apps than that, still reachable in git history and in `tools/`,
replayed handwriting: first synthesised from a font (which always looked
typeset), then a real capture of a hand drawing in a local tldraw. The capture
rig is still useful and still works, see "Capturing real handwriting" below.

## The board (verify before assuming)

This is the **RP2350** variant, not the ESP32-S3 one. Waveshare sells both under
almost the same product name and the same case, so it is easy to buy the wrong
docs. Ours reports `RP2350 revision A2, QFN60, 16MB flash` to `picotool info`.

| Part | Chip |
|---|---|
| MCU | RP2350A, dual Cortex-M33 + dual Hazard3 RISC-V, 150 MHz, 520KB SRAM |
| Display | 1.8" AMOLED 368x448, SH8601 over QSPI |
| Touch | FT3168 over I2C |
| IMU | QMI8658 |
| PMIC | AXP2101 |
| RTC | PCF85063 |
| Audio | ES8311 codec, speaker + mic |

Pins, taken from the vendor demo and confirmed working:

| Signal | GPIO |
|---|---|
| QSPI CS / SCLK | 9 / 10 |
| QSPI DIO0..3 | 11, 12, 13, 14 |
| Panel RST / PWR_EN | 15 / 17 |
| Panel TE (tearing effect) | 16 |
| I2C SDA / SCL | 6 / 7 |
| Touch RST / INT | 5 / 4 |
| IMU INT1 | 8 |
| AXP2101 interrupt | 2 |
| PWROK from PMIC (`SYS_OUT`) | 18 |
| Audio I2S DOUT / DIN / MCLK / LRCK / BCLK | 20 / 21 / 22 / 23 / 24 |
| Speaker amp enable | 19 |
| microSD CS / MOSI / SCK / MISO | 25 / 26 / 27 / 28 |

Taken from the published schematic
(`files.waveshare.com/wiki/RP2350-Touch-AMOLED-1.8/RP2350-Touch-AMOLED-1.8.pdf`),
not inferred from the demo code.

**LRCK (GPIO23) was missing from this table until the audio bring-up
(2026-08-13).** I2S cannot function without a word-select line, so it has to
exist somewhere; it does, contiguous with the other four (20-24), and was
found by tracing the schematic's "Codec" block net-label-to-net-label
(`firmware/runtime/sound.c`'s header comment has the exact coordinates) after
the demo/vendor sources gave no clean answer for this specific board. If a
pin table is ever hand-copied from a schematic again, check every signal a
chip's own datasheet says it needs is actually accounted for, not just the
ones a first pass happened to label.

## The buttons, which the vendor header describes wrongly

**`SYS_OUT` (GPIO18) is not a button.** The schematic shows it carrying `PWROK`
from the PMIC through a BSS138 level shifter: it is a power-good indicator. The
vendor demo's `DEV_IRQ_SET(SYS_OUT, GPIO_IRQ_LEVEL_HIGH, ...)` handler calling
`watchdog_reboot` is a power-loss path, not a key handler. Pressing either
button moves this pin not at all, which was verified across several sessions of
deliberate pressing before the schematic confirmed why.

The two side buttons, **screen facing you, buttons on the right**:

| Button | Position | Wiring | Readable at runtime |
|---|---|---|---|
| PWR | **lower** | to the AXP2101 `PWRON` pin | yes, via the PMIC |
| BOOT | upper | bootloader select | no |

So the power key belongs to the PMIC, and firmware only ever sees it second
hand: a press pulls **GPIO2** (`AXP_IRQ`) low and latches a bit in AXP2101
register `0x49`. Bits, from the datasheet and confirmed on hardware: `0x01`
release, `0x02` press, `0x04` long press, `0x08` short press. Enable them in
register `0x41` first; the two edge interrupts are disabled by default.

**Long-press gestures are safe.** Register `0x27` holds three fields (AXP2101
datasheet, X-Powers Rev 0.1, 2019-04-28): `IRQLEVEL` (bits 5:4, the long-press
interrupt threshold), `OFFLEVEL` (bits 3:2, the hard power-off hold), and
`ONLEVEL` (bits 1:0, how long POK must be held to power the board on from
off, unrelated to the runtime gesture). `IRQLEVEL` stays at its 1.5s default,
measured on this board at 1480ms after the press; the owner has decided the
menu gesture's feel is settled and it is not touched.

`OFFLEVEL` shipped at its 6s default, which a 4.5s hold did not cut power at,
so a 1.5s gesture only had 4.5s of margin before the rails dropped with no
warning. Since the menu gesture (both buttons held until the long-press
verdict) became this device's primary navigation, and the user is a small
child who holds buttons too long, `sensors_init()` now raises `OFFLEVEL` to
its maximum, `11b` = 10s, on every boot (`pmic_raise_poweroff_threshold()` in
`firmware/runtime/sensors.c`), read back and printed once at startup to
confirm the write took. That gives the 1.5s gesture 8.5s of margin instead of
4.5s.

A full framebuffer is 368*448*2 = 330KB and **fits** in the 520KB SRAM, so the
firmware keeps one and pushes dirty rectangles. The ESP32-S3 sibling cannot do
this and has to render in bands; do not copy that design here.

## A finger is about 100 pixels wide, and that decides most layouts

368x448 over a 1.8 inch diagonal is a diagonal of about 580 pixels, so roughly
**322 pixels per inch, or 12.7 pixels per millimetre**.

| | across | on this panel |
|---|---|---|
| adult fingertip contact | ~8 mm | **~100 px** |
| child fingertip contact | ~6 mm | **~75 px** |

A finger therefore covers **more than a quarter of the panel's width**. This
device is a toy for a young child, so the child figure is the one that governs.

Consequences worth having in mind before laying anything out:

- The panel fits about **4 finger-widths across and 6 down**. That is the real
  resolution of anything that has to be touched, not 368x448.
- **A row of targets across the 448px width runs out at five, and this is the
  worked example.** Three tiles across gives 149px, about two child fingers;
  four gives 112px; six gives 74px, which is under one finger; twelve gives
  37px. A layout that only uses the horizontal axis spends the whole 368px of
  height on a target that needs 75 of it, so its capacity is fixed by the
  short axis alone. The menu was exactly this and was rewritten as a grid on
  2026-08-15 for exactly this reason - see
  `docs/decisions/0013-the-menu-is-a-grid-and-nothing-is-hidden.md`. The
  general lesson, before laying anything out: count the targets the PANEL
  holds (about twenty at 75px), not the ones one row holds.
- Anything a finger must land on precisely (a ring to drag, a small control)
  is being asked for a precision the hardware cannot give. Prefer targets that
  are forgiving in one dimension: an angle around a large ring is forgiving,
  a 20px handle is not.
- Ink is the exception, not the rule: the sketchpad draws a 5px pen from a
  contact patch 15 times wider, because the controller reports a centroid. It
  reports one for a child's finger too, so drawing works; tapping a small
  target is what does not.

Verify the diagonal against the product page before treating the exact figures
as gospel; the ratio is what matters and it is not close to the edge.

## Layout

```
firmware/runtime/    the runtime: runtime.c (board entry point, startup,
                      watchdog, devlink wiring, profiler), runtime_core.c
                      (portable: arena, app table, switching, frame dispatch -
                      compiles for both the board and wasm32-freestanding, see
                      docs/decisions/0003), gfx.c (framebuffer + panel push,
                      the one place the 8-pixel row rule lives), sensors.c
                      (core1 owns i2c1: touch, IMU, PMIC), sound.c (the ES8311
                      codec + I2S over PIO/DMA, brought up on core0 before
                      core1 launches - see "Sound" below), sound_synth.c (the
                      chime itself, pure math, compiles into both main.uf2 and
                      emu.wasm unmodified)
firmware/apps/        one file per app plus shared helpers: chrono.c
                      (stopwatch), sketch.c (drawing), timer.c (countdown),
                      four.c (Connect Four for two people passing the puck,
                      slide a thumb and release to drop; nothing plays by
                      itself), level.c (a bubble level: tip the puck, a dot
                      slides downhill, hold it flat and a ring closes round
                      it - reads app_frame_t.tilt like any other app, see
                      "Which way is down" below), menu.c (the app picker: a
                      grid of 112px cells filling the glass, all apps visible
                      at once, press-drag-release to launch - decision 0013),
                      stubapps.c (empty unless the menu-stub define is
                      set; how that layout gets captured at six and twelve
                      apps, see its own header), digits.c
                      (shared seven-segment numerals), shapes.c (round
                      silhouettes built from rectangles, used by menu.c's
                      icons)
firmware/lib/         Waveshare drivers, copied from the vendor demo (with our
                      patches - see "Gotchas that bite" below)
firmware/bootbtn.c    reads the BOOT button by borrowing the flash chip select
firmware/devlink.c    the USB screenshot/touch-injection/app-switch link an
                      agent uses to drive the board without a human - see
                      tools/README-devlink.md
firmware/CMakeLists.txt   what actually builds; also documents what does NOT
                      any more (appswitch.c, bootreq.h) and why
vendor-baseline/      the untouched Waveshare demo download, for reference
docs/decisions/       why things are the way they are (AGENTS says how, this
                      says why): 0001 (the 8-pixel push rule), 0002 (the
                      single-binary runtime), 0003 (the emulator runs the real
                      apps)
emulator/             runs the firmware's own C, compiled to WebAssembly, in a
                      browser - see "The emulator" below
emulator/wasm/tests/  regression tests that run against the real compiled
                      firmware - see "Regression tests" below
tools/gate/           ONE command, every cross-cutting rule, every app, ~3s:
                      push geometry and per-tick cost, pixels changing
                      outside pushed rectangles, ink under the bezel,
                      animations that need touch samples or leave residue,
                      a stale emu.wasm, a board running a different build.
                      Run it before you believe a change - see "The gate"
                      below and tools/gate/README.md
store/                the retired two-flash-slot app switcher; its partition
                      machinery is kept as a golden-image crash-recovery
                      fallback, not how apps change now (see store/README.md
                      and docs/decisions/0002 section 6) - stale in places,
                      read it critically
tools/dev.ts          drives the board over USB: screenshot, tap, drag,
                      buttons, the menu chord, app switching - see "Driving
                      the board headlessly" below
tools/cam.py          photographs the physical panel with a USB webcam - see
                      "Photographing the panel" below
tools/gen-strokes.ts  generates strokes.h + a PNG preview, for the retired
                      handwriting-replay app (see "Capturing real handwriting")
tools/fonts/          Hershey single-stroke fonts (public domain), used by
                      gen-strokes.ts
preview/              host-rendered PNGs, for judging the handwriting look
                      without flashing
backup/                factory firmware pulled off the board before first flash
```

## Running it

Build (nothing is on PATH by default, so the env has to be set):

```powershell
$env:PICO_SDK_PATH = "$env:USERPROFILE\pico\pico-sdk"
$env:PICO_TOOLCHAIN_PATH = "C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1"
$env:PATH = "C:\Users\sylve\.espressif\tools\cmake\3.30.2\bin;C:\Users\sylve\.espressif\tools\ninja\1.12.1;$env:PICO_TOOLCHAIN_PATH\bin;" + $env:PATH

cmake -S firmware -B firmware/build -G Ninja `
  -Dpicotool_DIR="$env:USERPROFILE\pico\tools\picotool-dist\picotool" `
  -Dpioasm_DIR="$env:USERPROFILE\pico\tools\sdk-tools\pioasm"
cmake --build firmware/build
```

Flash (no buttons needed, picotool reboots the board itself):

```powershell
& "$env:USERPROFILE\pico\tools\picotool-dist\picotool\picotool.exe" load firmware/build/main.uf2 -f -x
```

Change the message or the look, then rebuild:

```powershell
bun tools/gen-strokes.ts --stroke 4.4   # writes preview/ and firmware/strokes.h
```

Useful flags: `--font` (any `.jhf` in `tools/fonts`), `--size` (glyph height),
`--stroke` (tldraw stroke width), `--gap`, `--tracking`, `--space`, `--jitter`,
`--rough`, `--seed`. Look at `preview/` before flashing.

## Two build targets: the board, and WebAssembly for the emulator

The board build above (`cmake -S firmware -B firmware/build`) is one target.
The second compiles most of the same C to `wasm32-freestanding` for the
emulator, per `docs/decisions/0003-emulator-runs-the-real-apps.md`:
`firmware/runtime/runtime_core.c`, `gfx.c`, and every file under
`firmware/apps/` (not `runtime.c`, `sensors.c`, `bootbtn.c` or `devlink.c` -
those are the board's own entry point and hardware, not the portable core).

```powershell
bun run emulator/wasm/build.ts
```

Requires `zig` (a self-contained cross-compiler with no sysroot to install;
`emulator/wasm/build.ts`'s own header comment explains why it was chosen over
emscripten or wasi-sdk). Defaults to `C:\Users\sylve\tools\zig\zig.exe`;
override with `ZIG_EXE`. Writes `emulator/wasm/dist/emu.wasm`. This is not the
same artifact as `firmware/build/main.uf2` and does not get flashed anywhere;
it is loaded by a browser page instead (see "The emulator" below).

## The emulator

`emulator/` compiles the firmware's own object code to WebAssembly and runs it
in a browser, rather than reimplementing the apps in TypeScript (that was
tried first and rotted the moment the firmware grew a runtime; see
`docs/decisions/0003-emulator-runs-the-real-apps.md` for the full argument).
Same C, same rasteriser, same arena and app-switch logic as the board; only
the framebuffer surface, the touch/button input and the clock are supplied by
the page instead of by silicon.

```powershell
cd emulator
bun install
bun run wasm/build.ts   # or: from the device root, bun run emulator/wasm/build.ts
bun run server.ts       # http://127.0.0.1:5330
```

There is no console once this is backgrounded: quit from the page's **quit**
button (`POST /api/quit`, guarded by a custom header a cross-origin page
cannot set, closing the localhost-CSRF hole - see `emulator/server.ts`). The
dev server watches `emulator/wasm/dist/` and live-reloads connected pages once
a rebuilt module is stable on disk, so editing an app and re-running the wasm
build shows up without a manual browser refresh.

**What it is genuinely good for:** iterating on an app's layout, logic and
redraw decisions without a flash cycle each time; reproducing a logic bug (an
app that stops responding, a wrong arena reset) deterministically, one tick at
a time; seeing the actual pushed windows as an overlay, which is what makes a
decision-0001-shaped bug (a partial refresh corrupting only some window
shapes) visible on screen instead of invisible until someone photographs the
panel.

**What it can never answer, per decision 0003, and this is not a minor
caveat:** timing. The browser's clock drives the tick loop; nothing here
reproduces the measured 695us I2C touch read, the ~12ms full-panel push, or
core1 existing at all. **Any question of the shape "is this responsive / does
this feel laggy / is the touch report rate good enough" is a question for the
board, always**, never for the emulator, no matter how convincing the
emulator's own timing looks. The emulator can also never exercise the real
FT3168's actual dropouts and strays (the stand-in touch controller is a clean
mouse drag, not real hardware defects), and there is no panel here to burn in,
dim, or tear.

## Driving the board headlessly: `tools/dev.ts`

`firmware/devlink.c` is a small command interpreter riding the same USB CDC
serial port the runtime already prints debug output to; `tools/dev.ts` is the
host CLI that talks to it, so an agent can see the screen and drive the
touchscreen and buttons without a human. Full wire protocol and design
rationale in `tools/README-devlink.md`.

```powershell
bun tools/dev.ts ping                          # is the device alive
bun tools/dev.ts shot out.png                  # screenshot (framebuffer, not the panel - see cam.py below)
bun tools/dev.ts app                           # which app is running
bun tools/dev.ts switch 0                      # switch to g_apps[0]
bun tools/dev.ts tap 184 224
bun tools/dev.ts draw 20,20 60,40 100,30 140,60
bun tools/dev.ts erase                         # synthetic shake, reaches only apps that opted in (the sketchpad)
bun tools/dev.ts key short                     # inject a PMIC key event (press/long/short)
bun tools/dev.ts boot click                    # inject the BOOT button
bun tools/dev.ts chord                         # the BOOT+PWR app-menu gesture
bun tools/dev.ts log 15                        # stream device output for 15s
```

Port/baud default to `COM4` / `115200`; override with `DEVLINK_PORT` /
`DEVLINK_BAUD`. `chord` is the one command worth calling out: it drives the
exact condition `runtime_core.c`'s menu gesture checks (BOOT held, PWR's
long-press verdict delivered), so it opens the menu with no human touching a
button, and sent again while the menu is open it closes back to whatever was
running before.

**The honest gap: injected buttons cannot test the PMIC decode path.**
`KEY`/`BOOT`/`CHORD` hand the runtime the bits a real press would have
produced, as if the AXP2101 register read and write-1-to-clear had already
happened and come out clean. They prove the runtime and the app underneath a
gesture handle that bit correctly. They prove nothing about whether the
AXP2101 will ever actually latch and deliver that bit on real silicon - the
one real bug this project has already shipped in this exact area (several PMIC
bits landing in one read and silently breaking the old menu gesture) lived
precisely in the register read and the read-and-clear timing that injection
skips. Only a human physically pressing the button checks that half. See
`tools/README-devlink.md`'s "What injection cannot test" for the full
argument, and note this is the mirror image of the emulator's own rule above:
that rule stops a *simulated* board from lying about silicon; this is a gap in
a test tool driving *real* firmware, with no simulated chip to blame.

## Photographing the panel: `tools/cam.py`

`tools/dev.ts shot` shows what the **framebuffer** holds. `tools/cam.py`
photographs what the **panel** actually displays with a named USB webcam
(matched by name, not by index, because the camera index is not stable on
this machine - see the file's own header comment for the day it silently
photographed the operator's face instead of the device).

```powershell
python tools/cam.py --list                     # find the camera's name first
python tools/cam.py out.png --autocrop          # crops to the self-lit panel automatically
```

Needs `opencv-python` and `pygrabber`/`comtypes` (`pip install opencv-python
pygrabber comtypes`); see the file's header for why this is the one tool in
the repo written in Python rather than TypeScript (no `ffmpeg` on this
machine's PATH, no usable native Node/Bun DirectShow binding on Windows-ARM64).

**Why this matters, and it has already saved a wrong investigation once:**
comparing a `dev.ts shot` framebuffer dump against a `cam.py` photograph of the
same moment separates a drawing bug from a display bug immediately. If the
dump is correct but the photo is wrong, the bug is downstream of the
framebuffer (a push, a window, the panel itself - exactly the shape of the
decision-0001 defect). If both are wrong the same way, the bug is upstream, in
the app's own drawing code. Without both, a display-path bug reads exactly
like a drawing bug, and this project has already spent real debugging time on
that confusion once (see `docs/decisions/0001-push-min-width.md`'s "What this
cost, and the lesson").

## Regression tests

`emulator/wasm/tests/` runs assertions against the real compiled firmware
(`emulator/wasm/dist/emu.wasm`), driven through `emu_tick()` with a synthetic
clock, exactly the way decision 0003 argues a reimplementation never could.
Build the wasm module first, then run a test directly with `bun`:

```powershell
bun run emulator/wasm/build.ts
bun run emulator/wasm/tests/repro-arena-not-zeroed.ts
bun run emulator/wasm/tests/repro-ring-shrink-residue.ts
bun run emulator/wasm/tests/repro-switch-input.ts
```

Each `repro-*` file is a reproduction of a real bug found on hardware or in
the emulator, turned into a standing regression check rather than a one-off
script; each `feature-*` file is a statement of what a feature is supposed to
do, asserted the same way. Read the header comment at the top of each file for
what it covers and why the assertions are shaped the way they are. They
compare framebuffer hashes, sampled pixels and `emu_app_current()`, never
internal pointers, so a failure is something a person at the device could also
have observed.

**A feature driven by touch needs BOTH kinds of file.** `feature-*` drives
clean input, which is what makes it a readable statement of intent; the
`repro-touch-dropout-*` files drive the same gesture through `TouchSim` at the
measured hardware dropout rate (34 episodes/sec), which is what catches the
class of bug that has already shipped once here - the sketchpad's palette
passed all 22 of its clean-input checks and did not work in the owner's hands.
Connect Four's own pair is `feature-four.ts` and
`repro-touch-dropout-four-drop.ts`.

The build retries `zig cc`, because this toolchain's linker crashes at random
(no diagnostics, exit code 5, links fine moments later - see
`emulator/wasm/build.ts`). A failure that survives all the attempts is real. A
one-shot build that is not checked will leave the PREVIOUS `emu.wasm` in
place, and the tests will then pass or fail about code nobody is looking at.

It also links to a temp file and **renames onto `dist/emu.wasm` only on
success**, which matters because more than one agent works in this repo at
once. A half-written module still *compiles* (the code section is intact and
the data section is short), so what you get is not a load error but this, on
the first tick after an app switch:

```
RuntimeError: call_indirect to a signature that does not match
```

with the firmware's own switch line printing an EMPTY app name just before
it. That is a zeroed `app_t` in `g_apps[]`, i.e. a truncated file - not a bug
in whatever app you happen to be working on, which is exactly what it looks
like. If you see it, rebuild and re-run before believing anything. It cost an
afternoon twice before the rename went in.

## The gate

```powershell
bun run emulator/wasm/build.ts
bun run tools/gate/run.ts
```

Three seconds, every app, no per-app opt-in. The tests above pin what one
app is supposed to do; this pins what is true of ALL of them, and each rule
is a bug that has already reached the owner's hands once:

- every pushed window's row length is a multiple of 8, and inside the panel
- work per tick is bounded, in pixels pushed and transfers issued (the
  palette's watchdog reset was unbounded render cost per drained sample)
- **no pixel changes outside that tick's pushed rectangles** - the class of
  bug that looks right in the emulator and is stale on the panel, and which
  has shipped four times
- no ink inside `PANEL_BEZEL_MARGIN_PX` on a settled frame
- an animation advances on the clock, not on the arrival of touch samples,
  and settles to the picture the settled state alone would draw
- a gesture test drives the measured controller profile, or says why not
- **it refuses to run at all against an `emu.wasm` older than its sources**,
  which is the mechanical version of the warning three sections up

It exits non-zero on anything new, prints known-and-unfixed findings loudly
without failing, and prints what it cannot check at the end of every clean
run. Read `tools/gate/README.md` before adding a rule, and
`tools/gate/known.ts` before believing the repo is clean.

`tools/gate/fingerprint.ts` is the other half: it hashes every firmware
source, `firmware/CMakeLists.txt` stamps that into the image, and devlink's
`FP` command reads it back. Nothing that compares the board against this
tree should be believed until `bun run tools/gate/fingerprint.ts --device`
agrees - a differential harness once diffed three apps on the board against
four here and reported a 28% rendering divergence that was a stale flash.

## Gotchas that bite

- **`zig.exe` hangs, and it looks exactly like a broken toolchain.** Builds
  start failing with `zig cc exited 5` and no diagnostics, then stop
  producing output at all. The cause is HUNG zig processes holding the shared
  global cache lock: eight of them were found at once on 2026-08-14, one 21
  hours old, all with a few hundredths of a second of CPU (a real build burns
  CPU; these were deadlocked). The run queue was at 41. Three agents lost
  time to it the same afternoon.

  Diagnose and clear it by AGE AND CPU, never by name - `taskkill /IM` on a
  shared machine is how someone's browser dies:

  ```powershell
  Get-Process zig | Select-Object Id, StartTime, CPU
  Get-Process zig | Where-Object { $_.CPU -lt 1 -and $_.StartTime -lt (Get-Date).AddMinutes(-2) } |
    ForEach-Object { Stop-Process -Id $_.Id -Force }
  ```

  Builds go back to succeeding first try. `emulator/wasm/build.ts` retries the
  crash and links through a temp file, which covers the intermittent case; it
  cannot do anything about a lock held by a process that never exits.


- **The QMI8658's low-pass filter is OFF, even though the driver looks like
  it enables it.** `QMI8658_config_acc()` (`firmware/lib/QMI8658/QMI8658.c`,
  around line 156) is called with `QMI8658Lpf_Enable`, builds the correct
  `CTRL5` value from `A_LSP_MODE_3 | 0x01`, and then does `ctl_dada = 0x00;`
  on line 196, the line before the write, so `CTRL5` is written 0.
  `QMI8658_config_gyro()` has the identical shape (`ctl_dada = 0x00;` on line
  258, right before its own `CTRL5` write). Found 2026-08-15 while writing
  the bubble level; deliberately NOT fixed, because it is vendor code, the
  shake detector's `JOLT_DEV_MG` was tuned against the signal as it actually
  is, and changing it cannot be tested without a board. Consequence for
  anything that reads the accelerometer: the part runs at a 1000 Hz ODR with
  no filter and core1 samples it at 50 Hz, so the full 500 Hz noise
  bandwidth folds into every sample. That is still only about 2.7 mg RMS
  (0.15 degrees of apparent tilt) - a hand's own tremor is ten times that, so
  the filtering that matters is in software either way (see "Which way is
  down" below for what that software filter now is). Also worth knowing: the
  gyroscope is configured and enabled by `QMI8658_init()` right alongside the
  accelerometer, and this firmware never reads it - only
  `QMI8658Register_Ax_L`'s six accelerometer bytes are ever pulled off the
  part.
- **We carry a patch to `AMOLED_1IN8_DisplayWindows`.** Upstream's DMA loop is
  `for (i = Ystart; i < Yend - 1; i++)`, which sends one row fewer than the
  window `SetWindows` just declared to the panel (it programs `Yend-1` as the
  inclusive last row). The bottom row of every partial refresh silently never
  updated. The same file's `Display()`/`Clear()` use the correct bound, which
  is what makes it a bug rather than a convention. `lib/AMOLED/AMOLED_1in8.c`
  is fixed here to `i < Yend`. **If you ever re-copy the driver from the
  Waveshare zip, this patch is lost** and partial updates will start dropping
  their last row again.
- **Every pushed window's row length must be a multiple of 8 pixels (16
  bytes), or `AMOLED_1IN8_DisplayWindows` corrupts the transfer.** This lives
  in one place now, `gfx_push()` in `firmware/runtime/gfx.c`
  (`PUSH_GRAN_W`/`PUSH_MIN_W`, both 8): it rounds every window's row length up
  to a multiple of 8 and, when that would run the window off the right edge,
  slides it left rather than clipping the width, because a clipped (shortened)
  row is exactly what corrupts. The window's *start* is aligned only to 2, not
  to 8 - the bisect that settled this (see below) showed an unaligned start
  with a rounded row length is clean, so aligning the start too would only
  widen the window for nothing. An earlier fix also forced a 64-pixel minimum
  width; that was superseded once the real rule (row length, not width, must
  be a multiple of 8) was isolated, and the minimum dropped to 8. If you ever
  see a 64-pixel-minimum version of this function again, it has regressed to
  the earlier, more expensive fix.

  Why this exists at all: `AMOLED_1IN8_DisplayWindows` sends **one DMA
  transfer per row**, so a window's width is the size of each transfer. A
  vertical stroke produces a tall, narrow dirty rect, which becomes hundreds of
  transfers of a few dozen bytes, and those come out corrupted: the ink is
  displaced sideways in bands, so a vertical line reads as a ladder of
  horizontal ticks. The full bisect that isolated the row-length rule from the
  minimum-width workaround is in `docs/decisions/0001-push-min-width.md`; its
  `push_bisect_test()` tool (draws the same vertical stroke under four
  alignment/width policies side by side) was a bring-up aid and is not part of
  the current tree - if you need to re-bisect this, expect to rebuild
  something like it rather than finding it still wired to a button.
- **Judge display bugs by window shape, not by what the drawing code did.**
  This one was mistaken twice for a touch problem and two real but unrelated
  bugs were fixed chasing it. What broke it open was that the artifact tracked
  stroke *direction*: the horizontal bottom of a U was clean while both
  vertical sides were shredded, in the same stroke, from the same code. Touch
  sampling has no reason to care about direction. Window aspect ratio does.
- **`picotool partition create <json> <out.uf2> <bootloader.elf>` does not write a
  UF2.** Given a bootloader argument it writes a raw ELF, whatever you called
  the output file and regardless of `-t uf2`, and it does so silently. The
  resulting file starts with the ELF magic, so `picotool info` reads those
  bytes as a UF2 family ID, reports something absurd like `0x00000034`, and
  says "does not contain a valid RP2 executable image". The SDK's own CMake
  passes the same ELF as both output and bootloader, modifying it in place,
  which is the clue that this command was never meant to emit a UF2.
  The right way is to let the build embed the table: `pico_embed_pt_in_binary()`
  plus `pico_set_uf2_family()`, both **before** `pico_add_extra_outputs()`,
  which the SDK enforces with a hard error. Then a normal build produces a
  bootloader UF2 with the table inside it and installing is one `picotool load`.
- **Always confirm an artifact offline before flashing it.** `picotool info -a`
  on the file will show the image def and, for a bootloader, the embedded
  partition table with `hash: verified`. Flashing an unverified artifact cost
  three physical power-cycle recoveries in one evening, because a board that
  will not boot cannot be reflashed over USB (see the recovery note below).
- **Recovering a board that will not boot.** Replugging USB does NOT reset this
  board: the PMIC holds the rails up, so a hung app keeps running and holding
  BOOT at plug-in does nothing, because BOOT is only sampled at reset. Unplug,
  hold PWR for at least **12 seconds** until the screen goes black (the PMIC's
  power-off threshold was raised from 6s to 10s, its maximum, see "Long-press
  gestures are safe" above; 12s gives margin the same way the old "10 seconds"
  instruction did over the old 6s threshold), then hold BOOT while plugging
  the cable back in.
  Related: while an app is hung, `picotool` cannot reboot it into the
  bootloader either, since that request is serviced by the running app's USB
  interface. Loads appear to succeed and silently do nothing. Read flash back
  and check which build is actually there before concluding a fix did not work.
- **Boot delays are patched down from 775ms to 182ms, using the SH8601
  datasheet.** This matters because switching apps reboots the chip, so the
  init path IS the switch latency. Upstream waits 50ms for a reset pulse the
  datasheet specifies as 10 MICROseconds (tRW), and 300ms for a reset
  completion specified at 5ms (tSRT). It also waits two unexplained 100ms in
  `DEV_Module_Init`. **In the other direction it is UNDER spec**: the
  datasheet requires 150ms after Sleep Out (`11h`) and upstream waits 120ms,
  which we raised. Do not "optimise" that 150ms away; it is the one delay
  here the panel actually demands, and being short would show up as an
  unreliable wake rather than as an obvious failure.
- **Serial needs DTR.** Opening the CDC port without asserting DTR reads as a
  dead device. `$p.DtrEnable = $true` before `Open()`, or you will conclude the
  firmware crashed when it is running fine.
- **This machine is Windows on ARM64.** There is no host C compiler, so
  pico-sdk cannot build its own `pioasm` and `picotool`. Use the prebuilt x64
  ones from `raspberrypi/pico-sdk-tools` (they run fine under emulation) and
  point cmake at them with `-Dpicotool_DIR` / `-Dpioasm_DIR`. `cmake` and
  `ninja` are borrowed from the ESP-IDF install under `~/.espressif/tools`.
- **Do not let a venv Python be first on PATH** when running SDK installers;
  ESP-IDF's refuses outright, and pico tooling gets confused too.
- **AMOLED burn-in is real.** Anything that ends up always-on needs the image
  to move or the panel to sleep. None of the three shipped apps sit static:
  the stopwatch and timer redraw their digits/ring continuously while running,
  and the sketchpad wipes and redraws on erase. Power management (dim on idle,
  sleep on long idle) is still an open item - see
  `docs/decisions/0002-runtime-architecture.md` section 10 - so this has not
  been re-examined for the menu screen or an app left idle and unattended.
- **The factory firmware is in `backup/factory-firmware.uf2`.** Restore with
  `picotool load backup/factory-firmware.uf2 -f -x`.

## Touch, IMU and PMIC: all three live on core1

Touch and the IMU were originally both polled from a single main loop,
deliberately never on an interrupt: the vendor demo drives touch from a GPIO
IRQ and guards the shared `i2c1` bus with a plain `uint8_t i2c_lock` using
`while(lock); lock=1;`, which is non-atomic check-then-act on a non-`volatile`
flag, so an edge arriving in the gap lets the ISR start an I2C transaction on
top of an in-flight one. **Do not move any of this onto an interrupt** - that
reasoning still holds and is why nothing here does.

Polling has since moved off core0 entirely. `firmware/runtime/sensors.c`
launches core1 to own `i2c1` exclusively (touch, the IMU and the PMIC),
because the touch read alone measured at about 695us, roughly 98 percent of
frame time, on the old single-core loop; see
`docs/decisions/0002-runtime-architecture.md` section 3. **Once
`sensors_start()` has run, core0 must never touch `i2c1` again** - not the
touch controller, not the IMU, not the PMIC, not a debug read (see
`sensors.h`'s ownership-rule banner, which is enforced by convention, not the
compiler). Apps and the runtime read published signals
(`sensors_touch_next()`, `sensors_key_take()`, `sensors_erase_seq()`,
`sensors_boot_down()`/`sensors_boot_clicked()`) instead of touching chips
directly; touch samples cross from core1 to core0 through a lock-free
single-producer/single-consumer ring, never a lock, since a lock held across
an I2C transaction is exactly the vendor bug above.

The controller still does not tell you whether a finger is down from a stale
read: with the finger count at zero, the coordinate registers still hold the
last real touch. `sensors.c` reads the finger-count register separately from
the X/Y burst and treats the count as the authority for stroke start and end,
not the vendor driver's own `FT3168_Get_Point()` (which has this exact trap -
avoid it if you ever touch that code path directly). Coordinates are clamped
downstream, both in `runtime_core.c`'s touch resolution and again in
`sketch.c`'s own stroke code: they come straight from 12-bit touch registers
and the driver never validates them.

## Which way is down (`firmware/runtime/tilt.h`)

**Read this before writing any app that reacts to being tilted.** There is
one orientation signal, and it is `app_frame_t.tilt` (`app.h`): a filtered
gravity vector in g, in the app's own drawing space, plus the angle from
flat and which edge is up. An app reads it the way it reads `touchDown`,
never by touching the IMU - which is core1's chip like every other part on
`i2c1`, per the ownership rule above.

The five things worth knowing before you build on it:

- **Flat on a table, screen up, is `(0, 0, 1)`. Upright with the app's top
  edge up is `(0, 1, 0)`.** `+x` right, `+y` down the screen as you drew it,
  `+z` into the glass. A ball rolls toward `(gx, gy)`; a bubble floats away
  from it.
- **It is already in YOUR coordinates.** The runtime rotates it for
  landscape apps, exactly as `gfx_land_rect()` rotates their rectangles. Do
  not rotate it yourself.
- **It is filtered, once, for everyone** (an adaptive one-euro filter plus a
  magnitude trust gate - `tilt.c`'s own header comment has the full
  argument and every constant), so the device feels the same in every app.
  Do not add your own smoothing; if the feel is wrong, change the constants
  in `tilt.c` and say why. Ported here from the bubble level's own original
  filter (it was the first app that needed real orientation and measured
  the trade before there was a shared signal to hand it to); `level.c`
  carries none of it any more.
- **Check `valid`.** It is false before the first reading and if the IMU
  goes quiet, and a level drawn from an invalid reading is a confident lie.
  `coasting` is a separate, narrower flag: true while the trust gate has
  fully given up on the current sample (the device is being carried) and
  gravity is holding its last belief rather than tracking - still safe to
  draw, just not currently moving. No shipped app reads it yet.
- **There is no magnetometer on this board.** The QMI8658 is a six-axis
  part, so this can tell you which way is DOWN and can never tell you which
  way is NORTH. A compass cannot be built here; see
  `docs/decisions/0011-what-this-board-can-actually-do.md` for what to build
  instead, and `0012-one-orientation-signal.md` for this signal's design.

**The device-to-panel axis mapping is a hypothesis, not a measurement**
(nothing in this repo records how the part is rotated on the PCB, and no
software oracle knows which way is up). `bun tools/dev.ts tilt` runs the
five-pose ritual on real hardware; `tilt.h` has the poses and the expected
readings. If an orientation app leans the wrong way, that mapping is the
first suspect, and it is one function.

In the emulator: two sliders in the bottom bar (tip and roll), and
`emu_sensor_vector()` for tests, which can drive any orientation including
the ones the sliders cannot reach. `emulator/wasm/tests/feature-tilt.ts` is
the worked example.

## Sound (`firmware/runtime/sound.c`, `sound_synth.c`, `sound_i2s.pio`)

The ES8311 codec (0x18 on `i2c1`, CE pulled to AGND on the schematic) and its
I2S link are brought up ONCE, on core0, in `sound_init()` (`runtime.c`),
right after `sensors_init()` and before `sensors_start()` hands `i2c1` to
core1 - the same "everything i2c1 happens before core1 exists" shape
`sensors_init()` already uses for touch/IMU/PMIC. After that one call, the
sound service never touches `i2c1` again: the codec is left unmuted at a
fixed volume forever, and "play"/"stop" (`sound_play()`/`sound_stop()`,
`sound.h`) are purely a DATA-PLANE change (is the I2S stream carrying the
chime or silence), never a control-plane one. So there is no cross-core
signal to design for sound at all, unlike every real sensor - see
`sound.h`'s header comment for the full argument.

I2S is driven by PIO (the RP2350 has no dedicated I2S peripheral), on `pio1`
(claimed via `pio_claim_unused_sm()`, never `pio0`: the display's own QSPI PIO
program, `firmware/lib/QSPI_PIO/qspi_pio.c`, uses `pio0` state machines 0 and
1 directly without ever calling `pio_sm_claim()` for them, so the SDK's claim
bookkeeping for `pio0` cannot be trusted - see `sound.c`'s header comment).
No MCLK is generated: the ES8311 derives its internal clock from the bit
clock instead (a documented, driver-confirmed mode), so only LRCK, BCLK and
DOUT are actually driven. 32kHz / 16-bit stereo was chosen because it lands
exactly on a supported ES8311 clock-coefficient row AND gives an exact
(zero-remainder) PIO clock divider on this board's 150MHz system clock - see
`sound.c` for both derivations.

The chime is synthesised sample-by-sample (`sound_synth.c`), never stored:
a phrase of PCM at any usable quality would cost tens of KB this device's
SRAM budget (decision 0002's "Memory" table) does not have spare. The same
`sound_synth_alarm_sample()` function also compiles into the emulator
(`emulator/wasm/build.ts`), so what a browser plays through WebAudio
(`emu_abi.h`'s "sound" section) is genuinely this firmware's own synthesis,
not a JavaScript reimplementation - useful for judging the tune quickly, but
not the timbre: a laptop speaker will always flatter what this device's tiny
one actually does, so a real "does this sound good" verdict still needs the
board.

## The sketchpad (`firmware/apps/sketch.c`)

**No pressure signal.** Measured 2026-08-13 (see `firmware/runtime/sensors.h`):
this FT3168 reports 0 for both its per-touch weight (0x07) and area (0x08)
registers, always, finger held hard included. There is no press force or
contact area to read from this panel; `sketch.c` derives pressure from stroke
speed instead, and that is the only option here, not a placeholder.

**Anti-aliasing without a second buffer.** Everything on screen is neutral grey,
so the 6-bit green channel of the RGB565 framebuffer doubles as an 8-bit ink
level: read it back widened, rebuild R/G/B symmetrically on write. A separate
coverage buffer would cost another 165KB and would not fit alongside the 330KB
framebuffer in 520KB of SRAM.

Each stroke segment is drawn as a round-capped capsule with linearly varying
radius, from the distance to the segment: `coverage = clamp(r + 0.5 - d, 0, 1)`.

**Composition is MIN (darkest wins), and this is not a detail.** Consecutive
segments overlap heavily along their shared edge. Alpha blending would re-darken
that overlap on every segment, compounding until the anti-aliased edge turns
solid, which produces exactly the hard, pixelated line the AA was meant to
avoid. MIN unions the shapes instead.

Pen shape follows tldraw's draw tool: streamline the incoming points, derive a
simulated pressure from speed (fast is light), rate-limit pressure changes so
width does not flicker on noisy samples, then `radius = SIZE * easeOutSine(0.5 -
THINNING * (0.5 - pressure))`. Tapered at both ends so strokes start and finish
in a point. All the constants are `#define`s at the top of `firmware/apps/sketch.c`.

Shake detection requires several jolts inside a rolling window rather than one
big reading, because a single spike is indistinguishable from a firm tap. It is
suppressed while a finger is down, and has a cooldown so one shake cannot erase
twice. Erase is an animated wipe in 16 bands, not an instant blank.

## The bubble level (`firmware/apps/level.c`)

Tip the puck and a dot slides downhill; hold it flat and a grey target ring
turns black and closes around it. No text, no numbers, no degrees: the whole
verdict is one shape closing. It is in `g_apps[]` like every other app,
reading `app_frame_t.tilt` the same way any orientation-aware app does (see
"Which way is down" above) - it carries no accelerometer code of its own.

It was built and tested behind `APPS_INCLUDE_LEVEL` before the shared
orientation signal (`firmware/runtime/tilt.h`) and the grid menu (decision
0013) both landed; both blockers it was written against are gone now, so the
flag was removed and the app joined the table unconditionally.

Level is 3 degrees from flat, with 0.6 degrees of hysteresis and a 250ms
dwell, because a two-year-old holding a puck cannot hold half a degree and a
band she can never reach reads as broken rather than as strict. Drawing
recomputes every pixel of a repainted rectangle from the model rather than
erasing and redrawing, so residue is impossible by construction rather than
by bookkeeping (`emulator/wasm/tests/repro-level-bubble-residue.ts` asserts
an incrementally updated screen is bit-identical to a freshly entered one
after motion). Measured cost: 3.3% of the panel on an average drawing frame,
12.2% on the one frame the verdict changes, nothing at all when still.

```powershell
bun run emulator/wasm/build.ts
bun run emulator/wasm/tests/feature-level.ts
bun run emulator/wasm/tests/repro-level-bubble-residue.ts
bun tools/preview-level.ts     # preview/level-*.png
```

## Capturing real handwriting

`tools/capture-server.ts` serves a local, real `tldraw` (npm, bundled by
`tools/build-capture.ts`, bound to 127.0.0.1). Draw, press "Send to device", and
the strokes land in `capture/handwriting.json`. `tools/gen-from-capture.ts`
fits them to the panel and emits `firmware/strokes.h`.

Two traps found the hard way, both fixed but worth knowing:

- tldraw's shipped ESM reads bare `process.env.X`, which is undefined in a
  browser, so the page renders **blank with no error**. `index.html` stubs
  `window.process` and the build defines the vars.
- The app deliberately avoids JSX. Bun picks the dev vs production JSX runtime
  by its own heuristics and got it wrong both ways here: unminified builds
  emitted `jsxDEV` calls React's production runtime does not export, and
  minified builds hid the error behind mangled names.

Also: the renderer stamps a disc per point, so points must be closer together
than the brush radius or a fast stroke breaks into a dotted line. The converter
densifies every gap. Any new stroke source must do the same.

## Design notes

The letterforms come from Hershey `futural`, a single-stroke font, because a
pen path is what makes "being handwritten" animate naturally. Outline fonts
would need a clipping mask instead. `futural` was chosen over the cursive
`scriptc` because tldraw's own face (Shantell Sans) is printed and casual
rather than joined-up.

The generator humanises the plotter-perfect Hershey vectors: per-letter
rotation and baseline wobble, a whole-line tilt, tightened tracking, narrowed
word spaces, and a low-frequency wander along each stroke.

**The ink itself is tldraw's, not ours.** `tools/tldraw-freehand/` holds
tldraw's real stroke pipeline, vendored byte-identical from `tldraw/tldraw`
(`packages/tldraw/src/lib/shapes/shared/freehand/`). The generator feeds each
centreline through their `ingest` (streamline, resampling) and `computeRadii`
(pressure simulation, thinning, end tapers) using the exact options their draw
tool uses for a mouse or finger stroke, from `shapes/draw/getPath.ts`:

```
size: strokeWidth, thinning: 0.5, smoothing: 0.62,
streamline: modulate(strokeWidth, [9,16], [0.64,0.74], true),
easing: easeOutSine, simulatePressure: true, last: true
```

`tools/tldraw-editor-shim.ts` supplies the two symbols those files import from
`@tldraw/editor` (`Vec`, `VecLike`), both type-only on the paths we call, so
the vendored sources need no edits. `tsconfig.json` maps the import.

Each point's radius is baked into `strokes.h`, and the firmware just stamps a
disc of that radius. That is geometrically the same shape perfect-freehand
draws, since its outline is the boundary of exactly those circles, and unlike a
closed outline polygon it can be revealed progressively for the animation.

Watch the scale: tldraw's `radius` is a half-width offset of roughly
`0.92 * size`, so a stroke is about `1.85 * size` wide. `--stroke 4.4` gives a
max radius near 3.6 px, which is right for 40 px glyphs. `--stroke 7` fills in
the counters.

`tools/gen-strokes.ts` renders the exact same rasteriser as the firmware to a
PNG, so the look can be judged in `preview/` without a flash cycle. Use it.
