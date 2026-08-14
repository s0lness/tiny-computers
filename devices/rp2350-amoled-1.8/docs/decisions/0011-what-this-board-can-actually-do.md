# 0011: A compass, a clock, and a few bytes that survive

Date: 2026-08-15
Status: two answers settled on documents, one owed to a probe that was built
and deliberately not flashed

Three apps the owner wants next depend on capabilities this firmware has never
used: a compass, a clock, and a high score. Each rests on an assumption about
the hardware that nobody here has checked. This document checks them, before
anyone writes an app on top of a guess.

The method was the schematic, four datasheets, and the firmware's own source.
Where that was not enough, a standalone probe image was built
(`firmware/probe/`) and left unflashed, because the owner was asleep and
flashing is a physical act on a device someone might be holding. What the
probe would tell us is at the end, along with what it could not tell us.

Everything below that says "the schematic" means the published PDF for this
exact board, `files.waveshare.com/wiki/RP2350-Touch-AMOLED-1.8/
RP2350-Touch-AMOLED-1.8.pdf`, read page by page rather than searched. Net
labels are quoted as they appear on it.

## Summary

| Question | Answer |
|---|---|
| A compass | **Impossible.** No magnetometer exists on this board, and the one the driver header describes is a chip we do not have. |
| A clock across a power cycle | **Possible, and better than assumed.** There is a real RTC with its own crystal on an always-on rail. One fact is still owed to hardware. |
| A few bytes in flash | **Yes, and it is safer than the bug that cost a day suggests**, because the fix for that bug removed the hazard. Four constraints, all cheap. |

---

# 1. A compass is impossible, and the code will tell you otherwise

## The verdict

**Impossible.** Not "possible but lying", not "possible with drift". There is
no sensor on this board that can see a magnetic field, so there is no quantity
to lie about in the first place.

## The evidence, in the order it should be checked

**The part is six-axis.** The schematic names it `QMI8658C` (U7). QST's own
datasheet for that part number titles it "6D Inertial Measurement Unit with
Motion Co-Processor", and its Table 8, "Gyroscope Electro-Mechanical
Specifications", is the last sensor table in the document. Accelerometer and
gyroscope. Nothing else.

**The board has no magnetometer either.** The schematic carries eight
integrated circuits: `U1` AXP2101 (PMIC), `U2` PCF85063ATL (RTC), plus the
RP2350A, a `W25Q128JVSIQ` flash, an `ES8311` codec, the touch controller, the
IMU, and the panel. Searching the full page text for every common magnetometer
family used with this class of part returns nothing: no AK09918, no QMC5883,
no LIS2MDL, no MMC5603, no IST8310, no BMM150.

**And the IMU's own auxiliary bus is parked.** The QMI8658 can act as an I2C
master for an external magnetometer; that is what its `SDx` and `SCx` pins
are for. On this board both are tied straight to `3V3`. They go to no device.
That is not an omission in the schematic, it is the schematic saying the
feature is unused.

## The trap, which is worse than the absence

Everything a person would naturally read to answer this question says a
magnetometer is available.

`firmware/lib/QMI8658/QMI8658.h`, which is in this repository and which we
compile, defines `QMI8658_CTRL7_MAG_ENABLE`, `QMI8658_CONFIG_ACCGYRMAG_ENABLE`,
an `enum QMI8658_MagOdr` with six output rates, an `enum QMI8658_MagDev`
naming `MagDev_AKM09918`, a `magnetometerData` pointer inside
`FisImuRawSample`, and six registers `QMI8658Register_Mx_L` through
`Mz_H` at addresses 65 to 70. The datasheet has a "Magnetometer Sensitivity"
row and a whole current-consumption table for "9DOF Attitude Engine Mode (with
Magnetometer)".

All of it refers to a magnetometer **the host must supply**. The QMI8658
provides the registers, the sampling and the fusion; the chip is somebody
else's, on the aux bus, and on this board there is nobody there. Enable
`MAG`, read registers 65 to 70, and they will return something. It will be
zeros or stale bytes, and it will look exactly like a compass that needs
calibrating.

This is the shape of defect this project already knows: a vendor header that
describes a board other than ours. Decision-adjacent precedent is in AGENTS.md
under "The buttons, which the vendor header describes wrongly", where
`SYS_OUT` was documented as a key and is a power-good line. Same failure, new
peripheral.

**Rule: nothing in this firmware may write `QMI8658_CTRL7_MAG_ENABLE` or read
registers 65 to 70. Not for a test, not for an experiment.** A magnetometer
register that answers is the most convincing possible lie.

## Why the gyroscope cannot stand in

A heading integrated from yaw rate has two error terms. One is random noise,
and it is irrelevant here: the datasheet gives 15 mdps/root-Hz, which is an
angle random walk of about 0.9 degrees per root-hour, so about 0.1 degrees of
wander over a minute. Nobody would notice that.

The other is bias, and it is fatal. A residual zero-rate offset of 1 dps is 60
degrees of drift per minute. Even 0.1 dps is 6 degrees per minute, so a needle
left alone during one conversation has turned a quarter turn for no reason.
**QST publishes no zero-rate output or bias-stability figure for the QMI8658C
at all**, which is itself the finding: the error that decides whether this
works is the one the manufacturer declines to bound. The part has a gyro-bias
calibration command (`QMI8658_Ctrl9_Cmd_GyroBias`), and it would help, and it
would not make the number knowable in advance.

A toy that sits still on a table for minutes at a time is the worst case for
exactly this term. A child would set the puck down pointing at the door, pick
it up after lunch, and the needle would be pointing at the window, having
turned while nothing moved. That is not a compass with an accuracy problem, it
is an object that does not tell the truth, and a four-year-old will learn that
about it faster than an adult would.

## What to build instead, and why it is not a consolation prize

The thing worth wanting here is real: **a child points the puck and a needle
swings.** That does not require magnetic north. It requires a needle that
answers the hand instantly and never lies.

**Build a needle that points downhill.** The accelerometer measures the
gravity vector, always, with no integration, no drift and no calibration. Take
its X and Y components in the plane of the panel and you have an exact
direction: the way a marble would roll if the panel were a table. Tilt the
puck and the needle swings at once; hold it flat and the needle goes calm and
loses its point, which is honest, because on a level surface there is no
downhill. Set it down and come back an hour later and it reads exactly the
same as when you left it.

It has properties a magnetic compass on this device would not have had:

- **It cannot be wrong.** There is nothing to calibrate and nothing to drift.
- **It responds to the gesture the child actually makes**, which is tilting
  and pointing, not walking in a circle.
- **It is legible without reading.** Down is a concept she already has.
- **It is drawn the same way**, a round face and a swinging needle, so nothing
  about the visual design of "a compass app" is lost.

Two variants worth prototyping, in this order: a **needle** pointing downhill
with a magnitude that grows with tilt, and a **ball in a bowl** that rolls to
the low side and rattles when shaken. The second is closer to a spirit level,
uses the same signal, and is probably more fun to hold.

If the owner still specifically wants a heading that responds to turning the
puck around, the honest version is a **"set your own north"** toy: press to
zero, then it tracks the turn from that moment. It must be told, on screen and
in one wordless way, that it forgets. It has a life measured in tens of
seconds, and the number that decides whether that is thirty seconds or three
minutes cannot be looked up, only measured on this puck. **Do not ship this
without measuring it**, and do not call it a compass.

---

# 2. The device can keep time across a power cycle

## The verdict

**Possible.** This board carries a genuine real-time clock with its own
crystal, on a rail the PMIC keeps alive when everything else is off. This
corrects the assumption the question was asked with. It does not correct it
all the way to "solved": one fact about this particular puck is still owed to
hardware, and it decides which of two products the clock is.

## What is actually on the board

**A PCF85063ATL** (`U2` on the schematic), NXP's tiny RTC/calendar, with a
dedicated `32.768KHz` crystal `Y1` and its two 22pF load capacitors. `CLKOE`
is tied to ground and `CLKOUT` is marked no-connect, so the part exists solely
to count time. `INT` goes to `RTC_INT`, which the schematic maps to **GPIO3**,
a pin that is not in AGENTS.md's table and should be added.

**It is on i2c1, the same bus as everything else.** `RTC_SDA` is GPIO6 and
`RTC_SCL` is GPIO7, identical to `QMI_SDA`/`QMI_SCL` and
`AXP2101_SDA`/`AXP2101_SCL`. Its address is fixed by the part at 0x51 (A2h
write, A3h read).

**Its supply is the PMIC's always-on RTC LDO.** The schematic runs AXP2101
pin 28, `RTCLDO`, to the net `VCC-RTC`, decoupled by `CP12` 2.2uF, and
`VCC-RTC` is the PCF85063's `VDD`. The AXP2101 datasheet, X-Powers Rev 0.1,
section 7.5.4, says it in one sentence:

> PMU has power off and power on status. When at off state, all voltage
> outputs are turned off except RTCLDO. At this time, the total power
> consumption is typically 25uA.

The front page repeats it as a headline feature: "Power off current <20uA
(BATFET off, RTCLDO output on)".

**A backup cell is designed for and not fitted.** AXP2101 pin 27, `VBACKUP`,
goes to the net `VBAT2`, and the board's own rail table names `VBAT1` "CHG
BAT" and `VBAT2` "CHG RTC". `VBAT2` terminates at `H1`, a two-pin part the
schematic marks **`NC`**. Those are the "backup battery pads" the product page
advertises: pads, not a battery.

## So what actually survives what

| Event | Does the clock keep counting |
|---|---|
| Firmware reboot, watchdog, app switch | Yes, trivially |
| PWR held until the rails drop, battery still attached | **Yes.** Off state keeps RTCLDO up at about 25uA |
| USB unplugged, running on battery | Yes |
| Battery goes flat, or is unplugged, with no cell at H1 | **No.** The AXP2101 calls this power-on reset and turns off "all voltage outputs including RTCLDO and VREF" |

There is one open fact, and it is the one that matters most: **is a battery
actually plugged into J1 on this puck.** The evidence says yes without proving
it. AGENTS.md's recovery ritual is "unplug, hold PWR for at least 12 seconds
until the screen goes black, then hold BOOT while plugging the cable back in",
which only describes a device that runs with no cable attached. Decision 0005
records the device "fully off (rails down, charge LED lit, no USB)". Both read
like a board with a cell in it. Neither is a measurement, and `REG00H[3]` on
the PMIC answers it in one read. The probe reads it.

## What the clock honestly is, in each case

**If a battery is fitted (expected):** a real clock. Set once, right until the
battery goes flat, across every reboot and every deliberate power-off in
between. The charge cycle of the toy becomes the clock's lifetime, which for a
device that lives on a shelf and gets charged is a good bargain, and the
failure is loud rather than silent (see below).

**If no battery is fitted:** a clock that is right until the cable comes out.
That is not a clock, that is a stopwatch with a face, and it should be
designed as a different product: something the owner sets deliberately, that
shows plainly it has forgotten, rather than something that quietly shows the
wrong time. **Do not paper over this case.** A wrong clock that looks
confident is worse than a blank one.

The good news is that the device can always tell which case it is in, with no
heuristic. The PCF85063's Seconds register carries an **OS flag** in bit 7,
set whenever the oscillator has stopped, which is to say whenever the RTC
domain lost power. NXP's data sheet, section 7.3.1.1:

> When the oscillator of the PCF85063A is stopped, the OS flag is set ... This
> method can be used to monitor the oscillator and to determine if the supply
> voltage has reduced to the point where oscillation fails.

So the clock app never has to guess. `OS == 1` means "I do not know what time
it is", and that is a picture to draw rather than a number to fake. Whatever
the app shows in that state is the single most important screen in it.

## The constraint this puts on the firmware, and it is not small

**The RTC is on i2c1, so core1 owns it.** `sensors.h`'s ownership banner is
absolute: once `sensors_start()` has run, core0 may never touch that bus, "not
the touch controller, not the IMU, not the PMIC, not a debug read". The RTC
joins that list. There is no exemption available for a clock, and a clock app
that reads the RTC directly is decision 0004 waiting to happen again.

The shape that fits what already exists:

- **Read the RTC once, in `sensors_init()`**, on core0, before
  `multicore_launch_core1()`, exactly where the FT3168, the AXP2101 threshold
  write and the ES8311 bring-up already happen. Publish seconds-since-epoch
  plus the OS flag as a plain value.
- **Run wall-clock time off `time_us_64()` from that base.** The RP2350's
  timer is perfectly good at counting the seconds since boot; the RTC is only
  needed to answer "what was the time when I woke up".
- **Setting the time is a write, so it goes where the power-off request
  already goes**: a core0-owned request flag, executed by core1, the same
  one-writer-one-reader pattern as `g_poweroffRequested`. Do not invent a
  second mechanism.
- **Nothing polls the RTC per frame.** There is no reason to. A second read,
  hours later, would only be worth it to correct the RP2350 timer's own drift,
  and that is a refinement, not a requirement.

---

# 3. Yes, we can write a few bytes to flash, and the reason is decision 0004

## The verdict

**Yes.** And the honest answer is the opposite of the one the question
expected: this is not a project where flash writes are especially dangerous,
it is a project that already paid, in full and in advance, for the thing that
makes them dangerous.

## Why the old hazard is gone

Decision 0005's chain was: `bootbtn.c` floats the flash chip select for 60 to
100us every 50ms with interrupts off; that protects only the calling core;
core1 executed its whole sensor loop from flash over XIP; a core1 instruction
fetch during a borrow returned garbage; core1 stopped.

Every link in that chain except the first was deleted by the fix.
`pico_set_binary_type(main copy_to_ram)` in `firmware/CMakeLists.txt` copies
the entire image to SRAM at boot. **No code and no constant on either core is
fetched from flash at runtime**, which decision 0004 verified against
`objdump` and `nm` rather than by inspection, and which
`tools/invariants/rules/rp2350-amoled-1.8.ts` rule 1 now enforces at build
time ("no executable byte at a flash VMA, outside the boot allowlist").

The flash chip is, at runtime, an unused peripheral. That is precisely the
condition under which writing to it is easy.

## The hazards that remain, all four of them

**1. A chip-select borrow during a flash transaction.** This is the mirror
image of the old bug: not "core0 takes the flash away while core1 needs it",
but "core0 takes the flash away while core0 is mid-erase". It cannot happen
today, because `bootbtn_poll_level()`/`bootbtn_poll_clicked()` are called only
from `rtcore_tick()` on core0, never from an interrupt (`sensors.c`: "BOOT
sampling is core0-only already"), and core0 blocked inside a flash call is
core0 not calling anything else.

That is a fact about scheduling, not about design, and decision 0005's whole
lesson is that such facts die in refactors. So use `flash_safe_execute()`
anyway, which disables interrupts on the calling core for the duration and
makes the property structural instead of circumstantial.

**2. Reading the saved bytes back from core1.** Reading through the XIP window
is a flash access like any other, and a read that collides with a borrow does
not crash, it returns wrong bytes, silently. **Rule: saved bytes are read on
core0, once, at boot, before `sensors_start()`.** Never on core1, never
repeatedly, never per frame.

**3. The core1 liveness guard, which is the interesting one.**
`runtime.c` treats core1 as dead after `CORE1_STALL_MS` (1500ms) with no
change in its loop counter, and the punishment is not cosmetic: it calls
`sensors_restart_core1()`, a hardware core reset plus an i2c1 bus recovery
plus a relaunch. Parking core1 for a flash write walks straight into it. One
4KB sector erase is 45ms typical and **400ms maximum** (Winbond W25Q128JV Rev
H, AC Characteristics, tSE), so a single-sector save is safe with margin. Two
sectors at worst case is 800ms, still under. Four is 1.6 seconds and the board
punishes itself for saving a high score.

The watchdog is a second, looser ceiling: `RUNTIME_WATCHDOG_MS` is 4000ms and
is fed at the bottom of the loop, so the same arithmetic gives up at about ten
sectors.

**Rule: one sector per save operation, ever. No exceptions, no loops.**

**4. `flash_safe_execute` has a precondition nobody has met yet.** It returns
`PICO_ERROR_NOT_PERMITTED` (or asserts, since `PICO_FLASH_ASSERT_ON_UNSAFE`
defaults to 1) unless the other core has called `flash_safe_execute_core_init()`.
So `sensors.c`'s `core1_entry()` gains one line, and `sensors_restart_core1()`
must be checked, because a relaunched core1 needs it again. `pico_flash` and
`hardware_flash` also have to be added to `target_link_libraries`; neither is
linked today.

## What it costs, in the units that matter

| Operation | Typical | Worst case (datasheet) |
|---|---|---|
| Program a 256-byte page (tPP) | 0.4ms | 3ms |
| Erase one 4KB sector (tSE) | 45ms | 400ms |

Core1 is parked for the whole of it, so a save costs exactly that much lost
sensor sampling. Core1 runs above one million loops per second, and the touch
controller is configured for a 10ms scan period, so a 45ms park drops on the
order of four touch samples and a 400ms park drops forty. Core0 is blocked too,
so the frame does not advance either.

The consequence, stated in the only way that matters here: **a 400ms save
during a stroke would eat most of a line the child was drawing.** A 0.4ms save
is a tenth of one panel push and nobody will ever see it.

That difference is the entire design.

## The design that follows, and it is small

**One 4KB sector, append-only.**

- **Where.** Offset `0x00FFF000`, the last sector of the 16MB part, XIP
  address `0x10FFF000`. The live image is 108,656 bytes at offset 0, so this
  is 15.9MB away from anything. It is also above every region
  `store/partitions.json` ever named (`slot_b` ends at 15,728,640, the
  manifest at 15,732,736). If the golden-image crash-recovery fallback in
  `store/` is ever revived, this sector must be declared in that table rather
  than squatted.
- **What.** Fixed-size records, 16 bytes: magic, version, payload, CRC. Append
  the next free one on each save. That is one page program, 0.4ms typical.
- **When to erase.** Only when the sector is full, so once every 256 saves.
  A child would have to beat their own high score 256 times to pay for one
  45ms hiccup, and the sector's rated endurance is "min. 100K program-erase
  cycles per sector", so at one erase per 256 saves the flash outlives
  everyone involved.
- **Torn writes.** A power cut mid-program corrupts at most the record being
  written, and the CRC catches it; the previous record is still there and
  still valid. A power cut mid-erase costs the whole sector, which is why the
  erase is rare and why a save must never be the only copy of anything the
  child can see on screen.
- **When to save.** On the event that made the value worth keeping: a new high
  score at the end of a game, the clock being set. Additionally, flush on the
  firmware-initiated soft power-off, which `sensors_request_poweroff()`
  already has a path for.

## What I would not do

**I would not build a general-purpose storage layer.** Not a key-value store,
not a settings API, not a wear-levelling scheme with two sectors and a
generation counter. Four apps need, between them, a high score and a clock
offset. A record format with a magic and a CRC is thirty lines. The moment it
becomes a subsystem, someone will write to it from an app, on a timer, from
core1, or in a loop over several sectors, and every one of those is a bug this
document exists to prevent.

**I would not save at power-off as the primary mechanism.** It sounds
disciplined and it does not work here. Of the ways this device loses power,
the firmware only sees one. The 10-second PWR hold is decided inside the
AXP2101 and cuts the rails with no warning; a flat battery gives no warning at
all; `PWROK` on GPIO18 is a power-good indicator, and by the time it moves
there are not 45 milliseconds left, let alone 400. Save on the event, and
treat the soft power-off flush as a bonus rather than the design.

**I would not save on a timer, or per frame, or on every value change inside a
game.** Save when a value becomes worth keeping, which is a handful of times
per session.

**I would not put the clock in flash at all.** If the RTC survives, the time
needs no flash: the chip is the storage. And the PCF85063 carries one byte of
general-purpose RAM at register 03h, backed by exactly the same supply as the
counters, with no erase cycle and no endurance limit. A high score of 0 to 255
fits in it. **That is the right home for anything that only needs to survive a
power-off**, and flash is for the strictly harder job of surviving a flat
battery. If the probe comes back saying the RTC domain holds, the first four
apps may need no flash writes whatsoever, and the correct amount of storage
layer to build is none.

---

# The probe: built, not flashed

`firmware/probe/rtcprobe.c` plus its own `CMakeLists.txt`. It is a separate
CMake project, not a second target in `firmware/CMakeLists.txt`, so a
diagnostic cannot change a byte of the image the owner's device runs and the
product build does not gain a dependency on `pico_flash`. It never touches the
panel. Both variants build clean:

```powershell
# read-only, the one to flash first
cmake -S firmware/probe -B firmware/probe/build -G Ninja `
  -Dpicotool_DIR="$env:USERPROFILE\pico\tools\picotool-dist\picotool" `
  -Dpioasm_DIR="$env:USERPROFILE\pico\tools\sdk-tools\pioasm"
cmake --build firmware/probe/build
# -> firmware/probe/build/rtcprobe.uf2   (built 2026-08-15, NOT flashed)

# adds section D, which writes one flash sector
cmake -S firmware/probe -B firmware/probe/build-flashtest -G Ninja -DPROBE_WRITE_FLASH=1 ...
# -> firmware/probe/build-flashtest/rtcprobe.uf2  (built, NOT flashed)
```

It is built `copy_to_ram`, like the product image, because section D's numbers
only transfer to the real firmware if the probe runs under the same rule.

**What each section would tell us:**

- **A, bus scan.** Whether 0x51 answers at all. An address that does not appear
  is the answer for anything expected. This is the difference between "the
  schematic says there is an RTC" and "there is an RTC".
- **B, the RTC.** The OS flag, the current time, and a magic byte written into
  `RAM_byte`. Two samples 1.1 seconds apart prove the oscillator is actually
  counting, which `OS == 0` alone does not. **This is the section the clock app
  depends on.**
- **C, the PMIC.** `REG00H[3]`, battery detect. One bit, and it decides whether
  the clock is a clock or a stopwatch with a face. Also `REG18H[2]`, the backup
  charger enable, which on this board charges an empty pair of pads.
- **D, the flash write** (only with `-DPROBE_WRITE_FLASH=1`). Real tSE and tPP
  on this specific part, and the measured cost of parking a core across them,
  against a stand-in core1 spinning at full rate. It writes only the last
  sector, 15.9MB clear of the image.

**The protocol for section B, which is the whole point:**

1. Flash the read-only build. Read the output. It writes a plainly synthetic
   stamp (2000-01-01 12:00:00) and the magic byte on first run.
2. Unplug USB. Hold PWR for 12 seconds until the screen goes black.
3. Wait a measured number of minutes by a wall clock.
4. Plug back in, power on, read the output again.

If the clock advanced by that number of minutes with `OS == 0` and `RAM_byte`
still holding the magic, **this device keeps time across a power cycle** and
the clock app is a clock. If `OS` came back 1, it does not, and the clock app
is a different product.

---

# What I could not establish

Stated plainly, because a document four apps will be built on is worth less if
its confident parts and its guesses look alike.

1. **Whether a battery is fitted to J1 on this puck.** Everything in this
   repository reads like a device that runs unplugged, and nothing measures it.
   Probe section C, one bit.
2. **Whether `RTCLDO` on this board's AXP2101 configuration actually holds
   `VCC-RTC` up through a deliberate power-off.** The datasheet says off-state
   keeps RTCLDO alive, and the schematic wires it correctly, and neither is the
   same as watching the seconds advance across a real power cycle. Probe
   section B.
3. **The QMI8658C's gyroscope zero-rate output.** QST does not publish one. It
   is the only number that would decide whether a "set your own north" toy has
   a useful life, and it can only be measured on this puck.
4. **Real tSE on this flash.** Everything above uses the datasheet's 45ms
   typical and 400ms maximum, and designs for the maximum. Probe section D.
5. **Whether `flash_safe_execute`'s lockout engages against sensors.c's real
   core1.** Probe section D uses a stand-in core1 that spins and counts. The
   real one installs fault handlers and runs bounded i2c transactions, and
   although it never uses the inter-core FIFO the lockout needs, and never
   disables interrupts, that is read from the source rather than observed.
   This is the probe's own honest gap, and it is the same shape as the one
   `tools/README-devlink.md` records about injected buttons.
6. **Whether the `store/` golden-image fallback will ever claim the last
   sector.** It does not today. If it is revived, `partitions.json` must
   declare the settings sector rather than leave it squatted.

Also worth recording, since it belongs to AGENTS.md's pin table rather than
here: **GPIO3 is `RTC_INT`**, the PCF85063's interrupt line, and the table does
not currently list it.

---

# What I would build first

The needle that points downhill. It needs no new driver, no new bus traffic,
no storage, and no answer from the probe: the accelerometer is already sampled
on core1 for the sketchpad's shake detection, and every value it needs is
already crossing to core0. It is the one of the four that can be built today,
in an afternoon, and be honest about what it shows.
