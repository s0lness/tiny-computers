# The gate

One command that runs every cross-cutting rule against every app, and
fails rather than warns.

```
bun run tools/gate/run.ts             # the gate. ~3 seconds.
bun run tools/gate/run.ts --measure   # what every app costs, no verdict
bun run tools/gate/selftest.ts        # prove the rules can fail at all
bun run tools/gate/fingerprint.ts     # what build this tree is
```

It sits beside [`tools/invariants`](../invariants/README.md) rather than
inside it because the two look at different things. The invariant checker
reads the linked ARM image and asks **what the code is**; this drives the
same firmware compiled to wasm and asks **what it does**. Same posture,
though, and the same three rules from decision 0006: every check carries
the reason it exists, a failure prints that reason and the offending
detail, and an input the tool cannot read is a failure, never a skip.

## Why it exists

Every rule here encodes a bug that shipped to the device and cost a round
trip through a human's thumb. Each was found in one app and was possible
in all of them. **The rules apply to every app by construction, not by an
app opting in**, because the next app is written by someone who has not
read the list.

There are no per-app tests here. `exercise.ts` names no app: it makes the
things a hand does to a puck (tap, swipe, hold, drag-pause-drag, click,
shake, put it down) and asks the firmware which apps exist. Adding an app
to `g_apps[]` puts it under every rule below with no other edit.

## The rules

| Rule | Catches | When it runs |
|---|---|---|
| `push-geometry` | a pushed window whose row length is not a multiple of 8, or that runs off the panel (decision 0001) | every tick, every app |
| `push-budget` | unbounded work per tick: the palette's near-full repaint per drained touch sample, which starved the watchdog | every tick, every app |
| `pushed-or-invisible` | a pixel that changes outside that tick's pushed rectangles - correct in the emulator, stale on the panel | every tick, every app |
| `bezel` | ink inside `PANEL_BEZEL_MARGIN_PX`, painted and never seen | every settled frame |
| `clock-driven` | an animation driven by the arrival of touch samples rather than by the clock | 3 hold positions per app |
| `settles-the-same` | residue: a settled picture that depends on how many intermediate frames were rendered | 3 hold positions per app |
| `clean-input` | a gesture test proven only against input this panel does not produce | over `emulator/wasm/tests/` |
| `arena-headroom` | an app's enter() creeping toward the 64KB arena's red overflow trap (app.h) | once per app, plus the menu |
| freshness | a module older than the sources it was built from | first, before anything else |

Decision 0014 added a phase that compares the COPIES of contracts parallel
agents maintain by hand, before any app is driven (`contracts.ts`):

| Rule | Catches |
|---|---|
| `app-wiring` | the app table's three copies (g_apps[], build.ts SOURCES, CMakeLists) disagreeing - a merge dropped one |
| `abi-parity` | emu_abi.h, build.ts EMU_EXPORTS and the module's export table disagreeing - the emu_sensor_vec3/emu_sensor_vector collision |
| `shim-purity` | emu_shim.c reaching into firmware/apps/ - a per-app emulator seam (the level branch's level.h) |
| `app-hygiene` | an app naming chip or hazard symbols (IMU, touch, PMIC, i2c, flash writes, watchdog, multicore) - apps read signals, never chips |
| `test-app-index` | a test's APP_* constant disagreeing with the firmware's own app list |
| `test-determinism` | a new test constructing TouchSim without a seed (the documented stroke-start flake, multiplying) |
| `app-count-ceiling` | a thirteenth app, which decision 0013's grid holds with every target under a fingertip |

The same phase prints, loudly and without failing, every `g_apps[]` entry
behind a preprocessor condition: those apps are absent from this module,
so **no rule in this gate has seen them**, and the gate says so instead of
claiming coverage it does not have.

The device's numbers - budgets, bezel width, probe positions - live in
`rules/rp2350-amoled-1.8.ts` and nothing else knows them, so a second
device is a second rules file rather than a fork.

## What each one costs

The whole gate is **about 3 seconds**, and that is the number that
matters: a gate nobody runs because it takes ten minutes is worse than no
gate.

| Phase | Cost |
|---|---|
| self-test (can the rules fail?) | <10ms, pure predicates |
| freshness | 31 `stat()` calls |
| contracts (decision 0014) | ~30 file reads plus one `WebAssembly.Module.exports()` on the already-compiled module; tens of ms |
| every app, every tick | ~5,500 ticks total, ~2s. Each tick is one full framebuffer compare (82k word compares) plus a push-mask fill |
| the two counterfactuals | 4 apps x 3 positions x 4 runs, ~0.7s. Only two framebuffer compares each, so they are cheap despite the run count |
| clean-input lint | 16 file reads |
| fingerprint | 30 file hashes |

The per-tick phase is where all the time is, and it is bounded by the
stimulus list rather than by the apps. Doubling the stimuli doubles the
run.

## The two counterfactuals, and why they are shaped that way

`clock-driven` and `settles-the-same` are the only rules that cannot be
evaluated by watching one run, because both ask about a picture that
should have been drawn and was not.

- **`clock-driven`** runs the same prefix twice and then either keeps
  repeating the same touch report or stops sending reports entirely. A
  real controller repeats a coordinate about sixty times before producing
  a new one, and drops contact 34 times a second, so an app whose picture
  depends on their arrival works on a bench and freezes in a hand. The two
  runs must end at the same framebuffer.
- **`settles-the-same`** runs the settle window at 60Hz and then in three
  ticks. A clock-driven app computes the same final picture either way; an
  app that leaves residue behind its intermediate frames does not, and the
  difference IS the residue. This is the general form of "after an
  animation completes, the framebuffer must equal what the settled state
  alone would produce" - the settled state alone is not something a
  general checker can construct, and a coarse settle is.

Both probes deliberately use **clean input**, which is the exception the
`clean-input` rule asks to be argued for: each isolates one variable (the
arrival of redundant reports, the number of intermediate ticks), and
dropouts or jitter would change the reports' content and make a difference
between the two runs prove nothing.

## Two baselines, and the rule that keeps them honest

`cleaninput.ts` and `known.ts` each carry a list of exceptions with a
written reason. Both have the same anti-rot rule: **an entry that no
longer applies fails the run.** A stale exemption is how a checker quietly
stops covering something.

`known.ts` in particular is not a way to make a violation go away. Every
entry is printed, loudly, on every run, and names who has to decide it.

## What this cannot catch, and must never look like it can

The gate prints this at the end of every successful run, and it belongs
here too.

- **Time.** There is no watchdog here, no second core, no bus latency and
  no 12ms panel push. `push-budget` counts pixels and transfers because
  counting is what an emulator can honestly do; it is a proxy for a cost
  in microseconds and never a measurement of one. "Is it responsive" is a
  question for the board, permanently (decision 0003).
- **The bezel as a physical object.** `PANEL_BEZEL_MARGIN_PX` is a number
  read off a photograph taken at an angle. The rule enforces the number;
  only a person holding the device can say whether it is the right number.
- **Anything below the sensor seam.** The FT3168's registers, the two-ring
  timestamp merge, the `Touch_INT_PIN` gating, the PMIC key decoding are
  all reimplemented in `emu_shim.c` and are invisible here by construction.
  A register nobody writes is exactly the bug this cannot see
  (decision 0008).
- **The panel.** No burn-in, no brightness, no tearing.
- **A child's own ink.** The `bezel` rule is evaluated on settled frames
  produced by gestures the gate keeps 48px from every edge, so what it
  finds is the app's own layout. It cannot tell a badly placed button from
  a drawing that happens to reach the edge, and it does not try.
- **Whether the picture is any good.** Decision 0009 is a rule about hard
  edges and right angles that no checker in this directory evaluates.
