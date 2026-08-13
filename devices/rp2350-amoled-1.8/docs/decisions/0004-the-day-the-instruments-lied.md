# 0004: The day the instruments lied

Date: 2026-08-13
Status: diagnosis established, fix in progress

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
