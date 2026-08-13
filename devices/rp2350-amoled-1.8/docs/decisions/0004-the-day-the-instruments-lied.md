# 0004: The day the instruments lied

Date: 2026-08-13
Status: root cause found, fix built, hardware validation pending (device
powered off at time of writing; see "The resolution" below)

A whole working day was lost to one bug. The bug itself is ordinary. What
made it expensive is that every instrument we had reported health, and each
one was wrong in a different way. That is the part worth writing down.

## The symptom, as the owner experienced it

"Every time I pick up the device to play with it, it is frozen on the
stopwatch." The screen was lit, sharp, showing `00:00:00`. Buttons did
nothing. Touch did nothing. Reported several times across the day as "the
device is bricked", which it never was.

## What was actually happening

The RP2350 has two cores. Core1 owns `i2c1` exclusively and publishes touch,
the IMU and the PMIC. Core0 renders. That split is a good design and it was
measured, not guessed (see decision 0002 section 3).

**A single real button press killed core1.**

The PMIC latches a button event. Core1's poll notices it and issues its
write-1-to-clear to register `0x48`. That write is **the first write core1
ever performs**: everything before it, for the whole life of the device, is
reads. The transaction starts and never completes. The i2c peripheral, read
at the moment of the hang, shows START detected, no STOP, master still
active, transmit FIFO empty, and no abort reported. The bus is wedged, and
the hardware raises no error because from its point of view nothing is wrong:
it is waiting.

Core0 carries on rendering a perfect, permanently frozen screen.

So the device was not broken by anything the owner did. It was broken by the
first thing they did.

## Why it took a day

Four instruments told us everything was fine. Each deserves its own line,
because each is a trap that will recur.

**The test rig bypassed the failure by construction.** `devlink` injects
touches and key events over USB, and that injection deliberately enters
*downstream* of core1, in a core0-owned queue, precisely so it cannot become
a second writer of core1's ring. So every hardware verification run all day
passed while the device was unusable by hand. The tool was correct, its
documentation said exactly this, and it still fooled us: the rig tested every
layer except the one that had failed. **A test that cannot fail the way the
product fails is not a test.**

**Every soak measured the one condition under which the fault cannot occur.**
Core1 only performs a write when the PMIC has actually latched something.
Nobody touches a device during an unattended soak. We ran soaks of 25
seconds, 75 seconds, 330 seconds and 10 minutes, all clean, and concluded
stability each time. The fault needs a human finger, and every test was
designed to have none.

**A frozen device is visually identical to a waiting one.** A stopwatch
stopped at `00:00:00` and a stopwatch that will never move again render the
same pixels. Nothing on screen distinguished "ready" from "dead", so the
owner picked it up five times believing it worked. This is now a design
requirement, not an observation.

**And one instrument reported healthy while being broken.** A core1 stack
canary reported 2048 bytes free out of 2048, i.e. a core that calls functions
and touches no stack. That is impossible, so the measurement was wrong. It
was caught, but only because the number was absurd; a plausible wrong number
would have been believed. A broken instrument that reads "fine" is worse than
no instrument.

There was also a fifth, smaller one: `touch reads=0` sat in every profiler
line from the morning onwards. It was seen, and dismissed as "nobody is
touching the panel", which was true and was also the tell.

## Three hypotheses that were wrong, and why that was fine

- **The audio bring-up.** Most recent commit, touches `i2c1`, claims PIO and
  DMA. Eliminated by disabling it and reproducing the freeze anyway.
- **A real gap in pico-sdk.** `i2c_read_blocking_internal()` has an unguarded
  wait loop with no timeout, unlike its write-path twin. Genuine, worth
  closing, and not this bug: the hang is on a write.
- **Cortex-M LOCKUP.** Fitted the evidence beautifully (no fault recorded
  despite handlers installed, stops at unrelated locations). Eliminated by
  reading the halted flag directly: core1 was executing, not halted.

Each elimination cost a hardware round trip and each was worth it. The
failure mode to avoid is not "wrong hypothesis", it is "hypothesis never
tested".

## What changes as a result

- **Core1 liveness is in the permanent profiler line**, not in a debug line
  someone can remove during cleanup. It was removed once during this
  investigation, which immediately made the bug invisible again.
- **A dead core1 must be acted on, not merely observed.** Core0 sees it
  within a second and, before this, did nothing at all. That a bus can wedge
  is an ordinary hardware accident. That a device stays dead afterwards was a
  choice, made by omission.
- **A dead core1 must be visible on the panel.** Subject to decision 0002
  section 4b, which forbids runtime chrome inside an app: a device that has
  lost all its input is arguably not running an app any more, so a failure
  state may show itself where a status indicator may not.
- **Idle soaks are not evidence for input-triggered faults.** Any acceptance
  test for this class of bug has to include real input.

## The resolution, found by a second pair of eyes reasoning from the evidence

The i2c hypothesis this document originally recorded did not survive contact
with the code. Two facts killed it:

1. The local write path requests the STOP correctly (`(last && !nostop)` on
   the last byte, a faithful copy of the SDK's own
   `i2c_write_blocking_internal()`), and "the first write core1 ever
   performs" was never true: `touch_recover_core1()` issues three
   STOP-terminated writes to the touch controller every five fingerless
   seconds, through the same function, and every clean soak executed dozens
   of them.
2. Both wait loops in the write are deadline-bounded, and the timer they
   check demonstrably runs (core0's profiler kept printing off the same
   timer). A wedged bus would cost a 2ms timeout and a counted failure, not
   a frozen loop counter. `g_core1Loops` frozen mid-write means core1
   **stopped executing instructions**. The captured i2c register state
   (master active, FIFO drained, no STOP, no abort) is what the peripheral
   looks like when its feeding core dies between two FIFO pushes: it was
   the corpse, not the killer.

The killer is `bootbtn.c`. Reading the BOOT button requires floating the
flash chip select for on the order of a hundred microseconds
(`read_cs_low()`), and that function protects exactly one core: it is
`__no_inline_not_in_flash_func` and disables interrupts, both on the core
that calls it. Core0 calls it every 50ms. Core1 executed its entire sensor
loop from flash over XIP, with nothing in RAM at all, so any core1
instruction fetch that missed the XIP cache during a borrow went out
through a chip select that had just been taken away and came back as
garbage. A core executing garbage stops advancing its counters without
being halted, without entering its (correctly installed) fault handlers,
and without the i2c peripheral reporting anything wrong - which is
precisely the observed signature. The SDK documents this exact hazard:
`flash_safe_execute()` exists because code touching flash must run "with
IRQs disabled and with the other core also not executing/reading flash".

Two supporting notes from the same review. `SYSCFG.PROC1_HALTED` indicates
debug halt, not LOCKUP, so the "halted=0" reading never actually eliminated
LOCKUP - an escalated fault from a garbage fetch remains fully consistent
with every measurement. And the reliability of a real press, against a
~0.4% borrow duty cycle, has a candidate explanation in the event chain
itself: publishing `KEY_PRESS` makes core0's power-off gesture sample BOOT
on its next tick, which lines a chip-select borrow up with core1 walking
the never-before-executed (therefore never-cached) PMIC clear-write path
within the same millisecond. That last link is plausible rather than
proven; the fix does not depend on it.

## The bug had already been solved once, in a comment a refactor deleted

The pre-runtime `firmware/main.c` (deleted at the runtime split, commit
6098a6c) read:

> the only thing this app needs to know is whether BOOT is down at the
> instant PWR's long press arrives, which is one read per event. A periodic
> poll was in here briefly and the app hung with a white screen and no
> input, which is exactly what a corrupted instruction fetch looks like.

The hazard was hit, diagnosed correctly, fixed by not polling, and written
down - in the one place guaranteed not to survive the file's deletion. The
runtime refactor reintroduced the periodic poll (apps now genuinely consume
BOOT clicks) and the day recorded above followed. The lesson is not "do not
refactor": it is that a constraint which exists to prevent a
hardware-level failure must live where the hardware lives (a decision
record, or the code that creates the hazard), not in a comment of one
caller that happened to respect it. This paragraph is that relocation.

## The fix: no code executes from flash, on either core, ever

`pico_set_binary_type(main copy_to_ram)` in `firmware/CMakeLists.txt`. The
whole image is copied to SRAM at boot and XIP is never executed afterward,
so the chip-select borrow is safe by construction, for both cores, and
stays safe against future refactors.

Per-function `__not_in_flash_func` annotation was considered and rejected:
auditing core1's real call graph found `time_us_64()`, `sleep_ms()`, libm's
`sqrtf`, the multicore launch trampoline, the vendor `FT3168_Reset()` and
the exception-install path all in flash, some reachable again at every
core1 relaunch, and every future edit to core1's path could silently add
another - which is exactly how the constraint was lost the first time.
Wholesale removal costs 65KB of `.text` plus 4KB of `.rodata` against
roughly 355KB of free SRAM and a few milliseconds of boot copy, and needs
no ongoing discipline.

Verified from the build artifacts, not by inspection: `objdump -h` shows
`.text` VMA 0x20000110 (LMA in flash, copied at boot), and `nm` places
every symbol on core1's path (`core1_entry`, both i2c helpers,
`core1_trampoline`, `time_us_64`, `sleep_ms`, `FT3168_Reset`,
`exception_set_exclusive_handler`, the fault handlers) in SRAM; `sqrtf`
compiles to a single inline `vsqrt.f32`. The only code left at a flash
address is crt0's reset stub, the boot-time vector table (VTOR is
retargeted to `ram_vector_table` in SRAM by a pre-init that itself runs
from SRAM) and the default unhandled-ISR stubs, all reachable only at cold
reset or on an already-dead machine.

Three hardening changes ride along, each correct regardless of the root
cause: `i2c1_bus_recover()` now uses the bounded local i2c calls instead of
the SDK's unbounded ones (the old chain was: recovery clear hangs on a
still-wedged bus, 4s watchdog reboots the chip, boot-time init hangs on the
same bus before the watchdog is re-armed - a lit, frozen panel forever);
the recovery publishes the PMIC's latched 0x49 key bits into `g_keyEvent`
instead of destroying them (the AXP2101 latches the short-press verdict at
RELEASE, which is after core1 died in every capture, so recovery held the
only copy of the press the owner actually made and was throwing it away -
this alone is why "publish first" still left the stopwatch not starting);
and a `PMIC_WRITE_SELFTEST` gate lets core1 issue the exact fatal
transaction shape every 2 seconds with no human, closing the "every soak
measured the one condition under which the fault cannot occur" hole for
this specific fault.

Validation still owed on hardware, in order: the control build
(`main-control-xip-selftest.uf2`, old XIP layout, self-test armed) should
reproduce a core1 death on the bench with no press; the fix build
(`main-fix-ram-selftest.uf2`) should run the same hammering clean; then the
acceptance test that actually matters, ten real presses over minutes, each
starting and stopping the stopwatch. Ship only after the third, with
`PMIC_WRITE_SELFTEST` back at 0.
