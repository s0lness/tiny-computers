# rp2350-amoled-1.8

Firmware for the **Waveshare RP2350-Touch-AMOLED-1.8**, a 368x448 AMOLED
touchscreen in a small plastic puck. A single-binary runtime holds three apps
(a stopwatch, a sketchpad, a countdown timer) plus a menu, switched by a
function call rather than a reboot; see `AGENTS.md` for the current shape.

`AGENTS.md` says how to build, flash and work on it. `docs/decisions/` says why
things are the way they are. This file is the bring-up log: what the hardware
and the vendor code actually do, as opposed to what they claim to. It predates
the single-binary runtime (see its own "Bring-up log" note below on which
vendor demo drop it applies to) and its findings are about the driver and the
silicon, not about that later restructuring, so they still hold.

## Bring-up log

Everything below was found while getting this board working between 10 and 11
August 2026, and verified on hardware. It applies to the vendor demo
`RP2350-Touch-AMOLED-1.8.zip` downloaded 2026-08-10 from the Waveshare wiki,
whose drivers are stamped V1.0 / 2025-03-20. A later drop may differ.

This is a log of this board, not a guide to embedded development. It is here
because the same surprises will cost the next person the same days.

### Display: `AMOLED_1IN8_DisplayWindows()`

Three separate defects in one function. It sends one DMA transfer per row of
the window, so the window's **width is the size of every transfer** and its
height is the number of them. That single fact explains all three.

| # | What happens | Fix lives in | Upstream |
|---|---|---|---|
| 1 | Loop bound is `i < Yend - 1`, one row short of the window it just declared, so the bottom row of every partial refresh never updates | patched in `firmware/lib/AMOLED/AMOLED_1in8.c` | [LCD-3.5#2](https://github.com/waveshareteam/RP2350-Touch-LCD-3.5/issues/2) |
| 2 | Chip select is deasserted as soon as the DMA reports done, but that only means the PIO FIFO has been fed; the state machine is still shifting, so the tail of every transfer is truncated | patched in the same file (`AMOLED_1IN8_WaitShiftOut`) | [AMOLED-1.75#4](https://github.com/waveshareteam/RP2350-Touch-AMOLED-1.75/issues/4) |
| 3 | Output is corrupted unless each row is a multiple of 8 pixels (16 bytes) long | our own `gfx_push()` in `firmware/runtime/gfx.c` (formerly `push_dirty()` in the pre-runtime `main.c`) | [AMOLED-1.75#3](https://github.com/waveshareteam/RP2350-Touch-AMOLED-1.75/issues/3) |

Two of those are patched **inside the vendor driver**, which means they are lost
if the driver is ever re-copied from the zip. Number 3 is deliberately not:
rounding the rectangle before handing it over needs no driver change, so it
survives.

Number 2 is the only one that could not have been done at app level. It sits
between two statements inside one driver function, with no hook in between.

Number 3 is a hardware or framing constraint rather than a coding error, and
its mechanism is **not established**: the DMA is byte-sized and the PIO
autopulls at an 8-bit threshold, so neither has an obvious reason to care about
16-byte multiples. The bisect that produced the rule is in
`docs/decisions/0001-push-min-width.md`. Do not extend the speculation here;
extend the measurement.

### Touch: the FT3168 dies intermittently, and it is the vendor init

`FT3168_Init()` writes `0x01` to `REG_POWER_MODE` (`0xA5`), which is **MONITOR**
in the driver's own `Device_Mode` enum, not ACTIVE. Per the FT3168 datasheet,
in monitor mode the chip does no touch tracking at all, and, critically:

> When FT3168 is in Monitor mode or Sleep mode, host will not be able to
> communicate with the touch IC via I2C after accessing any other slave device
> on the same bus.

The vendor demo polls a QMI8658 IMU on that same bus. So touch stops
responding, and it stops in the most confusing possible way: the chip still
answers its WhoAmI, and the coordinate registers still hold the last real
touch, so everything looks alive while the finger count never leaves zero.

Fix, app level: write `0x00` (ACTIVE) after init (`touch_set_active()` /
`touch_set_active_to()` in `firmware/runtime/sensors.c`, formerly in
`main.c`). `sensors.c` also re-arms the chip if nothing has been reported for
a while (`touch_recover_core1()`, `TOUCH_STALL_MS`), which is belt and braces.

Not filed upstream: this board's demo ships as a wiki zip with no repository,
and the one Waveshare repo that does use an FT3168 has a different driver that
never touches the power-mode register.

While you are there: `FT3168_Get_Point()` does **not** tell you whether a
finger is down. With no contact it returns without touching the struct, leaving
the previous coordinates in place. Read the finger count separately and treat
it as the authority. Also read both axes in one burst from `0x03`: the vendor
path reads X and Y in two transactions, so a report landing between them yields
a point built from the new X and the stale Y, which draws a spur at right
angles to the stroke.

### Buttons: `SYS_OUT` is not the button

The vendor header names GPIO18 `SYS_OUT` and the demo arms an interrupt on it
with a `watchdog_reboot` handler, which reads exactly like a power-key handler.
The schematic shows GPIO18 carrying **PWROK** from the PMIC through a BSS138
level shifter. It is a power-good indicator. It does not move when you press
anything, which we confirmed across several sessions before reading the
schematic.

The real path, screen facing you and buttons on the right:

- **PWR is the lower button**, wired to the AXP2101's `PWRON` pin. Firmware
  sees it only second hand: a press pulls **GPIO2** (`AXP_IRQ`) low and latches
  a bit in PMIC register `0x49` (`0x01` release, `0x02` press, `0x04` long
  press, `0x08` short press). Enable them in register `0x41` first; the two
  edge interrupts are off by default.
- **BOOT is the upper button** and produces nothing at runtime.

Long-press gestures are safe: register `0x27` holds the long-press interrupt
threshold (`IRQLEVEL`, 1.5s default, measured at 1480ms; left alone) and the
hard power-off threshold (`OFFLEVEL`; shipped at a 6s default that a 4.5s hold
did not cut power at, now raised at boot to 10s, the field's maximum, since
the menu gesture requires holding PWR past its long-press verdict and the
device is a toy for a child; see `AGENTS.md`).

Not filed upstream: this is board-specific. The ESP32-S3-Touch-AMOLED-1.75's
own hardware reference documents its `SYS_OUT` as a genuine conditioned button
state, and the RP2350-1.75 wires it to a different pin with a different
trigger, so there is no shared defect to report.

### Other pins worth knowing

GPIO16 is the panel's tearing-effect line and toggles continuously at the
refresh rate. It is not a fault, and it will bury any log that watches pins
without debouncing. It is also the correct signal to use if tear-free partial
updates are ever wanted.

There is a microSD slot on GPIO25 to 28, unused by the vendor demo and by us.

## What generalises

Three things here are worth carrying to the next board. The rest is trivia
about this one.

1. **DMA-done does not mean bus-idle when a PIO sits behind the DMA.** The DMA
   has finished writing to a FIFO, not finished transmitting. Any driver that
   deasserts a chip select on `dma_channel_is_busy()` going false has this bug,
   whether or not it has been noticed.
2. **Never trust a vendor demo's power-mode choices, especially on a shared
   I2C bus.** A low-power mode that is fine for a single-slave board can make a
   part unreachable on a multi-slave one, and the datasheet section that says
   so is not the section you would think to read.
3. **Check header pin names against the schematic before writing a handler.**
   A plausible name plus a plausible demo usage is not evidence. This one cost
   several rounds of testing a button that was never wired to anything.

And one about method, which cost more than any of the above: when an artifact
has a property the suspected subsystem cannot possibly explain, that property
eliminates whole layers at once. The display bug was blamed on touch twice
because the reasoning ran from a plausible mechanism toward the symptom. What
broke it open was noticing the corruption tracked stroke *direction*, which
touch sampling has no way to care about. Photographs and a screen recording
supplied that; the firmware's own logs never would have, because they were
correct and reassuring throughout.
