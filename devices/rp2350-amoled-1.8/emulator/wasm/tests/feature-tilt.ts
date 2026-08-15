// feature-tilt: drives the real firmware to any orientation, headlessly and
// deterministically, and checks what an APP would have been handed. Run with:
//
//   bun run emulator/wasm/build.ts && bun run emulator/wasm/tests/feature-tilt.ts
//
// This is the oracle the orientation signal ships with (firmware/runtime/
// tilt.h, docs/decisions/0011): three apps are about to be written on top of
// gravity, the board is asleep and cannot absorb their iteration, and
// without this file the only way to see what an app receives is to flash.
//
// WHERE IT READS, AND WHY THERE. Every assertion below reads emu_tilt(),
// which returns what the CURRENT app was handed on the last tick, after the
// filter and after the runtime's landscape rotation - not the vector this
// test just sent. Asserting on what was sent would be an instrument reading
// upstream of every bug that can actually happen here (a swapped axis, an
// inverted sign, a rotation applied the wrong way, a filter that never
// converges), which is docs/decisions/0010's whole finding.
//
// WHAT IT STILL CANNOT SEE, said here rather than discovered later:
//   - whether the device-to-panel mapping matches the physical case. No
//     software oracle knows which way is up. tilt.h's on-board ritual is
//     the only thing that can settle it, and it needs a hand.
//   - whether the adaptive filter's constants FEEL right. The emulator's
//     gravity is perfectly still and exactly unit length; a real
//     accelerometer in a child's hand is neither (emu_abi.h's honesty
//     section).
//   - the staleness path (tilt.h's TILT_STALE_MS). emu_tick() submits a
//     sample every frame by construction, so "the IMU went quiet" cannot be
//     reproduced here at all; on the board it is what makes an app show a
//     question mark instead of a confident wrong picture.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

// Facts lifted from the firmware rather than re-derived: the app table's
// order (runtime_core.c), which of those apps is landscape (app_t.landscape,
// set in each app's own file), and app.h's four up-edge codes.
const APP_CHRONO = 0; // landscape
const APP_DRAW = 1; // portrait
const UP_TOP = 0;
const UP_RIGHT = 1;
const UP_BOTTOM = 2;
const UP_LEFT = 3;
const UP_NAME = ["TOP", "RIGHT", "BOTTOM", "LEFT"];

// How long a step is held before this file calls the filter "settled" -
// not one of tilt.h's own constants any more (the adaptive filter has
// several, not one time constant), just a duration comfortably past all of
// them for a step this hard (see section 3 below for what "this hard"
// means for an adaptive corner).
const STEP_MS = 150;

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string): void {
  if (ok) passCount++;
  else failCount++;
  console.log(`[${ok ? "PASS" : "FAIL"}] ${label}${detail ? " - " + detail : ""}`);
}
function near(a: number, b: number, tol: number): boolean {
  return Math.abs(a - b) <= tol;
}

interface Tilt {
  gx: number;
  gy: number;
  gz: number;
  deg: number;
  up: number;
  valid: boolean;
}

async function loadDevice() {
  const bytes = readFileSync(WASM_PATH);
  const mod = await WebAssembly.compile(bytes);

  let memory!: WebAssembly.Memory;
  const decoder = new TextDecoder();
  const imports = {
    env: {
      js_log(ptr: number, len: number) {
        console.log(`    [fw] ${decoder.decode(new Uint8Array(memory.buffer, ptr, len))}`);
      },
      sinf: Math.sin,
      cosf: Math.cos,
      atan2f: Math.atan2,
      sqrtf: Math.sqrt,
      fabsf: Math.abs,
      floorf: Math.floor,
      fmodf: (x: number, y: number) => x % y,
      powf: Math.pow,
      expf: Math.exp,
    },
  };

  const instance = await WebAssembly.instantiate(mod, imports);
  memory = instance.exports.memory as WebAssembly.Memory;
  const exp = instance.exports as any;
  if (typeof exp.emu_sensor_vector !== "function" || typeof exp.emu_tilt !== "function") {
    throw new Error(
      "this emu.wasm has no emu_sensor_vector/emu_tilt: it predates the orientation signal. " +
        "Rebuild it (bun run emulator/wasm/build.ts) - a test that ran against the previous " +
        "binary is exactly the stale-wasm trap docs/decisions/0010 lists."
    );
  }
  if (exp.emu_init() !== 1) throw new Error("emu_init() failed - see the [fw] lines above");

  return {
    // Sets the pose and holds it, exactly like a hand holding the puck
    // still: the value stands until it is set again (emu_abi.h).
    gravity(x: number, y: number, z: number) {
      exp.emu_sensor_vector(1, x, y, z); // index 1 = "gravity" in emu_device()'s sensors array
    },
    tick(nowMs: number) {
      exp.emu_tick(nowMs);
    },
    switchTo(index: number) {
      exp.emu_app_switch(index);
    },
    tilt(): Tilt {
      return {
        gx: exp.emu_tilt(0),
        gy: exp.emu_tilt(1),
        gz: exp.emu_tilt(2),
        deg: exp.emu_tilt(3),
        up: exp.emu_tilt(4) | 0,
        valid: exp.emu_tilt(5) === 1,
      };
    },
  };
}

const dev = await loadDevice();

// Every timestamp in this file comes from this one counter, never from
// Date.now(): emu_tick's nowMs is the module's only clock, so the same
// sequence of (call, timestamp) pairs is the same run every time.
let t = 1000;
function advance(ms: number, stepMs = 10): void {
  const end = t + ms;
  while (t < end) {
    t = Math.min(t + stepMs, end);
    dev.tick(t);
  }
}

// ---------------------------------------------------------------------------
// 1. Before anything has been sampled, the signal says so.
// ---------------------------------------------------------------------------
{
  const before = dev.tilt();
  check("no reading yet reads as invalid, so an app cannot draw a confident lie", !before.valid);
}

// ---------------------------------------------------------------------------
// 2. The default pose is flat on a table, and it is neutral: no in-plane
//    gravity for a ball to roll along, nothing for a bubble to chase.
// ---------------------------------------------------------------------------
advance(1000);
{
  const flat = dev.tilt();
  check("a sampled reading is valid", flat.valid);
  check(
    "flat on a table reads (0, 0, 1) and 0 degrees from flat",
    near(flat.gx, 0, 0.01) && near(flat.gy, 0, 0.01) && near(flat.gz, 1, 0.01) && near(flat.deg, 0, 1),
    `g=(${flat.gx.toFixed(3)}, ${flat.gy.toFixed(3)}, ${flat.gz.toFixed(3)}) deg=${flat.deg.toFixed(1)}`
  );
}

// ---------------------------------------------------------------------------
// 3. The filter. tilt.h's adaptive corner (one euro, ported here from the
//    bubble level's own original filter) is not a fixed one-pole any more,
//    so there is no single "one time constant is 63 percent" answer to
//    assert - the corner WIDENS as the derivative sees real motion, by
//    design (tilt.h's "FILTERING" section), so a step tracks faster than a
//    fixed pole ever would. What still has to hold, and does, is that the
//    answer does not depend on how many ticks the same elapsed time was cut
//    into: the board samples at a fixed 20ms on core1; the browser ticks at
//    whatever the tab manages; a game's frame costs anywhere from 27us to
//    12ms of push time. A filter stepped by a fixed per-sample constant
//    would settle at a different speed on each, which is the "speed must
//    not depend on push cost" trap decision 0010 names for the game loop,
//    hiding inside a sensor.
// ---------------------------------------------------------------------------
{
  dev.switchTo(APP_DRAW); // portrait: no rotation between panel and app space
  advance(600); // let the switch and the flat pose settle

  dev.gravity(0, 1, 0); // stand it upright, portrait, top edge up
  advance(16, 16); // one board-cadence tick
  const oneTick = dev.tilt();
  check(
    "a hard step already moves most of the way within one board-cadence tick",
    oneTick.gy > 0.5,
    `gy=${oneTick.gy.toFixed(3)} after ~16ms (a fixed 150ms one-pole would read about 0.10 here)`
  );

  dev.gravity(0, 0, 1);
  advance(2000); // back to flat, fully settled
  dev.gravity(0, 1, 0);
  advance(STEP_MS, 10); // 150ms, in 15 fine ticks
  const fineSteps = dev.tilt();
  check(
    `within ${STEP_MS}ms, fine ticks, the step is essentially settled`,
    near(fineSteps.gy, 1.0, 0.001),
    `gy=${fineSteps.gy.toFixed(5)}`
  );

  // Same physical 150ms, cut into 3 ticks instead of 15: a coarser sample
  // rate gives the derivative a cruder finite difference to work from, so
  // the two answers are not asserted BIT-identical - only close, and closer
  // than the gap between "barely moved" and "settled" by a wide margin.
  dev.gravity(0, 0, 1);
  advance(2000);
  dev.gravity(0, 1, 0);
  advance(STEP_MS, 50);
  const coarseSteps = dev.tilt();
  check(
    "the same 150ms cut into 3 ticks lands within 1% of the same 150ms cut into 15",
    near(coarseSteps.gy, fineSteps.gy, 0.01),
    `${coarseSteps.gy.toFixed(4)} vs ${fineSteps.gy.toFixed(4)}`
  );

  advance(2000);
  const settled = dev.tilt();
  check(
    "and it does converge: upright portrait reads (0, 1, 0), 90 degrees from flat",
    near(settled.gx, 0, 0.01) && near(settled.gy, 1, 0.01) && near(settled.deg, 90, 1),
    `g=(${settled.gx.toFixed(3)}, ${settled.gy.toFixed(3)}, ${settled.gz.toFixed(3)}) deg=${settled.deg.toFixed(1)}`
  );
  check(
    "upright portrait puts the app's TOP edge up",
    settled.up === UP_TOP,
    `up=${UP_NAME[settled.up]}`
  );
}

// ---------------------------------------------------------------------------
// 4. The landscape rotation. THE SAME PHYSICAL POSE, read by a landscape app
//    and by a portrait one, must come out in each app's own drawing space -
//    otherwise every landscape app re-derives the rotation by hand and the
//    first one to get it backwards ships a level that leans the wrong way.
//
//    Held upright portrait (gravity down the panel), a landscape app is
//    being looked at sideways: gravity crosses its canvas from left to
//    right, and its LEFT edge is the high one.
// ---------------------------------------------------------------------------
{
  dev.switchTo(APP_CHRONO); // landscape
  advance(600);
  const land = dev.tilt();
  check(
    "a landscape app reads the same pose in ITS OWN axes",
    near(land.gx, 1, 0.01) && near(land.gy, 0, 0.01),
    `g=(${land.gx.toFixed(3)}, ${land.gy.toFixed(3)}, ${land.gz.toFixed(3)})`
  );
  check(
    "and its up-edge is rotated to match",
    land.up === UP_LEFT,
    `up=${UP_NAME[land.up]} (portrait saw TOP for the same pose)`
  );
  check(
    "the angle from flat is the same in both spaces (a rotation about the screen normal cannot change it)",
    near(land.deg, 90, 1),
    `deg=${land.deg.toFixed(1)}`
  );
}

// ---------------------------------------------------------------------------
// 5. The up-edge hysteresis, which is the reason "which way is up" is
//    published rather than left to each app's own atan2.
// ---------------------------------------------------------------------------
{
  dev.switchTo(APP_DRAW);
  advance(600);

  dev.gravity(0, 1, 0); // top edge up, unambiguous
  advance(1000);
  check("a clear pose decides the up-edge", dev.tilt().up === UP_TOP);

  // Exactly on the diagonal: neither axis dominates, so the previous answer
  // must hold rather than flickering between two equally good ones.
  dev.gravity(0.7071, 0.7071, 0);
  advance(1000);
  check(
    "a pose exactly on the diagonal holds the previous answer instead of flickering",
    dev.tilt().up === UP_TOP,
    `up=${UP_NAME[dev.tilt().up]}`
  );

  // Past the dead band, it does change.
  dev.gravity(1, 0, 0);
  advance(1000);
  check(
    "past the dead band it changes: gravity toward the right edge means the LEFT edge is up",
    dev.tilt().up === UP_LEFT,
    `up=${UP_NAME[dev.tilt().up]}`
  );

  // Laid flat, the in-plane vector is meaningless. The answer must be HELD,
  // not cleared: an orientation-aware clock put down on a table should keep
  // the orientation it had.
  dev.gravity(0, 0, 1);
  advance(2000);
  const flat = dev.tilt();
  check(
    "laid flat, the up-edge holds rather than blanking",
    flat.up === UP_LEFT && near(flat.deg, 0, 1),
    `up=${UP_NAME[flat.up]} deg=${flat.deg.toFixed(1)}`
  );
}

// ---------------------------------------------------------------------------
// 6. Any orientation, including the ones a slider cannot reach.
// ---------------------------------------------------------------------------
{
  dev.gravity(0, 0, -1); // face down on the table
  advance(2000);
  const faceDown = dev.tilt();
  check(
    "face down reads 180 degrees from flat",
    near(faceDown.deg, 180, 1) && near(faceDown.gz, -1, 0.01),
    `deg=${faceDown.deg.toFixed(1)} gz=${faceDown.gz.toFixed(3)}`
  );

  dev.gravity(0, -1, 0); // upside down, portrait
  advance(2000);
  const upsideDown = dev.tilt();
  check(
    "held upside down, the BOTTOM edge is up",
    upsideDown.up === UP_BOTTOM,
    `up=${UP_NAME[upsideDown.up]}`
  );

  dev.gravity(-1, 0, 0);
  advance(2000);
  check("gravity toward the left edge means the RIGHT edge is up", dev.tilt().up === UP_RIGHT);
}

console.log(`\n${passCount} passed, ${failCount} failed`);
process.exit(failCount === 0 ? 0 : 1);
