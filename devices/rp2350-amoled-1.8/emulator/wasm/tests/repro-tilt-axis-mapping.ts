// repro-tilt-axis-mapping: pins firmware/runtime/tilt.c's device_to_panel()
// against the four real-hardware readings that settled it, 2026-08-17. Run
// with:
//
//   bun run emulator/wasm/build.ts && bun run emulator/wasm/tests/repro-tilt-axis-mapping.ts
//
// WHY THIS IS A repro-*, NOT A feature-*. Every other orientation test in
// this directory (feature-tilt.ts, feature-tiltball.ts) injects gravity
// through emu_sensor_vector() AS IF it were already panel-space, exactly the
// convention that file's own header comment documents - and that
// convention is what makes those tests blind to device_to_panel(): whatever
// this file's raw-to-panel mapping is, it is the identity from THEIR point
// of view by construction (emu_shim.c's own header comment on
// emu_sensor_vector() says so plainly). This file exists BECAUSE that gap
// is real, not despite it: it drives emu_sensor_vector() with the actual
// RAW device-axis numbers devlink's TILT command printed on the physical
// board, so the compiled device_to_panel() runs for real - the one function
// in this codebase no software oracle could check before the axis ritual
// gave it something to check against (tilt.h's own "THE AXIS RITUAL"
// section). This is the test that would have caught the bug the owner
// found by hand: identity device_to_panel() plus runtime_core.c's landscape
// rotation put a BOOT/PWR tilt on breakout's gy ("front/back") instead of
// its gx ("left/right"), so the paddle never moved.
//
// WHAT THIS STILL CANNOT SEE: whether device_to_panel()'s mapping is
// actually correct on the physical board - only that it keeps producing
// what tilt.c's own header comment says it was derived from. The board is
// the only oracle for the first question; see tilt.h's ritual.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

// runtime_core.c's g_apps[] order (AGENTS.md, "Layout"): chrono is index 0
// (landscape, what the flat/top-up/quarter-turn poses were read against),
// breakout is index 9 (landscape too - the BOOT-tilt pose was read with it
// running, since that is the app whose paddle the owner found unresponsive).
const APP_CHRONO = 0;
const APP_BREAKOUT = 9;

const SETTLE_MS = 2000; // comfortably past tilt.h's filter (TILT_FC_MIN_HZ's
                         // 177ms time constant, plus BETA's speedup on a
                         // hard step) - feature-tilt.ts measures full
                         // convergence well under a second.
const FRAME_MS = 20; // the board's own IMU-poll cadence (sensors.c's
                      // IMU_POLL_MS), so the filter steps the same way it
                      // does on real hardware.

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
        "Rebuild it (bun run emulator/wasm/build.ts) first."
    );
  }
  if (exp.emu_init() !== 1) throw new Error("emu_init() failed - see the [fw] lines above");

  return {
    // Deliberately RAW: unlike feature-tilt.ts's gravity(), this hands
    // emu_sensor_vector() the actual device-axis reading a real IMU sample
    // would carry, so tilt_submit_device_g() runs device_to_panel() on it
    // for real rather than on an already-panel-space number.
    raw(x: number, y: number, z: number) {
      exp.emu_sensor_vector(1, x, y, z);
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

let t = 1000;
function settle(appIndex: number, rawX: number, rawY: number, rawZ: number): Tilt {
  dev.switchTo(appIndex);
  const end = t + SETTLE_MS;
  while (t < end) {
    t += FRAME_MS;
    dev.raw(rawX, rawY, rawZ);
    dev.tick(t);
  }
  return dev.tilt();
}

// The four rows measured on real hardware 2026-08-17 (bun tools/dev.ts
// tilt), RAW column, and what a landscape app must receive once
// device_to_panel() (tilt.c) and runtime_core.c's tilt_for_app() landscape
// rotation (untouched, and not this file's business) both apply. Derivation
// and the determinant check are in tilt.c's own header comment on
// device_to_panel(); this file only pins the arithmetic's result.
//
// TOL is loose enough to absorb the raw table's own +-0.03g of hand-holding
// noise (these are literal field measurements, not synthetic unit vectors)
// while still catching a swapped or wrong-signed axis, which is off by
// magnitudes, not hundredths.
const TOL = 0.03;

{
  const t1 = settle(APP_CHRONO, 0.039, 0.050, -1.009);
  check(
    "flat on the table, screen up: chrono reads (0.04, -0.05, 1.01)",
    near(t1.gx, 0.039, TOL) && near(t1.gy, -0.050, TOL) && near(t1.gz, 1.009, TOL),
    `g=(${t1.gx.toFixed(3)}, ${t1.gy.toFixed(3)}, ${t1.gz.toFixed(3)})`
  );
}

{
  const t2 = settle(APP_CHRONO, 1.021, 0.029, 0.050);
  check(
    "upright, top edge up: chrono reads (1.02, -0.03, -0.05)",
    near(t2.gx, 1.021, TOL) && near(t2.gy, -0.029, TOL) && near(t2.gz, -0.050, TOL),
    `g=(${t2.gx.toFixed(3)}, ${t2.gy.toFixed(3)}, ${t2.gz.toFixed(3)})`
  );
}

{
  const t3 = settle(APP_CHRONO, 0.035, 1.049, 0.029);
  check(
    "quarter turn, right edge up: chrono reads (0.04, -1.05, -0.03)",
    near(t3.gx, 0.035, TOL) && near(t3.gy, -1.049, TOL) && near(t3.gz, -0.029, TOL),
    `g=(${t3.gx.toFixed(3)}, ${t3.gy.toFixed(3)}, ${t3.gz.toFixed(3)})`
  );
}

{
  // The pose that started this: the puck tilted toward BOOT, breakout
  // running, paddle reading tilt.gx.
  const t4 = settle(APP_BREAKOUT, -0.855, -0.014, -0.482);
  check(
    "tilted toward BOOT: breakout reads (-0.86, 0.01, 0.48)",
    near(t4.gx, -0.855, TOL) && near(t4.gy, 0.014, TOL) && near(t4.gz, 0.482, TOL),
    `g=(${t4.gx.toFixed(3)}, ${t4.gy.toFixed(3)}, ${t4.gz.toFixed(3)})`
  );
  // The actual owner complaint, restated as its own assertion so a future
  // regression fails on the readable sentence rather than only on a
  // three-decimal vector: tilting toward BOOT has to move the paddle LEFT
  // (negative gx), toward PWR has to move it RIGHT. Under the identity
  // mapping this replaced, gx measured -0.026 here - indistinguishable from
  // flat - because the gesture was landing on gy instead.
  check(
    "tilting toward BOOT lands on gx, not gy - the paddle actually moves",
    t4.gx < -0.3,
    `gx=${t4.gx.toFixed(3)} (was -0.026 under the identity mapping)`
  );
}

console.log(`\n${passCount} passed, ${failCount} failed`);
process.exit(failCount === 0 ? 0 : 1);
