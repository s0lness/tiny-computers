# App fuzzing: chrono, timer, sketch, menu

Date: 2026-08-14
Method: the real firmware compiled to wasm, driven through `emu_tick()` with a
synthetic clock. Four directed reproduction scripts plus roughly 116,000 fuzzed
ticks across the three apps, from a seeded PRNG so every run reproduces.

The clock was lied to deliberately: duplicate timestamps, `dt` of zero, and
one-hour forward jumps. Touch was aimed at `sketch.c`'s own thresholds by name
(`MIN_JUMP_ALLOW_PX`, `MAX_JUMP_PX`, `CONFIRM_PX`, `LIFT_DEBOUNCE_MS`) rather
than scattered randomly.

## 1. The timer silently eats a PWR short-press when BOOT is released with it

**FIXED, 2026-08-17, in commit `88cabe6` ("timer: stop a same-tick BOOT release
from swallowing a PWR short-press").** The repro below now PASSES: after the
combined tick the timer is running, and the two framebuffer hashes two seconds
apart differ, which is the assertion that the countdown actually advanced.

Left in place rather than deleted because a finding is worth more with its
resolution attached than as a paragraph that quietly disappears. What follows
is the original write-up, and everything in it was true when written.

Reproduction: `bun run emulator/wasm/tests/repro-timer-swallows-pwr-short-with-boot.ts`,
which failed when this was written and passes now.

`timer_tick()` opens with `if (f->bootClicked) { ...; return; }`, before it ever
looks at `f->key & KEY_SHORT`. `sensors_key_take()` in `runtime_core.c` is
read-and-clear, and it is called once per tick whether or not the app consumes
what it returns.

So when a BOOT release and a PWR short-press verdict land in the same tick, the
`KEY_SHORT` bit is gone for good. Not deferred, not queued: **gone**, with no
log line and no state change. Nothing that tick or any later tick can recover
it.

That collision is easy to reach. BOOT is sampled at about 20Hz, so releasing
both buttons together is enough. The emulator shows only
`timer: BOOT reset to 01:55`, and the framebuffer is provably static for the
next two real seconds, counting down nothing.

`chrono.c` meets the identical situation and handles it differently: no early
return, so both events apply in order. It resets to zero, and then, because
`running` is now false, the `KEY_SHORT` starts it. Verified side by side: after
the same combined press, the stopwatch is visibly running and the timer is not.

This is a defect rather than a decision. Nothing in `timer.c` mentions it, and
the sibling app in the same codebase does the opposite. The harm is muted,
which is likely why real use has not surfaced it: BOOT's own action already
stops the countdown, just by reverting to the set value instead of pausing.
A silent, unrecoverable dropped input is still worth fixing.

## 2. The push path held, and that is a real answer

Every `emu_push_*` rectangle was checked on every tick of the full run against
two invariants:

- inside the panel, positive width and height, row length a multiple of 8
  (decision 0001)
- **no framebuffer pixel changed outside that tick's union of pushed
  rectangles**

Zero violations, in any app.

The second one is the finding. A pixel that changes outside a pushed rectangle
never reaches the real panel, so it looks correct in the emulator and is broken
on hardware, which makes it exactly the class of bug this emulator exists to
catch and the one nobody had gone looking for. It held under duplicate
timestamps, hour-long clock jumps, and touch aimed at the sketchpad's own
glitch and dropout thresholds.

Reported as a substantive negative result, not as an absence of findings.

## 3. `app_frame_t.dtMs` is dead, so the 250ms clamp protects nothing

**NO LONGER TRUE, checked 2026-08-17.** The field has real readers now:
`dino.c` integrates its jump by it, `breakout.c` steps the ball by it (and its
header argues explicitly about how), and `bowling.c` reads it in flight. All
three arrived after this was written, with the apps the owner asked for. The
250ms clamp therefore protects something today, which is the opposite of what
this section concluded.

Kept for the same reason as finding 1: a document that silently drops a
paragraph teaches nobody anything, and "this was true and stopped being true"
is itself worth reading. The original write-up follows.

`runtime_core.c` writes it. No app reads it: not `chrono.c`, not `timer.c`, not
`sketch.c`, not `menu.c`. Both apps that track elapsed time compute it from
absolute `nowMs` deltas instead, unclamped.

The timer was fed a one-hour forward jump specifically to break this, and it
did not: the catch-up loop is bounded and capped at 3600 seconds, and it
reaches the alarm correctly.

So this is a vestigial field and a documentation mismatch rather than a live
bug. No regression test, because there is no wrong behaviour to pin.

## Checked and clean

- **Alarm dismissal parity.** Touch, PWR, a BOOT click and a shake all produce
  byte-identical framebuffers afterwards, and all four stop the sound.
- **BOOT recall across two full alarm cycles.** `00:25` came back identically
  both times, across different dismissal kinds.
- **The ring across 0 and 360.** A continuous drag through twelve o'clock lands
  on exactly the tick a direct touch there would, with no rendering residue.
  The large value jump this produces is inherent to a fixed-start dial with no
  true zero.
- **Arena zeroing and touch-resolver reset, for the sketchpad.** The existing
  tests already covered chrono and timer. Re-entry after dirtying a stroke and
  switching away is clean.

## A hypothesis that was wrong, recorded because it was tested

Escaping a ringing alarm through the BOOT+PWR menu chord looked certain to
leave the sound playing forever, since `timer.c` has no `leave()` to call
`sound_stop()`.

It does not. The PWR press edge that begins the chord already sets
`frame.key != 0` on an earlier tick, and `handle_alarm()`'s "any input" check
catches it before the chord can complete.

Worth writing down as a falsified hypothesis rather than folding into a list of
passes: it would have been written up as a bug on reasoning alone.

## Present, confirmed, and deliberate

The stopwatch wraps its minutes at 100 (`mm = totalCs/6000 % 100`). Confirmed
at `t = 6,000,000ms`: five of six digit cells reset at once while `running`
stays true, which is visually indistinguishable from a BOOT click. `chrono.c`'s
own comment accepts this ("not a case this puck's use needs to handle
cleanly"), so it is not reported as a defect.

## Verdict on the instrument

The emulator found a real bug and disproved a plausible one, and both required
running the compiled firmware rather than reasoning about the C. The push-path
fuzzing in particular is impractical by hand and genuinely exercises the real
`gfx_push` rounding and clamping.

The honest caveat is that the code turned out more solid than the hunt assumed.
After the three bug classes already fixed (arena zeroing, touch-resolver reset,
ring residue), everything held except the timer interaction above.
