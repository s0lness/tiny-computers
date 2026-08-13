# 0005: RCA, core1 dies on the first button press

Date: 2026-08-13
Status: cause established on evidence, fix not yet verified on hardware

## What the owner experienced

Every time the device was picked up, it was frozen on the stopwatch: screen
lit, sharp, `00:00:00`, responding to nothing. Reported repeatedly as
"bricked". It never was.

## The causal chain

**1. Core1 owns every sensor.** It has exclusive use of `i2c1` and publishes
touch, the IMU and the PMIC. Core0 renders. Decision 0002 section 3, and it
was a measurement: the touch read alone was about 695us, roughly 98 percent
of frame time.

**2. Core1 executes from flash.** Nothing in `sensors.c` is marked
`__not_in_flash_func`. Verified: zero occurrences. Every instruction core1
runs is fetched over XIP.

**3. Core0 periodically takes the flash away.** BOOT is not a GPIO on this
part. Reading it means borrowing the QSPI chip select: `read_cs_low()` in
`bootbtn.c` floats the pad for roughly 60 to 100us, with interrupts disabled,
every 50ms. During that window **flash is unreadable**.

**4. Disabling interrupts protects only the core that does it.** The function
itself correctly lives in RAM, and its own comment says why: "a function
executing in place from flash would fetch its own next instruction through a
chip select it has just taken away." The hazard was understood. It was
handled for the calling core and for nobody else.

**5. So core1 can fetch garbage.** If core1 needs an instruction from flash
during a borrow window, it gets nothing meaningful. A corrupted fetch does
not reliably raise a fault: it can simply branch somewhere and stop making
progress. That is exactly the observed signature, and it is why the fault
handlers recorded nothing.

**6. And it explains WHICH code dies.** Frequently executed code stays in the
XIP cache and is never re-fetched. Rarely executed code must be fetched. The
PMIC's write-1-to-clear runs **only when a real interrupt has been latched**,
which is to say only when someone presses a button. It is therefore almost
never in cache. Touch reads, which run continuously, are cached and immune.

That is the whole bug: **the first button press runs cold code, the fetch
collides with a chip-select borrow, and core1 stops.** The i2c bus is then
left mid-transaction with START and no STOP, which is the corpse everyone
kept examining, not the killer.

## Why it took a day

Recorded separately and at length in decision 0004. In short: four
instruments reported health and each was wrong differently, and the test rig
injected input downstream of the layer that had failed, so every automated
verification passed while the device was unusable by hand.

## The part that hurts

This bug was already known here. The deleted `firmware/main.c` carried:

> A periodic poll was in here briefly and the app hung with a white screen and
> no input, which is exactly what a corrupted instruction fetch looks like.

It had been hit, diagnosed correctly, and fixed by reading BOOT once per
long-press event instead of polling. **The runtime refactor deleted that file
and reintroduced the periodic poll.** The warning died with the file carrying
it.

The lesson is not "be careful in refactors". It is that a hazard recorded only
as a comment next to the code that avoids it will be lost the moment that code
moves. This belongs in a decision record, which is why it is now in one.

## Corroboration

The pico-sdk provides `flash_safe_execute`, documented as "execute a function
with IRQs disabled **and with the other core also not executing/reading
flash**", noting it cannot know whether the other core is running from flash
so it must assume so. The SDK exists to solve exactly this. We were doing the
unsafe thing.

## The fix

**Move core1's execution path into RAM.** `__not_in_flash_func` on
`core1_entry()` and everything it calls, verified against the .map file rather
than by inspection. This removes the hazard rather than scheduling around it,
costs a few KB of about 120KB of spare SRAM, and leaves BOOT polling alone,
which matters because apps now use BOOT clicks to reset.

Rejected alternatives:
- `flash_safe_execute` around the borrow: correct, but it parks core1 twenty
  times a second, which is the sensor latency the core1 split exists to avoid.
- Stop polling BOOT: what the original did, but apps now need BOOT clicks.

## What happens when the device is powered back on

The device is currently fully off (rails down, charge LED lit, no USB). It
needs one physical PWR hold. Then, in order:

1. Flash the RAM-resident core1 build.
2. Confirm core1 alive and idle-stable for a few minutes.
3. One real button press.

### The scenarios, and what each one means

**A. Core1 survives and the stopwatch starts.** The cause is confirmed and the
fix works. Then: ten presses over several minutes, re-enable `sound_init()`
(the chime was cleared of blame and the owner likes it), remove the temporary
instrumentation while KEEPING the permanent core1 liveness counter, and ship.

**B. Core1 survives idle but a press still kills it.** The XIP collision was
not the cause, or not the only one. Then: use the already-built
`PMIC_WRITE_SELFTEST`, which issues the exact fatal transaction shape every
two seconds with no human involved, so the fault can be hammered on the bench.
And investigate whether the AXP2101 clock-stretches during a write to its IRQ
status registers, which would hold the bus legitimately and is not something
any timeout in our code can shorten.

**C. Core1 now dies even at idle.** The RAM move introduced something. Bisect
it: the likely trap is a function that was moved while something it calls was
not, which leaves the fetch in flash anyway.

**D. The device does not power on.** The PMIC needs a VBUS edge: unplug and
replug USB rather than pressing longer.

### What we measure, in every case

- `core1=<n>/s` in the profiler line, and whether it reaches zero
- the phase at death, which says where
- the write-path instruments (`started`, `returned`, `pushed`), where
  `started == returned + 1` proves the call never returned
- the i2c1 registers at the moment of death, not at boot
- a photograph of the panel, since a framebuffer dump shows what the firmware
  believes rather than what the screen does

### The acceptance test, unchanged

Press PWR, the stopwatch starts. Press again, it stops. Ten times, over
minutes. Survival is necessary and not sufficient: a device that survives a
press without acting on it is no better to hold than one that freezes.
