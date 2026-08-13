# rp2350-amoled-1.8

Firmware for the **Waveshare RP2350-Touch-AMOLED-1.8**, a 368x448 AMOLED in a
small plastic puck.

**Current app: a sketchpad.** Draw on the touchscreen with a finger, shake the
puck to erase. Strokes are anti-aliased with variable width, shaped by a
tldraw-style pressure model, so they read as ink rather than as pixels.

Earlier apps, still reachable in git history and in `tools/`, replayed
handwriting: first synthesised from a font (which always looked typeset), then
a real capture of a hand drawing in a local tldraw. The capture rig is still
useful and still works, see "Capturing real handwriting" below.

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
| Audio I2S DOUT / DIN / MCLK / BCLK | 20 / 21 / 22 / 24 |
| Speaker amp enable | 19 |
| microSD CS / MOSI / SCK / MISO | 25 / 26 / 27 / 28 |

Taken from the published schematic
(`files.waveshare.com/wiki/RP2350-Touch-AMOLED-1.8/RP2350-Touch-AMOLED-1.8.pdf`),
not inferred from the demo code.

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
- A menu of three tiles across the 448px landscape width gives each tile about
  149px, which is only about two child fingers. That is usable, and it is much
  closer to the limit than the pixel count suggests.
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
firmware/          the app: main.c, CMakeLists.txt, generated strokes.h
firmware/lib/      Waveshare drivers, copied verbatim from the vendor demo
docs/decisions/    why things are the way they are (AGENTS says how, this says why)
tools/gen-strokes.ts   generates strokes.h + a PNG preview
tools/fonts/       Hershey single-stroke fonts (public domain)
preview/           host-rendered PNGs, for judging the look without flashing
vendor/            the untouched Waveshare demo download, for reference
backup/            factory firmware pulled off the board before first flash
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

## Gotchas that bite

- **We carry a patch to `AMOLED_1IN8_DisplayWindows`.** Upstream's DMA loop is
  `for (i = Ystart; i < Yend - 1; i++)`, which sends one row fewer than the
  window `SetWindows` just declared to the panel (it programs `Yend-1` as the
  inclusive last row). The bottom row of every partial refresh silently never
  updated. The same file's `Display()`/`Clear()` use the correct bound, which
  is what makes it a bug rather than a convention. `lib/AMOLED/AMOLED_1in8.c`
  is fixed here to `i < Yend`. **If you ever re-copy the driver from the
  Waveshare zip, this patch is lost** and partial updates will start dropping
  their last row again.
- **Never push a narrow window.** `push_dirty` pads every pushed rectangle to
  at least 64 pixels wide and aligns it to 8. This is not tidiness, it is the
  fix for the defect that made strokes look shredded, and removing it brings
  the defect straight back. `AMOLED_1IN8_DisplayWindows` sends **one DMA
  transfer per row**, so a window's width is the size of each transfer. A
  vertical stroke produces a tall, narrow dirty rect, which becomes hundreds of
  transfers of a few dozen bytes, and those come out corrupted: the ink is
  displaced sideways in bands, so a vertical line reads as a ladder of
  horizontal ticks. The diagnosis is in `docs/decisions/0001-push-min-width.md`.
  Long-press PWR runs `push_bisect_test()`, which draws the same vertical
  stroke under four alignment and width policies side by side.
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
  to move or the panel to sleep. The current app wipes and redraws, which is
  enough.
- **The factory firmware is in `backup/factory-firmware.uf2`.** Restore with
  `picotool load backup/factory-firmware.uf2 -f -x`.

## The sketchpad (current `firmware/main.c`)

Touch and IMU are both polled from the main loop. **Do not move either onto an
interrupt.** The vendor demo drives touch from a GPIO IRQ and guards the shared
`i2c1` bus with a plain `uint8_t i2c_lock` using `while(lock); lock=1;`. That is
non-atomic check-then-act on a non-`volatile` flag, so an edge arriving in the
gap lets the ISR start an I2C transaction on top of an in-flight one. Polling
sidesteps it completely.

`FT3168_Get_Point()` does **not** tell you whether a finger is down; when the
finger count is zero it returns without touching the struct, leaving the last
coordinates in place. Read `FT3168_ReadState(FT3168_FINGER_NUMBER)` separately
and treat that as the authority for stroke start and end. Clamp the coordinates
too: they come straight from 12-bit touch registers and the driver never
validates them.

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
in a point. All the constants are `#define`s at the top of `main.c`.

Shake detection requires several jolts inside a rolling window rather than one
big reading, because a single spike is indistinguishable from a firm tap. It is
suppressed while a finger is down, and has a cooldown so one shake cannot erase
twice. Erase is an animated wipe in 16 bands, not an instant blank.

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
