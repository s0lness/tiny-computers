# 0007: A panicking core1 is an accepted exception, not a fixed hazard

Date: 2026-08-14
Status: decided, unvalidated on hardware (see "What would change this")

## The finding

Building the invariant checker planned in decision 0006 and running its rule
2 ("nothing on the core1 path takes the stdio lock", `sensors.h`'s ownership
rule) against the current firmware fails it for real, on the very first run:

`hard_assertion_failure` calls `panic`, `panic` calls `__wrap_puts`, and all
three are reachable by direct branch from `core1_entry` - specifically via
the SDK's sleep/alarm machinery (`sleep_ms` inside `touch_recover_core1()`
walks into `best_effort_wfe_or_timeout` and its assert calls). `panic()`
itself calls `stdout_serialize_begin()` first (`stdio_put_string`'s own
mutex acquire, `pico-sdk`'s `print_mutex`), so a core1 panic takes the same
stdio lock core0 also takes.

Decision 0006 named exactly two acceptable resolutions and forbade a third
(silently allowlisting the finding to make the build green): a
core1-specific panic hook, or an annotated exception recording "a panicking
core1 is already lost". This document is that explicit decision, made in
favour of the second.

## The decision: annotated exception, not a panic hook

**Reasoning:**

1. **A panicking core1 is not made safe by changing what it does at the
   moment it panics - it is already lost.** By the time `panic()` runs on
   core1, that core has already hit an assertion it cannot recover from. The
   thing worth protecting is core0, and the mechanism that protects core0
   from a dead core1 already exists and does not depend on core1's panic
   path cooperating: `runtime.c`'s core1-liveness guard notices frozen
   counters within about a second and calls `sensors_restart_core1()`
   (`sensors.h`, PERMANENT, decision 0005), which resets core1's CPU state
   unconditionally - "works regardless of what core1 was doing - a wedged
   i2c1 wait or a genuine LOCKUP, this does not depend on core1's
   cooperation". A panicking core1 is just one more way core1 can stop
   advancing; the guard was already built to not care why.

2. **The one real gap is the stdio mutex specifically, and a custom panic
   hook does not obviously close it for free.** If core1's panic takes
   `print_mutex` and never releases it (panic is `noreturn`; the SDK's
   default implementation spins after printing), a future core0 `printf`
   would block on that mutex forever, turning a dead core1 into a wedged
   core0 - the exact compounding failure decision 0004 spent a day
   chasing. A `PICO_PANIC_FUNCTION` override for core1 would need to avoid
   `print_mutex` entirely (so: no `printf`, no `stdio_put_string`, nothing
   that walks back into the very code path rule 2 is checking), which
   means it cannot reuse the SDK's own panic formatting and has to be
   written and verified from scratch.

3. **That verification cannot happen right now.** The device is powered off
   for this work; per the task this checker was built under, the hardware is
   owned by another agent. Shipping a new core1-only panic path - code that
   only ever runs at the exact moment core1 is already faulting, the
   hardest state to reproduce in this project's own experience (decision
   0004's whole day was spent on exactly this class of hard-to-reproduce
   fault) - with zero runtime validation is the failure mode decision 0004
   is about: "a broken instrument that reads 'fine' is worse than no
   instrument." An unverified panic hook that we then mark as "fixed" would
   be exactly that.

4. **The honest cost of the alternative is smaller than it looks.** The
   exception this document records is narrow: it names three specific
   functions (`__wrap_puts`, `weak_raw_vprintf`, `stdio_put_string`), and
   today they are reachable from core1 *only* through this one path
   (`hard_assertion_failure -> panic`; nothing in `sensors.c`'s normal loop
   calls any of the three, which is exactly what rule 2 is protecting and
   what makes the exception legible instead of a rubber stamp). If a future
   change adds a second, unrelated route to any of the three, rule 2 fails
   again on that new route and this document does not cover it - the
   exception is scoped to the panic path, not to the three function names
   in general.

## What is accepted, in writing

**A core1 panic still takes `print_mutex` and does not release it.** If
core0 calls `printf`/`puts` afterward, core0 blocks on that mutex too. This
is not fixed by this decision. It is accepted because:

- it requires core1 to have already hit an unrecoverable assertion (rare by
  construction - decision 0004 found exactly one, on a bus stall, in this
  project's history so far),
- core0's own liveness guard already exists to detect and recover a dead
  core1 (`sensors_restart_core1()`), independent of *why* core1 died, and
- `sensors_restart_core1()`'s reset of core1's CPU state does not currently
  release `print_mutex` if core1 died holding it - **this is the residual
  gap this decision leaves open**, not a gap it closes. A core0 `printf`
  issued after a core1 panic can still wedge, until `sensors_restart_core1()`
  (or the mutex itself) is taught to recover a stdio lock left held by a
  core it just reset. That is future work, not silently swallowed: it is
  written here so the next person auditing this exception starts from an
  accurate list of what is and is not actually handled.

## What would change this

- If `sensors_restart_core1()` is ever extended to force-release
  `print_mutex` (mirroring what it already does for the i2c1 bus,
  decision 0005), the residual gap above closes and this document should be
  updated to say so.
- If a second, non-panic path ever reaches `__wrap_puts`, `weak_raw_vprintf`,
  or `stdio_put_string` from core1, rule 2 fails on it and that failure is
  real: this document's reasoning applies only to the panic route, and the
  new route needs its own decision, not a widened version of this one.
- If the device is ever brought back up and someone deliberately reproduces
  a core1 panic on hardware (mirroring `PMIC_WRITE_SELFTEST`'s approach to
  the decision-0004 fault) and it behaves differently than reasoned here,
  that measurement overrides this document.

## Where this is enforced

`tools/invariants/rules/rp2350-amoled-1.8.ts`, rule 2's exception list, cites
this file by path. The runner checks the path resolves; it does not, and
cannot, check that the reasoning still holds - that is this document's job,
not the tool's.
