# 0014: Six agents, one contract, and nothing that compared the copies

Date: 2026-08-15
Status: accepted; the gate's `contracts` phase (tools/gate/contracts.ts) is
built, self-tested, and was shown firing on the real collision below before
this document was finished

## The failure class

Decision 0010 audited thirteen bugs and found every instrument reading
upstream of its bug on a single machine's chain: source to binary to pixels
to panel to a child's eye. That work is done and this document does not redo
it. What 0010's chain does not contain is the thing that actually produced
this week's collisions: **several agents editing copies of one contract in
worktrees that cannot see each other.** Each worktree is self-consistent, so
every instrument in it reads green. The defect exists only in the MERGE, and
the merge happens downstream of every instrument this project had, except a
human reading reports.

The failure class this record names: **a contract that exists as N
hand-maintained copies with no comparator.** Not a wrong value in a copy - a
disagreement between copies, each locally defensible, invisible until the
board or the child finds the seam.

## The evidence, all from one night

- **The same ABI function shipped twice under two names.** Commit `103c35d`
  (the tilt work, decision 0012) added `emu_sensor_vector()`, feeding the
  runtime's one orientation signal. Branch `c00db2f` (the bubble level),
  written the same night by an agent who could not see that work, added
  `emu_sensor_vec3()` for the same physical sensor, in different units (mg
  against g), quantised where the other is not, feeding a private latch
  instead of the published signal. Both edit `emu_abi.h`; a textual merge
  keeps both, compiles cleanly, and leaves two orientation inputs of which
  apps use one each. No test on either branch can fail on this, because each
  branch is internally consistent.
- **An app grew a private seam the same week the pattern was paid off.**
  `c00db2f`'s `emu_shim.c` includes `firmware/apps/level.h` so the emulator
  can feed the level's own accessor. Decisions 0008 and 0012 each cost a
  rework to establish that seams live in the runtime (`sensors.h`,
  `tilt.h`), never per app. The prose rule did not reach the agent; nothing
  executable said no.
- **The app table exists as three hand-maintained copies** -
  `runtime_core.c`'s `g_apps[]`, `emulator/wasm/build.ts`'s `SOURCES`,
  `firmware/CMakeLists.txt`'s `target_sources` - **plus a fourth in the
  tests** (`const APP_DRAW = 1` and friends, whose alignment comments were
  already stale: three of them still said `g_apps[] = { chrono, sketch,
  timer }` after four landed). A copy dropped in a merge gives a wasm build
  that gates an app the board never runs, or a board build that fails to
  link hours after the gate said green. Commit `7a15a80` ("menu: unbreak
  main, which I broke by committing another agent's file") is what this
  looks like arriving.
- **The fifth app broke the menu while another agent was replacing the
  menu.** `c00db2f` measured it honestly (37 outside-push violations with
  the level in the table) and parked the app behind `APPS_INCLUDE_LEVEL`.
  Reasonable - and it created a new dark region: a `g_apps[]` entry behind a
  preprocessor define is invisible to the gate, which exercises only the
  apps compiled into the default `emu.wasm`, while believing and reporting
  that it covered "every app".
- **The tests themselves have a copy-drift problem with chance.** Five test
  files construct `TouchSim` without a seed, so they draw from
  `Math.random()` and are a different test every run.
  `repro-touch-dropout-stroke-start.ts` documents its own flake and prints
  "(no simulated stray actually fired this run)" when the dice land wrong.
  The gate's own driver seeds (tools/gate/touch.ts) precisely to avoid this;
  nothing required the tests to.

## What was built: the gate compares the copies

A new first-class phase in `tools/gate/run.ts`, "the contracts several
agents share" (`tools/gate/contracts.ts`), running before any app is driven.
Every rule names the copy that disagrees, carries its reason, and fails on
an input it cannot parse. All predicates are in `selftest.ts` (44 checks
now), and each was additionally shown firing on the real artifact: run
against branch `c00db2f`'s actual files, `shim-purity` fires on the
`level.h` include, `abi-parity` fires on `emu_sensor_vec3`, and
`app-hygiene` stays quiet on `level.c` itself, which touches no hardware.

| Rule | Compares | Would have caught |
|---|---|---|
| `app-wiring` | `g_apps[]` against build.ts SOURCES against CMakeLists, both directions, with argued exemptions (menu, stubapps.c) that fail when stale | a table entry or source dropped in a merge; an app the board runs that no test has seen |
| `abi-parity` | emu_abi.h declarations against build.ts EMU_EXPORTS against the module's real export table | `emu_sensor_vec3` landing beside `emu_sensor_vector`; a declared call the JS side finds undefined |
| `shim-purity` | emu_shim.c's includes and symbols against the app layer | the shim including `level.h`; any future app-private emulator seam |
| `app-hygiene` | every `apps/*.c` against the chip and hazard symbols (QMI8658, FT3168, PMIC, i2c, `flash_range_*`, watchdog, multicore, `tilt_submit_device_g`) | the next app reading the IMU directly; the first app to improvise flash storage (0010's "next 0005") |
| `test-app-index` | tests' `APP_*` constants against `emu_device()`'s own app list | an inserted app silently reordering what every test drives |
| `test-determinism` | `new TouchSim(...)` constructions against the seeding rule, with a no-new-entries baseline for the five existing offenders | the sixth unseeded flaky test |
| `app-count-ceiling` | declared app count against decision 0013's twelve | the thirteenth app shrinking every target under a fingertip with nothing visibly breaking |
| `arena-headroom` | each app's `enter()` allocation against 90% of `APP_ARENA_BYTES`, via a new oracle (`emu_arena_used()`, reading `rtcore_arena_used()`) | an app creeping toward the red overflow trap; measured now: draw 52,908 of 65,536 bytes (81%), everything else under 1% |

Gated `g_apps[]` entries are not a failure (stubapps and the level flag are
deliberate), but the gate now prints them loudly, known.ts-style: "NOT GATED
THIS RUN: absent from this module, so no rule here has seen it". The gate no
longer claims coverage it does not have.

Cost: the phase is file parsing plus one `WebAssembly.Module.exports()` read
of the already-compiled module. The gate ran in 2.2-2.5s before and runs in
2.2-2.5s after; the arena oracle adds one wasm call per app and one extra
device load for the menu's own footprint.

The arena capacity is deliberately NOT restated in the rules file: the gate
reads it from the firmware (`emu_arena_capacity()`), because a second copy
of `APP_ARENA_BYTES` in a TypeScript file would be this document's own
disease. Only the 90% fraction is the gate's opinion.

## What this phase does not do

- It cannot say which of two duplicate contracts is RIGHT. When the level
  branch merges, `abi-parity` and `shim-purity` will fail the merged tree
  loudly, which is the designed outcome: the merge stops being quiet. A
  human still decides that `emu_sensor_vector` plus `app_frame_t.tilt` is
  the survivor and ports `level.c` onto it. The checks turn a silent
  collision into a fifteen-minute review with the failure text as the
  worklist; they do not perform the review.
- `app-hygiene` is the source-text half of decision 0012's owed rule. The
  linked-image reachability half (no `QMI8658_*` or flash-write symbol
  reachable from app code, in tools/invariants) still needs an ARM build to
  run against and a mutation to be watched failing, and stays owed.
- It reads this worktree. Two agents' un-merged branches still collide at
  merge time; the difference is that the first gate run on the merged tree
  now fails, instead of the merged tree passing everything and the collision
  surviving to the board.

## Two adjacent findings, written down rather than half-fixed

**The fingerprint is blind to build flags.** `tools/gate/fingerprint.ts`
hashes sources only, so a board flashed from this tree with
`-DAPPS_INCLUDE_LEVEL=1` or `-DMENU_STUB_APPS=12` reports the SAME
fingerprint as the default build while running a different app set - the
exact "board differs from tree" confusion the fingerprint exists to end,
one level down. This is 0010's law again: the instrument reads at the
sources, and the defines live downstream of it. The fix is mechanical
(CMake passes its optional defines into the fingerprint script; devlink
reports them) but it cannot be watched failing with the board asleep, and
an instrument that has never been seen red is not an instrument (0012
deferred its own invariant for exactly this reason). Owed at next flash.

**Time is still the open instrument gap, and it is not closable from here.**
The gate's budgets count pixels and transfers because that is what an
emulator can honestly count; a tick that computes expensively but pushes
little (game physics, a flood fill) is invisible to every rule in this tree
and will starve the watchdog on the board. A wall-clock bound inside the
gate was considered and rejected: on this machine, which runs many agents
and has been measured with a run queue of 41, host time flaps, and a gate
that cries wolf gets re-run until it agrees. The honest instrument is
0010's on-board effect ritual (hashes 500ms apart, counters provoked
through their selftests, achieved tick rate in the profiler line), which
remains owed and remains a board-in-hand job.

## What stays human, and the cheapest procedure for each

- **Merge arbitration when the contract checks fire.** Read the failure
  list, pick the surviving contract, port the loser. The gate output is the
  worklist; budget fifteen minutes per collision.
- **The tilt axis mapping** (decision 0012): two minutes of
  `bun tools/dev.ts tilt` with the board awake, then delete the word
  HYPOTHESIS. Nothing in software can substitute.
- **Responsiveness and tick rate**: the board, always (decision 0003). Next
  flash, run the effect ritual and read the profiler line; one minute.
- **Whether the picture is any good** (decision 0009): no checker evaluates
  ruler-lines or icon quality, deliberately. Cheapest eye-path remains
  regenerating `preview/` PNGs and looking; 0010's contact-sheet tool (one
  PNG grid of every app's settled frame, regenerated by the gate) is still
  the right build and still unbuilt.
- **The arena fraction** if an app someday legitimately needs more than
  90%: that is a decision about the device's memory story, made in the
  rules file with a written reason, not a threshold to nudge.
