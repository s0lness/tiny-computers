# Requirements: what this emulator must be

This emulator exists because of one decision:
[0003](../../docs/decisions/0003-emulator-runs-the-real-apps.md) made it run the
firmware's own C, compiled to WebAssembly, not a reimplementation. Everything
below assumes that decision and asks what follows from it: what makes a tool
like this actually useful to someone iterating on firmware, day to day, with a
coding agent alongside them.

## The four requirements, as stated

The owner asked for exactly these, in this order of importance:

1. Very easy to launch.
2. Very fast to iterate on apps.
3. Easy screenshots, optional annotations, and a "freeze" that produces
   something an agent can act on.
4. Debugging capability.

Everything that follows is either in direct service of one of these four, or
a foundation that several of them stand on.

## What research into mature emulators actually says

Before ranking anything, a grounding pass across the tools people actually
rely on for embedded development and debugging: [Wokwi](https://docs.wokwi.com/)
(the closest peer: a browser-embedded simulator that real teams use for daily
Arduino/ESP32/Pico work), [Renode](https://renode.io/) (Antmicro's
deterministic, multi-node embedded simulator, built specifically for testing),
[QEMU's record/replay](https://www.qemu.org/docs/master/system/replay.html)
and [gdbstub](https://qemu-project.gitlab.io/qemu/system/gdb.html), the
tool-assisted-speedrun community's decades of practice on save states and
input movies, [Chrome DevTools' DWARF support for wasm](https://developer.chrome.com/docs/devtools/wasm),
and the CI practice of hashing framebuffers against golden images. A few
things came out of that pass that shaped the ranking below, and are worth
stating plainly because they cut against the assumption that "more fidelity
is always better":

- **Every one of these tools treats determinism as a prerequisite, not a
  feature.** Renode's own description of its "superpower" is representing
  the whole system in one deterministic simulation so it fits a normal CI
  workflow ([renode.io](https://renode.io/)). QEMU's record/replay only works
  at all because `-icount` makes execution deterministic in the first place
  ([QEMU docs](https://www.qemu.org/docs/master/system/replay.html)). TAS
  tooling (BizHawk, FCEUX) is built around the fact that "the same series of
  inputs, played back at different times, always gives the same results"
  ([Wikipedia, Tool-assisted speedrun](https://en.wikipedia.org/wiki/Tool-assisted_speedrun);
  [BizHawk](https://bizhawk.org/bizhawk-the-ultimate-emulator-for-retro-gaming/)).
  This project already has determinism for free: `emu_tick(nowMs)` takes the
  host's clock as its only time source
  ([`emu_abi.h`](../wasm/emu_abi.h)). That single ABI decision is what makes
  everything else in this document cheap.
- **DWARF debugging of wasm in Chrome is real, but reported as fragile in
  practice.** Multiple open issues describe the "C/C++ DevTools Support
  (DWARF)" extension breaking across Chrome releases or failing to load
  sources ([emscripten#13486](https://github.com/emscripten-core/emscripten/issues/13486),
  [emscripten#22525](https://github.com/emscripten-core/emscripten/issues/22525)).
  It should be offered, not relied on as the primary debugging path.
- **Even the tools with the best claim to fidelity are explicit that timing
  is not one of the things they guarantee.** Wokwi's own AVR core is
  cycle-accurate, but real discrepancies between board and simulation show
  up around interrupt timing and real-time peripherals like the RTC (open as
  [wokwi-features#931](https://github.com/wokwi/wokwi-features/issues/931),
  and reported directly by users on the
  [Arduino forum](https://forum.arduino.cc/t/physical-uno-and-wokwi-give-different-results-for-a-simple-test/1328159)).
  This lines up exactly with what decision 0003 already says about this
  project: timing is a question for the board, always.
- **CI-grade regression testing for emulators is done with framebuffer
  hashing against golden references**, not visual inspection: a per-frame
  hash written during the run, diffed against a checked-in golden hash, fails
  the build on divergence. This is documented practice in emulator CI (for
  example a GBA emulator's `--hash-out`/`--screenshot-out` pattern feeding a
  CI diff step) and is exactly the same idea as Flutter's golden-image tests.
- **Fault injection is an established, separate discipline from normal
  emulation**, distinct enough that QEMU has dedicated fault-injection
  extensions and there is a body of academic tooling built on top of Unicorn
  and QEMU for it (surveyed in
  [arxiv.org/abs/2404.10509](https://arxiv.org/pdf/2404.10509); QEMU-based
  fault injection specifically in
  [dl.acm.org/doi/10.1007/s10836-015-5555-z](https://dl.acm.org/doi/10.1007/s10836-015-5555-z)).
  The pattern that recurs is: inject a *misbehaviour* (a dropped byte, a
  stuck register, a bit flip), not a different ideal input. That is precisely
  what this project's own touch controller already needed
  ([decision 0003](../../docs/decisions/0003-emulator-runs-the-real-apps.md)).

## Ranked capabilities

Ranked by value against effort, in the order this project should actually
build them. "Effort" accounts for what the ABI already gives us for free
(`emu_abi.h` already exports the push-window log and takes an explicit
`nowMs`), not effort in the abstract.

| # | Capability | Value | Effort | Why |
|---|---|---|---|---|
| 0 | Determinism (host tick is the only clock) | Foundation | Already paid for | Every other row on this table is cheap or expensive *because* of this row. Not optional, not rankable against the others. |
| 1 | Peripheral push-window overlay | Very high | Near zero | `emu_abi.h` already exports `emu_push_*`; drawing them is the single highest-value line item because it already cost this project days of bisection once ([decision 0001](../../docs/decisions/0001-push-min-width.md)). |
| 2 | Structured, filterable guest logging | High | Low | `env.js_log` already exists as an import; a console pane with level/source filters is the same idea every one of these tools ships (Wokwi's serial monitor, Renode's [logger](https://renode.readthedocs.io/en/latest/basic/logger.html)). |
| 3 | Freeze bundle (screenshot + JSON) | Very high | Low-medium | Directly requirement 3; the emulator already renders to a canvas, so a PNG capture is nearly free. See [agent-loop.md](agent-loop.md) for the full spec. |
| 4 | Headless mode, framebuffer hashing, golden-image diffs | High | Medium | Turns "does this still look right" into a CI gate instead of a person looking at a page, the way GBA-emulator and Flutter golden-image CI both do it. |
| 5 | Record and replay of input traces | Very high | Medium | This is what determinism buys: an input trace becomes a file that reproduces a bug exactly, every time, which is the entire premise TAS tooling is built on. It is also, concretely, the thing decision 0003 exists because the *previous* emulator could not do. |
| 6 | Pause / single-step by frame | High | Low | `emu_tick` is already called from JS in a loop; stepping is "call it once, then stop," not a new mechanism. |
| 7 | Fault injection for peripherals (touch dropouts/strays) | High | Medium | The old TypeScript emulator's `touchsim` already modeled this and is explicitly the part of it worth keeping (decision 0003). Ports directly to sit between the browser's pointer events and `emu_touch`. |
| 8 | Save states / snapshot-restore | Medium-high | Medium-high | Genuinely useful for the debugging loop, but its value is compounded by row 5 existing first: a snapshot with no trace of how you got there is just a memory dump. |
| 9 | Rewind via a snapshot ring | Medium | High | See the dedicated section below: this is very likely not worth building as a standalone mechanism given row 5 already provides rewind by replaying the trace to frame N-1. |
| 10 | Source-level DWARF debugging in Chrome DevTools | High when it works | Medium-high, fragile | Directly requirement 4, and the whole reason decision 0003 is valuable when it works (see the dedicated section). Ranked below the rows above because its own upstream reports call it unreliable across Chrome versions. |
| 11 | Hot reload that preserves app state | Medium | High if done "properly," low if done honestly | See the dedicated section: the honest version reuses row 5 (rebuild, restart, replay the trace) rather than trying to preserve live wasm memory across a rebuild. |

A cross-cutting note that changes several of these effort estimates at once:
**rows 1, 2, 5, and the peripheral access log below are one mechanism, not
four.** All of them are views over the same underlying thing: a log of
`(nowMs, event)` pairs (`emu_touch`, `emu_button`, `emu_sensor_event`) plus
the push-window list each tick produces. Build that log once and the push
overlay, the record/replay trace, the peripheral access log, and the freeze
bundle's `pushWindows`/`inputTrace` fields are all just different renderings
of it. This is the single most leveraged thing to build first.

## Determinism as the foundation

The guest's only clock is the host's tick (`emu_tick(uint32_t nowMs)`,
[`emu_abi.h`](../wasm/emu_abi.h)). A firmware that reads its own clock has
broken the contract and will not be reproducible; the ABI header says so in
as many words. This buys exactly one thing, but it is the thing everything
else in this document depends on: **an input trace makes a bug a file**. Not
a description of a bug, not steps to reproduce, an actual sequence of
`(nowMs, event)` pairs that, replayed against the same wasm module, produces
bit-identical framebuffer output every time. This is not a novel idea, it is
the load-bearing assumption of every deterministic-replay tool that exists
(Renode's [reverse execution](https://renode.io/news/initial-support-for-reverse-execution-in-renode/),
QEMU's [record/replay](https://www.qemu.org/docs/master/system/replay.html),
every TAS movie file). What is specific to this project is that determinism
was already a design requirement of the ABI before this document existed,
which means this row costs nothing further to adopt; it only needs to be
protected (nothing should call `Date.now()` or `Math.random()` from inside
the tick path) and exposed (the trace needs to actually be recorded).

## Record and replay of input traces

Given the event log described above, recording is just "don't throw the log
away" and replay is "feed the log back through `emu_touch`/`emu_button`/
`emu_tick` instead of listening to the pointer." The output is a small JSON
or binary file that is a complete, exact reproduction of a session. This is
the single highest-leverage capability on the list because it is what turns
"the app stops responding to input after switching from the menu" (the real
bug that motivated decision 0003) from a description into an artifact: a
trace file that a bisection can replay against successive commits without a
human re-performing the gesture that triggered it.

## Pause, single-step by frame, and rewind via a snapshot ring

Pause and single-step are nearly free: the emulator already drives
`emu_tick` from a JS loop, so pausing is "stop calling it" and stepping is
"call it exactly once." Rewind is the interesting case, and the research
here argues against building it the obvious way.

TAS tooling rewinds by loading a save state and re-simulating forward
([BizHawk](https://bizhawk.org/bizhawk-the-ultimate-emulator-for-retro-gaming/)
describes exactly this for its re-recording workflow). Renode's reverse
execution takes **periodic snapshots and re-runs from the nearest one
backwards while searching for a breakpoint**, rather than storing every
frame ([renode.io](https://renode.io/news/initial-support-for-reverse-execution-in-renode/)).
QEMU's reverse-step does the same thing: it "loads the nearest snapshot and
replays the execution until the required instruction is met"
([QEMU replay docs](https://www.qemu.org/docs/master/system/replay.html)).
None of the three mature tools researched here stores a dense ring of full
snapshots, one per frame; they all store sparse snapshots and replay the gap
deterministically, because replaying is cheap when the target is
deterministic and memory is not free.

Given this project already has the record/replay trace (row 5) as a cheaper
prerequisite, the recommendation is: **implement rewind as "restart from
`emu_init`, replay the trace up to frame N-1" first**, and only invest in an
actual ring of periodic wasm-memory snapshots if replay-from-zero turns out
to be too slow in practice for how this tool is actually used (a 330KB
framebuffer and a few hundred milliseconds of touch events per session make
this unlikely to ever be the bottleneck; state this as the condition under
which row 9 should be revisited, not a permanent no).

## Save states / snapshot and restore

Distinct from the ring above: a single, on-demand snapshot of the wasm
module's linear memory plus whatever small amount of JS-side engine state
sits alongside it (current app index, panel rotation), so a developer can
capture "the state right before this reproduces" and hand it around, or jump
back to it after trying something. This is the same "save state, try
something, reload" loop TAS and speedrun tooling built its whole re-recording
workflow around
([TASVideos](https://tasvideos.org/EmulatorResources/Doom);
[fceux.com](https://fceux.com/web/help/ToolAssistedSpeedruns.html)). Its
value compounds with the trace from row 5, since a snapshot with no record of
the input sequence that produced it is a memory dump with no story.

## Source-level debugging of the C through DWARF

This is the concrete build requirement, verified against Chrome's own
documentation and the compiler this project already uses:

1. **Compile with debug info and don't strip it.** `zig cc` uses LLVM's
   backend, which [supports full DWARF debug-info generation](https://deepwiki.com/ziglang/zig/2.6.1-llvm-backend)
   (this specific claim is sourced from a community-maintained wiki, not
   Zig's own docs, and should be verified against a real build before being
   relied on). The general rule that applies regardless of which
   Clang-family compiler is used: passing `-g` embeds DWARF as custom
   sections in the `.wasm` binary
   ([Chrome for Developers, Debug C/C++ WebAssembly](https://developer.chrome.com/docs/devtools/wasm)).
   Do not run a stripping pass (`wasm-opt -strip-debug`, `strip`) on the
   build variant meant for debugging.
2. **Install the "C/C++ DevTools Support (DWARF)" Chrome extension.** This is
   what actually reads those custom sections and maps wasm offsets back to
   `firmware/*.c` line numbers, breakpoints, and locals
   ([chrome-stats listing](https://chrome-stats.com/d/pdcpmagijalfljmkmjngeonclgbbannb);
   [Chrome for Developers docs](https://developer.chrome.com/docs/devtools/wasm)).
   Chrome 114+ has some support without the flag that used to be required,
   but the extension remains the documented path.
3. **Serve the `.wasm` and the original `.c` sources from a stable local
   path** so DevTools' Sources panel can actually fetch the files it needs to
   show; the dev server this tool already runs locally satisfies this by
   construction.
4. **Treat this as best-effort, not load-bearing.** Multiple open issues
   against Emscripten's own DWARF pipeline describe the extension breaking
   across Chrome releases or failing to load sources at all
   ([emscripten#13486](https://github.com/emscripten-core/emscripten/issues/13486),
   [emscripten#22525](https://github.com/emscripten-core/emscripten/issues/22525)).
   The peripheral access log, guest logging, and pause/step (rows 1, 2, 6)
   should be built and trustworthy independent of whether DWARF debugging
   works on any given Chrome version, because they are the debugging path
   that cannot silently regress out from under this project on a browser
   update.

## A peripheral access log, and making push windows visible

The push-window overlay (row 1) is the specific, already-justified case: the
firmware's push path records every window it sent to the panel, after
whatever rounding its own code applies, and the emulator blits and outlines
only those windows. This is not a generic feature request; it is a direct
response to a defect that cost this project days
([decision 0001](../../docs/decisions/0001-push-min-width.md)) precisely
because the pushed windows were invisible. The general form of this
(Renode's [peripheral access log](https://renode.readthedocs.io/en/latest/execution-tracing/execution-tracing.html),
which records address, register name, and value for every access) does not
have a direct analogue here, because the ABI deliberately abstracts away
registers; the closest equivalent this project has is the same
`(nowMs, event)` log described above, filtered to the calls that cross the
ABI boundary (every `emu_touch`, `emu_button`, `emu_sensor_event`,
`emu_app_switch`, and the push list each tick emits). That filtered view is
what a "peripheral access log" means for a device whose peripherals are
already an abstraction, not raw registers.

## Fault injection

Real peripherals misbehave, and firmware carries code that exists only to
survive that misbehaviour. This project's own touch controller is the
example already on record: the real FT3168 drops contact mid-stroke and
emits stray reports at the wrong position, which is the entire reason the
sketchpad app has dropout bridging, glitch rejection, and stroke-start
confirmation
([decision 0003](../../docs/decisions/0003-emulator-runs-the-real-apps.md)).
A clean mouse drag exercises none of that code. This is not a project-specific
observation; it is a recognized, separate discipline in embedded testing,
with academic tooling built specifically to inject bit flips and instruction
skips into emulated peripherals rather than relying on ideal simulated inputs
([survey at arxiv.org/abs/2404.10509](https://arxiv.org/pdf/2404.10509);
QEMU-based fault injection in
[dl.acm.org/doi/10.1007/s10836-015-5555-z](https://dl.acm.org/doi/10.1007/s10836-015-5555-z)).
The previous TypeScript emulator's `touchsim.ts` already modeled this
statistically (a per-report Bernoulli event for dropouts and strays) and is
explicitly the part of that codebase worth keeping. It should be ported to
sit between the browser's pointer events and `emu_touch`, **off by default
and clearly labelled when on**, exactly as `emu_abi.h` already specifies.

## Headless mode, framebuffer hashing, golden-image diffs

CI-grade regression testing for an emulator is done by hashing the
framebuffer each frame and diffing against a checked-in golden hash, not by
a person looking at a screenshot. This is documented, working practice (a
GBA emulator's `--hash-out`/`--screenshot-out` pair feeding a CI diff step is
one concrete example) and is the same underlying idea as Flutter's
golden-image widget tests. For this project specifically: `emu_fb()` already
returns the framebuffer as a wasm memory offset, so a headless runner (the
same `puppeteer-core` + installed-Chrome pattern already used by
[`scripts/verify.ts`](../scripts/verify.ts), and by the separate `markup`
tool's own `scripts/shot.ts`) can drive a replay
trace (row 5) with no display attached, hash the resulting framebuffer per
frame, and fail a CI run the moment a change silently alters what an app
renders for a fixed input sequence. This is the automated backstop for
exactly the class of drift decision 0003 exists to prevent.

## Structured, filterable guest logging

`env.js_log(ptr, len)` is already the module's printf and is already
specified in `emu_abi.h`. The only work here is presentation: a console pane
that can filter by level or source the way Wokwi's serial monitor and
Renode's [logger](https://renode.readthedocs.io/en/latest/basic/logger.html)
both do, so a firmware author can silence noise from one app while watching
another. Low effort, and it is the debugging surface every one of the mature
tools researched here treats as table stakes, not a differentiator.

## Hot reload that preserves app state, honestly

The tempting version of this feature serializes the wasm module's live
memory, rebuilds, and pokes the old state back into the new module. Game
developers who have actually shipped hot-reloadable native code warn against
exactly this: when a struct's layout changes between reloads, the new code
reads old memory with the wrong shape, and the result is silent corruption
or a crash with no useful signal
([Karl Zylinski, "Hot Reload Gameplay Code"](https://zylinski.se/posts/hot-reload-gameplay-code/)).
The mitigation that article settles on is a size check that falls back to a
full reset when the shape has changed, and the explicit conclusion that
"doing something complicated to all your state probably has negligible
returns."

This project already has a cheaper, honest alternative: **row 5**. A rebuild
plus `emu_init()` plus replaying the recorded input trace produces
equivalent state through the same code path the firmware would take on a
real boot, with no memory-layout assumptions and no silent corruption mode,
because it is not preserving memory across an ABI change at all, it is
re-deriving the same state the normal way. The cost is the replay time
(bounded by session length, not by anything expensive) instead of the risk
of a change to a struct silently producing garbage. This is what "doing it
honestly" means here: the illusion of instant state preservation is not
worth the failure mode it buys, and the trace-replay version is not
meaningfully slower for sessions of the length this tool is actually used
for.

## What this emulator does not model, and must say so

Stated once, plainly, because everything above depends on nobody forgetting
it: **timing is not modeled.** The browser's clock drives the tick; nothing
here reproduces bus latency, panel push cost, or a second core. This is not
a gap to be closed later, it is a category this tool does not claim.
Wokwi's own AVR core is cycle-accurate and *still* has documented,
user-reported timing divergences from real boards around interrupts and
real-time peripherals
([wokwi-features#931](https://github.com/wokwi/wokwi-features/issues/931);
[Arduino forum thread](https://forum.arduino.cc/t/physical-uno-and-wokwi-give-different-results-for-a-simple-test/1328159)),
which is the strongest evidence available that timing fidelity is not
something a browser-hosted tool should promise even when it tries hard to.
The general framing that applies here: an emulator and the real device "are
different failure modes," not degrees of the same one
([bug0.com](https://bug0.com/blog/what-is-an-emulator)); a fast, faithful
functional tool is not a substitute for the one question only the board can
answer.

## What a good emulator refuses to do

- **It never implies timing fidelity it does not have.** No frame-rate
  counter, no "cycles per tick," no UI element that looks like a
  performance measurement, because every one would be read as a claim about
  the board this tool cannot back. The one honest statement is the one this
  project already ships: "any question about responsiveness is a question
  for the hardware. Always."
  ([decision 0003](../../docs/decisions/0003-emulator-runs-the-real-apps.md)).
- **It never silently diverges from the real target.** This is the entire
  reason decision 0003 exists: a reimplementation "agrees exactly once, at
  the moment the second is written, and drifts from then on with no test
  that can notice." The same discipline applies to every feature in this
  document: an injected fault must be visibly labelled when active (per
  `emu_abi.h`'s own instruction), a build flag that diverges the emulator's
  compile from the firmware's real build must be loud about it, and nothing
  here should ever produce output that looks identical to the real device's
  when it is not.
