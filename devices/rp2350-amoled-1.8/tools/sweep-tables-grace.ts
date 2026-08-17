/**
 * sweep-tables-grace: measures RELEASE_GRACE_MS in firmware/apps/tables.c
 * the same way tables.c's own header comment says COMMIT_CONFIRM_MS was
 * measured: rebuild emu.wasm at each candidate value
 * (EMU_EXTRA_DEFINES=-DRELEASE_GRACE_MS=<n>, see emulator/wasm/build.ts's
 * own header for that mechanism), drive the compiled firmware through many
 * press-hold-release trials under the SAME calibrated dropout-heavy touch
 * stream this project already treats as authoritative
 * (TOUCHSIM_HARDWARE_MEASURED, emulator/src/constants.ts: 34 dropout
 * episodes/sec at 60Hz, measured on the real FT3168, TOUCH_POLL_SELFTEST,
 * 2026-08-14), and count BOTH failure modes the constant trades against
 * each other:
 *
 *   - PREMATURE commits: a run of dropped reports longer than the grace
 *     window, while a real finger is still down (held still, or mid-slide),
 *     reads as a genuine lift and commits whatever cell happens to be
 *     hovered - the exact thing this window exists to bridge.
 *   - WRONG/MISSED commits on a normal press-release: too small a grace
 *     also costs a NORMAL tap, because a premature verdict mid-press resets
 *     the whole gesture (tables_tick() clears contactSeen/armed/hoverCell/
 *     pendingCell together), so the real release that follows starts a
 *     brand new, possibly not-yet-armed gesture instead of finishing the
 *     one she thinks she is still holding.
 *
 * TOOLS/SWEEP-TABLES-COMMIT.TS, WHICH tables.c'S OWN COMMENT CITES FOR
 * COMMIT_CONFIRM_MS=72, DOES NOT EXIST IN THIS REPOSITORY'S HISTORY.
 * `git log --all -S sweep-tables -- .` finds the string only in the two
 * commits that added tables.c itself (078eb45, a9a8cd6) and in this
 * directory's own comments - never a tools/sweep-tables-commit.ts file, in
 * any commit, ever. It was run ad hoc against a local build and never
 * committed. This file is the same measurement for RELEASE_GRACE_MS,
 * rebuilt from the same methodology and committed this time, so the next
 * constant this app needs measured has something to re-run rather than a
 * citation to a file nobody can find.
 *
 * Run with (no prior build needed - this rebuilds emu.wasm itself, once per
 * candidate value):
 *
 *   bun tools/sweep-tables-grace.ts [--values 80,120,160,200,250,300] [--trials 200]
 *
 * NOT FAST: a build per candidate (the zig link alone is a few seconds,
 * sometimes retried - see emulator/wasm/build.ts's own header) plus three
 * scenarios of `trials` runs each. Leaves emulator/wasm/dist/emu.wasm on
 * whichever candidate ran last; rebuild it clean afterwards
 * (`bun run emulator/wasm/build.ts`) before trusting it for anything else -
 * this tool prints a reminder at the end too.
 *
 * LAST MEASURED 2026-08-17: 80,120,160,200,250,300 at 200 trials/scenario,
 * then 260,270,280,290,300,320,350 at 400 trials/scenario to find the knee
 * precisely. Premature commits (see above) fall from 94% at 80ms to a noisy
 * near-zero tail (0-1%) across 250-280ms, then go cleanly to zero at 290ms
 * and stay there through 350ms. 300ms - the value copied from menu.c/
 * four.c - sits inside that clean band with margin on both sides, which is
 * NOT what "a copied number, probably conservative" predicts: this is a
 * real floor the FT3168's measured 34/sec dropout rate imposes, not room to
 * shrink. tables.c's RELEASE_GRACE_MS is left at 300 because of this
 * result, not in spite of running it. Re-run this after any change to the
 * dropout profile TOUCHSIM_HARDWARE_MEASURED uses, or if RELEASE_GRACE_MS
 * is ever reconsidered.
 */
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { TouchSim } from "../emulator/src/touchsim";
import { TOUCHSIM_HARDWARE_MEASURED } from "../emulator/src/constants";
import { seededRng, seedFromName } from "./gate/touch";
import { PANEL_W, PANEL_H, CELL_ZERO, cellCx, cellCy, landToPanel } from "./tables-layout";

const ROOT = join(import.meta.dir, "..");
const BUILD_TS = join(ROOT, "emulator", "wasm", "build.ts");
const WASM_PATH = join(ROOT, "emulator", "wasm", "dist", "emu.wasm");

// tables.c's own layout constants, read from ./tables-layout.ts - the
// convention every test/tool under this directory uses against its app
// (see feature-tables.ts, repro-touch-dropout-tables.ts, preview-tables.ts)
// - rather than the hand-copied `CELL_W=99, CELL_H=64, NUMPAD_Y0=OY+92`
// this file carried until 2026-08-17, which was still the layout FROM
// BEFORE that day's redesign: silently hit-testing the wrong rectangle
// rather than failing, the exact hollow-test failure mode tables-layout.ts's
// own header argues against.

// COMMIT_CONFIRM_MS is held fixed at its own measured value throughout this
// sweep - this file is about RELEASE_GRACE_MS only, one constant at a time,
// same discipline tables.c's own header used.
const COMMIT_CONFIRM_MS = 72;

const args = process.argv.slice(2);
function argVal(name: string, dflt: string): string {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] !== undefined ? args[i + 1]! : dflt;
}
const CANDIDATES = argVal("values", "80,120,160,200,250,300").split(",").map(Number);
const TRIALS = Number(argVal("trials", "200"));

function buildWith(graceMs: number): void {
  const env = { ...process.env, EMU_EXTRA_DEFINES: `-DRELEASE_GRACE_MS=${graceMs}` };
  const res = Bun.spawnSync(["bun", "run", BUILD_TS], { cwd: ROOT, env, stdout: "inherit", stderr: "inherit" });
  if (!res.success) throw new Error(`build failed for RELEASE_GRACE_MS=${graceMs} (exit ${res.exitCode})`);
}

async function loadDevice(compiled: WebAssembly.Module) {
  let memory!: WebAssembly.Memory;
  const dec = new TextDecoder();
  const log: string[] = [];
  const inst = await WebAssembly.instantiate(compiled, {
    env: {
      js_log(p: number, l: number) { log.push(dec.decode(new Uint8Array(memory.buffer, p, l))); },
      sinf: (x: number) => Math.sin(x),
      cosf: (x: number) => Math.cos(x),
      atan2f: (y: number, x: number) => Math.atan2(y, x),
      sqrtf: (x: number) => Math.sqrt(x),
      fabsf: (x: number) => Math.abs(x),
      floorf: (x: number) => Math.floor(x),
      fmodf: (x: number, y: number) => x % y,
      powf: (x: number, y: number) => Math.pow(x, y),
      expf: (x: number) => Math.exp(x),
    },
  });
  memory = inst.exports.memory as WebAssembly.Memory;
  const e = inst.exports as any;
  if (e.emu_init() !== 1) throw new Error("emu_init() failed");
  const jsonBytes = new Uint8Array(memory.buffer, e.emu_device());
  let end = 0; while (jsonBytes[end] !== 0) end++;
  const apps: string[] = JSON.parse(dec.decode(jsonBytes.subarray(0, end))).apps || [];
  const APP_TABLES = apps.map((a) => a.toLowerCase()).indexOf("tables");
  if (APP_TABLES < 0) throw new Error("no tables app in this emu.wasm");
  e.emu_tick(0);
  e.emu_app_switch(APP_TABLES);
  e.emu_tick(10);
  log.length = 0;
  return {
    feed(down: boolean, x: number, y: number, nowMs: number) { e.emu_touch(down ? 1 : 0, x, y); e.emu_tick(nowMs); },
    drainLog(): string[] { const o = log.slice(); log.length = 0; return o; },
  };
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

const profile = TOUCHSIM_HARDWARE_MEASURED;
const STEP_MS = 1000 / profile.reportRateHz;

// ---- Scenario A: a thumb held STILL on one cell for 2s, under the
// dropout+jitter weather, must never commit before the intentional release
// at the end - RELEASE_GRACE_MS's whole reason to exist. -------------------
async function scenarioHeldStill(compiled: WebAssembly.Module, graceMs: number) {
  const HOLD_MS = 2000;
  let premature = 0;
  for (let i = 0; i < TRIALS; i++) {
    const dev = await loadDevice(compiled);
    const sim = new TouchSim(profile, PANEL_W, PANEL_H, seededRng(seedFromName(`grace-${graceMs}-held-${i}`)));
    const cell = i % 9;
    const [px, py] = landToPanel(cellCx(cell), cellCy(cell));
    sim.setPointer(true, px, py);
    let t = 1000, committed = false;
    for (let e = 0; e < HOLD_MS; e += STEP_MS) {
      t += STEP_MS;
      const r = sim.poll(t);
      dev.feed(r.fingers === 1, r.x, r.y, t);
      if (dev.drainLog().some((l) => l.includes("tables: digit") || l.includes("tables: backspace"))) { committed = true; break; }
    }
    if (committed) premature++;
  }
  return { premature, trials: TRIALS };
}

// ---- Scenario B: press, DRAG across the pad while still deciding (a real
// "sustained slide"), then a genuine release - measures the same premature-
// commit property under motion, plus whether the real release still lands
// the correct cell. ---------------------------------------------------------
async function scenarioDrag(compiled: WebAssembly.Module, graceMs: number) {
  const DRAG_MS = 900, SETTLE_MS = 320;
  let premature = 0, correct = 0, wrong = 0, missed = 0;
  for (let i = 0; i < TRIALS; i++) {
    const dev = await loadDevice(compiled);
    const sim = new TouchSim(profile, PANEL_W, PANEL_H, seededRng(seedFromName(`grace-${graceMs}-drag-${i}`)));
    const from = i % 9, to = (i * 5 + 4) % 9;
    let t = 1000, early = false;
    for (let e = 0; e < DRAG_MS; e += STEP_MS) {
      const u = e / DRAG_MS;
      const [px, py] = landToPanel(
        cellCx(from) + (cellCx(to) - cellCx(from)) * u,
        cellCy(from) + (cellCy(to) - cellCy(from)) * u);
      sim.setPointer(true, px, py);
      t += STEP_MS;
      const r = sim.poll(t);
      dev.feed(r.fingers === 1, r.x, r.y, t);
      if (dev.drainLog().some((l) => l.includes("tables: digit"))) early = true;
    }
    const [tx, ty] = landToPanel(cellCx(to), cellCy(to));
    sim.setPointer(true, tx, ty);
    for (let e = 0; e < SETTLE_MS; e += STEP_MS) {
      t += STEP_MS;
      const r = sim.poll(t);
      dev.feed(r.fingers === 1, r.x, r.y, t);
      if (dev.drainLog().some((l) => l.includes("tables: digit"))) early = true;
    }
    if (early) premature++;

    sim.setPointer(false, 0, 0);
    const digits: number[] = [];
    for (let e = 0; e < graceMs + 400; e += STEP_MS) {
      t += STEP_MS;
      const r = sim.poll(t);
      dev.feed(r.fingers === 1, r.x, r.y, t);
      for (const l of dev.drainLog()) {
        const m = l.match(/tables: digit (\d+)/);
        if (m) digits.push(Number(m[1]));
      }
    }
    const expected = to === CELL_ZERO ? 0 : to + 1;
    if (digits.length === 0) missed++;
    else if (digits.length === 1 && digits[0] === expected) correct++;
    else wrong++;
  }
  return { premature, correct, wrong, missed, trials: TRIALS };
}

// ---- Scenario C: a normal, STATIONARY press-release (no drag at all) -
// what a real tap costs when the grace window is too aggressive, isolated
// from any mid-gesture motion. 400ms is a realistic, unhurried child press,
// comfortably past the arm+confirm floor (112ms) under clean weather - long
// enough that this project's own heavy dropout rate gets several chances to
// produce a spuriously-long gap DURING the hold, not just at the real lift.
async function scenarioTap(compiled: WebAssembly.Module, graceMs: number) {
  const PRESS_MS = 400;
  let correct = 0, wrong = 0, missed = 0;
  for (let i = 0; i < TRIALS; i++) {
    const dev = await loadDevice(compiled);
    const sim = new TouchSim(profile, PANEL_W, PANEL_H, seededRng(seedFromName(`grace-${graceMs}-tap-${i}`)));
    const cell = i % 9;
    const [px, py] = landToPanel(cellCx(cell), cellCy(cell));
    sim.setPointer(true, px, py);
    let t = 1000;
    const digits: number[] = [];
    for (let e = 0; e < PRESS_MS; e += STEP_MS) {
      t += STEP_MS;
      const r = sim.poll(t);
      dev.feed(r.fingers === 1, r.x, r.y, t);
      for (const l of dev.drainLog()) {
        const m = l.match(/tables: digit (\d+)/);
        if (m) digits.push(Number(m[1]));
      }
    }
    sim.setPointer(false, 0, 0);
    for (let e = 0; e < graceMs + 400; e += STEP_MS) {
      t += STEP_MS;
      const r = sim.poll(t);
      dev.feed(r.fingers === 1, r.x, r.y, t);
      for (const l of dev.drainLog()) {
        const m = l.match(/tables: digit (\d+)/);
        if (m) digits.push(Number(m[1]));
      }
    }
    const expected = cell === CELL_ZERO ? 0 : cell + 1;
    if (digits.length === 0) missed++;
    else if (digits.length === 1 && digits[0] === expected) correct++;
    else wrong++;
  }
  return { correct, wrong, missed, trials: TRIALS };
}

async function main() {
  console.log("=== RELEASE_GRACE_MS sweep: firmware/apps/tables.c ===");
  console.log(`profile: TOUCHSIM_HARDWARE_MEASURED - ${profile.dropoutsPerSec} dropout episodes/sec @ ${profile.reportRateHz}Hz, ` +
    `jitter ${profile.positionJitterPerSec}/sec (${profile.positionJitterMinPx}-${profile.positionJitterMaxPx}px, up to ${profile.positionJitterMaxHoldReports} reports)`);
  console.log(`${TRIALS} trials per scenario per value, COMMIT_CONFIRM_MS held fixed at ${COMMIT_CONFIRM_MS}, candidates: ${CANDIDATES.join(", ")}\n`);

  const rows: { grace: number; held: Awaited<ReturnType<typeof scenarioHeldStill>>; drag: Awaited<ReturnType<typeof scenarioDrag>>; tap: Awaited<ReturnType<typeof scenarioTap>> }[] = [];

  for (const grace of CANDIDATES) {
    console.log(`--- RELEASE_GRACE_MS=${grace} ---`);
    buildWith(grace);
    const compiled = await WebAssembly.compile(readFileSync(WASM_PATH));

    const held = await scenarioHeldStill(compiled, grace);
    const drag = await scenarioDrag(compiled, grace);
    const tap = await scenarioTap(compiled, grace);

    console.log(`  held-still (2s), premature commits:  ${held.premature}/${held.trials}`);
    console.log(`  drag (~1.2s), premature commits:      ${drag.premature}/${drag.trials}`);
    console.log(`  drag release verdict:                 ${drag.correct} correct, ${drag.wrong} wrong, ${drag.missed} missed (of ${drag.trials})`);
    console.log(`  plain tap (400ms) release verdict:    ${tap.correct} correct, ${tap.wrong} wrong, ${tap.missed} missed (of ${tap.trials})\n`);

    rows.push({ grace, held, drag, tap });
  }

  console.log("=== summary ===");
  console.log("grace(ms)  premature (held/drag, of N each)      normal press-release (drag-end + plain tap), wrong+missed of 2N");
  for (const r of rows) {
    const premature = `${r.held.premature}+${r.drag.premature} of ${r.held.trials}+${r.drag.trials}`;
    const badReleases = (r.drag.wrong + r.drag.missed) + (r.tap.wrong + r.tap.missed);
    const totalReleases = r.drag.trials + r.tap.trials;
    console.log(`${String(r.grace).padStart(9)}  ${premature.padStart(20)}                     ${badReleases}/${totalReleases}`);
  }

  console.log("\nCopy the smallest value that keeps premature commits at zero WITH REAL MARGIN into");
  console.log("tables.c's RELEASE_GRACE_MS, with this table (or a re-run of it) in the comment there.");
  console.log(`emulator/wasm/dist/emu.wasm is now built with RELEASE_GRACE_MS=${CANDIDATES[CANDIDATES.length - 1]} - `);
  console.log("rebuild it clean (bun run emulator/wasm/build.ts) before trusting it for anything else.");
}

main();
