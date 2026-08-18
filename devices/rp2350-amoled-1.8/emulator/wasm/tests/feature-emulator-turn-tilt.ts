// feature-emulator-turn-tilt: the emulator page's own TURN/TILT control
// (main.ts's #rotQuick + #tilt, puckpose.ts's gravityFromPose()), driven the
// exact way the page drives it, against the REAL firmware compiled to wasm.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/feature-emulator-turn-tilt.ts
//
// WHY THIS FILE, SEPARATELY FROM feature-tilt.ts: that file pins tilt.c
// itself, calling emu_sensor_vector() through a small helper that undoes
// device_to_panel() by hand at each call site. This file pins the actual
// TypeScript the browser runs - it imports gravityFromPose() from
// emulator/src/puckpose.ts, the same module main.ts's sendGravity() calls -
// so a regression in THAT function (the one place turning the on-screen
// puck becomes an ABI call) is caught here even if feature-tilt.ts's own
// hand-rolled conversion is never touched. Isolated on 2026-08-18: tilt.c's
// device_to_panel() was corrected from the identity to a real, measured
// mapping, and gravityFromPose() kept handing it panel-space vectors as if
// nothing had changed - every TURN button still moved the picture but the
// firmware's own `up` came out wrong (a 90-degree rotation of the intended
// edge), because the conversion this file now exercises did not exist yet.
//
// WHAT THIS FILE PROVES:
//   1. all four #rotQuick buttons (TOP/RIGHT/BOTTOM/LEFT, data-deg
//      0/90/180/-90) make the firmware's own `up` (emu_tilt(), read the way
//      an app receives it, never the vector just sent - decision 0010) agree
//      with the button pressed, at the page's actual default TILT (90, "on
//      edge" - decisive on purpose, see main.ts's own header comment on
//      quickDeg/tiltDeg for why 0 would not prove anything here).
//   2. the page's actual BOOT state (TURN=LEFT, TILT=90) reads `up`=LEFT
//      from the very first settled sample - not "eventually", which is the
//      "must agree from the first frame" requirement main.ts's own comment
//      on the 90 default argues for.
//   3. near flat (TILT dragged down to a few degrees), the firmware HOLDS
//      the previous up-edge rather than reporting a new or blank one - the
//      real hysteresis tilt.c's own header describes, not something this
//      page's control fakes.
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { gravityFromPose } from "../../src/puckpose";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

const UP_TOP = 0;
const UP_RIGHT = 1;
const UP_BOTTOM = 2;
const UP_LEFT = 3;
const UP_NAME = ["TOP", "RIGHT", "BOTTOM", "LEFT"];

// Checked against APP_DRAW (portrait, index 1 - runtime_core.c's app
// table), not whatever boots by default (chrono, landscape): emu_tilt()
// reports the CURRENT app's own rotated space (emu_abi.h's own field
// comment, proven by feature-tilt.ts section 4 - a landscape app's `up` is
// the panel edge rotated a further quarter turn). This file is pinning
// puckpose.ts's device-axis conversion against tilt.c's PANEL-space
// up-edge, the same thing the TURN button labels (TOP/RIGHT/BOTTOM/LEFT)
// name, so it has to read that through a portrait app, where the two agree.
const APP_DRAW = 1;

// data-deg values are the ABI (index.html's own comment): keep this table in
// the same order the bezel buttons are drawn in.
const TURN_BUTTONS: { name: string; deg: number; want: number }[] = [
  { name: "TOP", deg: 0, want: UP_TOP },
  { name: "RIGHT", deg: 90, want: UP_RIGHT },
  { name: "BOTTOM", deg: 180, want: UP_BOTTOM },
  { name: "LEFT", deg: -90, want: UP_LEFT },
];

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
        "Rebuild it (bun run emulator/wasm/build.ts)."
    );
  }
  if (exp.emu_init() !== 1) throw new Error("emu_init() failed - see the [fw] lines above");

  return {
    // The exact call main.ts's sendGravity() makes: gravityFromPose()'s
    // output handed straight to emu_sensor_vector(), no per-call-site
    // conversion here (that would defeat the point of this file).
    setPose(turnDeg: number, tiltDeg: number) {
      const [x, y, z] = gravityFromPose(turnDeg, tiltDeg);
      exp.emu_sensor_vector(1, x, y, z); // index 1 = "gravity" in emu_device()'s sensors array
    },
    tick(nowMs: number) {
      exp.emu_tick(nowMs);
    },
    switchTo(index: number) {
      exp.emu_app_switch(index);
    },
    tilt() {
      return {
        gx: exp.emu_tilt(0) as number,
        gy: exp.emu_tilt(1) as number,
        gz: exp.emu_tilt(2) as number,
        deg: exp.emu_tilt(3) as number,
        up: (exp.emu_tilt(4) as number) | 0,
        valid: exp.emu_tilt(5) === 1,
      };
    },
  };
}

const dev = await loadDevice();
dev.switchTo(APP_DRAW);

let t = 1000;
function advance(ms: number, stepMs = 10): void {
  const end = t + ms;
  while (t < end) {
    t = Math.min(t + stepMs, end);
    dev.tick(t);
  }
}

// ---------------------------------------------------------------------------
// 1. Every TURN button, at the page's own default TILT (90), reports the
//    matching up-edge - the arithmetic worked out from tilt.c's
//    device_to_panel() and up-edge decision, checked the way the task asked:
//    through emu_tilt(), not by trusting the derivation.
// ---------------------------------------------------------------------------
for (const { name, deg, want } of TURN_BUTTONS) {
  // Land it decisively from a neutral start each time, so one button's
  // result cannot be hysteresis left over from the previous one.
  dev.setPose(deg, 0);
  advance(600);
  dev.setPose(deg, 90);
  advance(600);
  const reading = dev.tilt();
  check(
    `TURN=${name} (data-deg=${deg}) reports up=${UP_NAME[want]}`,
    reading.up === want && reading.valid,
    `up=${UP_NAME[reading.up]} g=(${reading.gx.toFixed(3)}, ${reading.gy.toFixed(3)}, ${reading.gz.toFixed(3)}) deg=${reading.deg.toFixed(1)}`
  );
}

// ---------------------------------------------------------------------------
// 2. The page's actual boot state (main.ts: quickDeg=-90 "LEFT", tiltDeg=90)
//    must read up=LEFT from the very first settled sample, not eventually -
//    the "picture and firmware must agree from frame one" requirement.
// ---------------------------------------------------------------------------
{
  dev.setPose(0, 0); // scramble away from LEFT first, so this is a real check
  advance(600);
  dev.setPose(-90, 90); // the page's own boot defaults
  advance(16, 16); // one board-cadence tick: tilt_submit_device_g()'s first
                    // sample after a reset-like gap lands whole, not filtered
  const first = dev.tilt();
  check(
    "the page's boot pose (TURN=LEFT, TILT=90) reads up=LEFT within one tick",
    first.up === UP_LEFT,
    `up=${UP_NAME[first.up]} after ~16ms`
  );
}

// ---------------------------------------------------------------------------
// 3. Near flat, the firmware HOLDS the previous up-edge (tilt.c's own
//    hysteresis) rather than the emulator's TURN control faking a new
//    answer - dragging TILT down must behave exactly like tilting a real
//    puck back toward the table, not like re-picking an edge.
// ---------------------------------------------------------------------------
{
  dev.setPose(90, 90); // decisive RIGHT
  advance(600);
  check("RIGHT settles before the hold is tested", dev.tilt().up === UP_RIGHT);

  dev.setPose(90, 5); // same TURN, dragged nearly flat
  advance(1000);
  const held = dev.tilt();
  check(
    "dragging TILT down to 5deg holds the previous up-edge rather than reporting a new one",
    held.up === UP_RIGHT,
    `up=${UP_NAME[held.up]} deg=${held.deg.toFixed(1)}`
  );
  check("and tiltDeg itself reads back near flat", near(held.deg, 5, 1), `deg=${held.deg.toFixed(1)}`);
}

console.log(`\n${passCount} passed, ${failCount} failed`);
process.exit(failCount === 0 ? 0 : 1);
