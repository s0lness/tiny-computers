/**
 * sweep-bowling-grace: measures RELEASE_GRACE_MS in firmware/apps/bowling.c
 * the same way tools/sweep-tables-grace.ts measures tables.c's own constant
 * of the same name (that file's header comment is the precedent this one
 * follows rather than re-deriving): rebuild emu.wasm at each candidate value
 * (EMU_EXTRA_DEFINES=-DRELEASE_GRACE_MS=<n>, see emulator/wasm/build.ts's
 * own header for that mechanism), drive the compiled firmware through many
 * grab-flick-release trials under the SAME calibrated dropout-heavy touch
 * stream this project already treats as authoritative
 * (TOUCHSIM_HARDWARE_MEASURED, emulator/src/constants.ts: 34 dropout
 * episodes/sec at 60Hz plus the controller's own position-jitter weather,
 * measured on the real FT3168), and count what bowling.c's own header
 * comment (section 1) names as the risk of shrinking this constant:
 *
 *   - a mid-flick dropout longer than the grace window is mistaken for the
 *     real release, launching the ball EARLY - while she is still winding
 *     up or mid-swing - at whatever partial velocity happens to be sitting
 *     in the velocity ring buffer at that instant, not the throw she was
 *     actually building toward.
 *
 * Unlike tables.c (where a too-short grace costs a wrong DIGIT), a
 * premature bowling release only ever produces a visible "launch" line at
 * all when the ring buffer's contents already clear MIN_LAUNCH_SPEED in the
 * forward direction - launch_velocity_from_vbuf() silently absorbs anything
 * slower or backward as "she let go without really throwing," the same
 * bucket a deliberate too-slow release falls into. So this sweep measures
 * TWO things, not one:
 *
 *   1. HOW OFTEN a premature launch fires at all (a nonzero-speed forward
 *      throw that fires before the scripted gesture's own true release
 *      point) - the frequency the tables sweep already reports.
 *   2. HOW WRONG it is when it does - bowling.c's own brief names this
 *      explicitly ("wrong speed and wrong place"), and it is not visible
 *      from a frequency count alone. This file drives a two-phase flick
 *      (a stationary WIND-UP long enough to arm, then a constant-velocity
 *      SWING toward the pins ending in the real, scripted release) and
 *      compares every premature launch's reported (vx, vy) against a
 *      CANONICAL reference: the same exact motion, replayed with no
 *      simulated dropouts at all, so its one "bowling: launch" line is what
 *      the throw was actually supposed to produce. The comparison is a
 *      relative speed ratio (a premature fire mid-swing has not yet
 *      accumulated a full velocity-window's worth of swing-speed samples,
 *      so it reads weaker than the real thing) and how many milliseconds
 *      early it fired.
 *
 * Run with (no prior build needed - this rebuilds emu.wasm itself, once per
 * candidate value):
 *
 *   bun tools/sweep-bowling-grace.ts [--values 80,120,160,200,250,290,300] [--trials 200]
 *
 * NOT FAST, for the same reason sweep-tables-grace.ts is not fast: a build
 * per candidate plus (trials x 2) scenario runs each. Leaves
 * emulator/wasm/dist/emu.wasm on whichever candidate ran last; rebuild it
 * clean afterwards (`bun run emulator/wasm/build.ts`) before trusting it for
 * anything else - this tool prints a reminder at the end too.
 */
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { TouchSim } from "../emulator/src/touchsim";
import { TOUCHSIM_HARDWARE_MEASURED } from "../emulator/src/constants";
import { seededRng, seedFromName } from "./gate/touch";

const ROOT = join(import.meta.dir, "..");
const BUILD_TS = join(ROOT, "emulator", "wasm", "build.ts");
const WASM_PATH = join(ROOT, "emulator", "wasm", "dist", "emu.wasm");

const PANEL_W = 368, PANEL_H = 448;

// bowling.c's own layout/timing constants, lifted rather than re-derived -
// the convention every test/tool under this directory uses against its own
// app (see feature-bowling.ts, repro-touch-dropout-bowling-throw.ts).
const BEZEL = 10;
const LAND_W = PANEL_H; // 448
const SAFE_X0 = BEZEL, SAFE_X1 = LAND_W - BEZEL;
const SAFE_Y0 = BEZEL, SAFE_Y1 = PANEL_W - BEZEL;
const LANE_CY = (SAFE_Y0 + SAFE_Y1) / 2;
const BALL_START_X = SAFE_X0 + 58;
const BALL_START_Y = LANE_CY;
const ARM_MS = 40;
const MIN_LAUNCH_SPEED = 0.16;
const MAX_LAUNCH_SPEED = 1.55;
const LAUNCH_GAIN = 0.62;

const args = process.argv.slice(2);
function argVal(name: string, dflt: string): string {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] !== undefined ? args[i + 1]! : dflt;
}
const CANDIDATES = argVal("values", "80,120,160,200,250,290,300").split(",").map(Number);
const TRIALS = Number(argVal("trials", "200"));

// gfx.h's mapping is landscape (lx, ly) -> panel (PANEL_W-1-ly, lx) - the
// same conversion every test/tool in this directory applies against a raw
// touch report, which already arrives in panel space.
function landToPanel(lx: number, ly: number): [number, number] {
  return [PANEL_W - 1 - Math.round(ly), Math.round(lx)];
}

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
  const APP_BOWLING = apps.map((a) => a.toLowerCase()).indexOf("bowling");
  if (APP_BOWLING < 0) throw new Error("no bowling app in this emu.wasm");
  e.emu_tick(0);
  e.emu_app_switch(APP_BOWLING);
  e.emu_tick(10);
  log.length = 0;
  return {
    feed(down: boolean, x: number, y: number, nowMs: number) { e.emu_touch(down ? 1 : 0, x, y); e.emu_tick(nowMs); },
    drainLog(): string[] { const o = log.slice(); log.length = 0; return o; },
  };
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

// bowling.c's own printf: "bowling: launch vx=%d.%03d vy=%s%d.%03d" - vx has
// no sign character (the app only ever prints a launch with vx > 0), vy
// carries an explicit one (see that printf's own comment on why: %d on a
// truncated float between -1 and 0 silently drops the sign).
function parseLaunch(line: string): { vx: number; vy: number } | null {
  const m = line.match(/bowling: launch vx=(\d+)\.(\d+)\s+vy=(-?)(\d+)\.(\d+)/);
  if (!m) return null;
  const vx = Number(m[1]) + Number(m[2]) / 1000;
  const vySign = m[3] === "-" ? -1 : 1;
  const vy = vySign * (Number(m[4]) + Number(m[5]) / 1000);
  return { vx, vy };
}

const profile = TOUCHSIM_HARDWARE_MEASURED;
const STEP_MS = 1000 / profile.reportRateHz;

// ---- Scenario A: a thumb held STILL near the ball for 2s (grabbed, never
// swung) must never produce a launch line - the baseline sanity check this
// project's tables sweep also runs, adapted to bowling's own vocabulary
// (log line, not a digit). A still hold's vbuf velocity sits at ~0, so this
// is expected to hold at every grace value; it is here to catch a
// regression, not to find the knee. ------------------------------------
async function scenarioHeldStill(compiled: WebAssembly.Module, graceMs: number) {
  const HOLD_MS = 2000;
  let premature = 0;
  for (let i = 0; i < TRIALS; i++) {
    const dev = await loadDevice(compiled);
    const sim = new TouchSim(profile, PANEL_W, PANEL_H, seededRng(seedFromName(`bgrace-${graceMs}-held-${i}`)));
    const [px, py] = landToPanel(BALL_START_X, BALL_START_Y);
    sim.setPointer(true, px, py);
    let t = 1000, fired = false;
    for (let e = 0; e < HOLD_MS; e += STEP_MS) {
      t += STEP_MS;
      const r = sim.poll(t);
      dev.feed(r.fingers === 1, r.x, r.y, t);
      if (dev.drainLog().some((l) => l.includes("bowling: launch"))) { fired = true; break; }
    }
    if (fired) premature++;
  }
  return { premature, trials: TRIALS };
}

// ---- Scenario B: the real event. WIND_UP_MS stationary at the ball (long
// enough to arm and to fill the vbuf with near-zero-velocity samples), then
// a CONSTANT-velocity SWING toward the pins for SWING_MS, ending in the
// scripted, genuine release. SWING_MS is long enough that the vbuf window
// (VBUF_LEN=6 accepted samples, ~100-250ms under this profile's dropout
// rate) is fully flushed to swing-only samples well before the real
// release, so a canonical reference run (same motion, zero simulated
// dropouts) gives one stable (vx, vy) to compare every premature fire
// against - see this file's header. --------------------------------------
const WIND_UP_MS = 200;
const SWING_SPEED = 0.7; // raw finger px/ms; *LAUNCH_GAIN ~= 0.434, a solid
                          // moderate throw per bowling.c's own worked table
const SWING_MS = 400;
const TRUE_RELEASE_MS = WIND_UP_MS + SWING_MS; // gesture-local

function swingTarget(elapsedMs: number): { lx: number; ly: number } {
  if (elapsedMs <= WIND_UP_MS) return { lx: BALL_START_X, ly: LANE_CY };
  const swingT = Math.min(elapsedMs - WIND_UP_MS, SWING_MS);
  return { lx: BALL_START_X + SWING_SPEED * swingT, ly: LANE_CY };
}

// Canonical reference: the identical motion, clean (no TouchSim at all),
// fed at the same cadence so the vbuf window's semantics match. One
// "bowling: launch" line, the throw this gesture is actually supposed to
// produce.
async function canonicalReference(compiled: WebAssembly.Module) {
  const dev = await loadDevice(compiled);
  let t = 1000;
  for (let e = 0; e <= TRUE_RELEASE_MS; e += STEP_MS) {
    t += STEP_MS;
    const { lx, ly } = swingTarget(e);
    const [px, py] = landToPanel(lx, ly);
    dev.feed(true, px, py, t);
  }
  dev.drainLog();
  let launch: { vx: number; vy: number } | null = null;
  for (let e = 0; e < 800; e += STEP_MS) {
    t += STEP_MS;
    dev.feed(false, 0, 0, t);
    for (const l of dev.drainLog()) {
      const p = parseLaunch(l);
      if (p) launch = p;
    }
  }
  if (!launch) throw new Error("canonical reference flick never launched - check SWING_SPEED/timings");
  return launch;
}

async function scenarioFlick(compiled: WebAssembly.Module, graceMs: number, ref: { vx: number; vy: number }) {
  const refSpeed = Math.sqrt(ref.vx * ref.vx + ref.vy * ref.vy);
  let premature = 0, realFired = 0, neverFired = 0;
  const speedRatios: number[] = [];
  const earlyMs: number[] = [];
  for (let i = 0; i < TRIALS; i++) {
    const dev = await loadDevice(compiled);
    const sim = new TouchSim(profile, PANEL_W, PANEL_H, seededRng(seedFromName(`bgrace-${graceMs}-flick-${i}`)));
    let t = 1000;
    let premLaunch: { vx: number; vy: number; atMs: number } | null = null;

    for (let e = 0; e <= TRUE_RELEASE_MS; e += STEP_MS) {
      const { lx, ly } = swingTarget(e);
      const [px, py] = landToPanel(lx, ly);
      sim.setPointer(true, px, py);
      t += STEP_MS;
      const r = sim.poll(t);
      dev.feed(r.fingers === 1, r.x, r.y, t);
      if (!premLaunch) {
        for (const l of dev.drainLog()) {
          const p = parseLaunch(l);
          if (p) premLaunch = { ...p, atMs: e };
        }
      } else {
        dev.drainLog();
      }
    }

    if (premLaunch) {
      premature++;
      const speed = Math.sqrt(premLaunch.vx * premLaunch.vx + premLaunch.vy * premLaunch.vy);
      speedRatios.push(speed / refSpeed);
      earlyMs.push(TRUE_RELEASE_MS - premLaunch.atMs);
      continue;
    }

    // The scripted release itself: lift for real, wait out the grace plus
    // margin, and see whether the genuine throw still fires (it must,
    // regardless of grace value - grace only delays belief, it never
    // prevents it).
    sim.setPointer(false, 0, 0);
    let realLaunch: { vx: number; vy: number } | null = null;
    for (let e = 0; e < graceMs + 400; e += STEP_MS) {
      t += STEP_MS;
      const r = sim.poll(t);
      dev.feed(r.fingers === 1, r.x, r.y, t);
      for (const l of dev.drainLog()) {
        const p = parseLaunch(l);
        if (p) realLaunch = p;
      }
    }
    if (realLaunch) realFired++; else neverFired++;
  }
  return { premature, realFired, neverFired, trials: TRIALS, speedRatios, earlyMs };
}

function stats(xs: number[]): string {
  if (xs.length === 0) return "n/a";
  const mean = xs.reduce((a, b) => a + b, 0) / xs.length;
  const min = Math.min(...xs), max = Math.max(...xs);
  return `mean ${mean.toFixed(2)}, range [${min.toFixed(2)}, ${max.toFixed(2)}]`;
}

async function main() {
  console.log("=== RELEASE_GRACE_MS sweep: firmware/apps/bowling.c ===");
  console.log(`profile: TOUCHSIM_HARDWARE_MEASURED - ${profile.dropoutsPerSec} dropout episodes/sec @ ${profile.reportRateHz}Hz, ` +
    `jitter ${profile.positionJitterPerSec}/sec (${profile.positionJitterMinPx}-${profile.positionJitterMaxPx}px, up to ${profile.positionJitterMaxHoldReports} reports)`);
  console.log(`${TRIALS} trials per scenario per value, candidates: ${CANDIDATES.join(", ")}`);
  console.log(`flick profile: ${WIND_UP_MS}ms wind-up (stationary) then ${SWING_MS}ms swing at ${SWING_SPEED}px/ms raw ` +
    `(~${(SWING_SPEED * LAUNCH_GAIN).toFixed(3)} launch px/ms post-gain), true release at t=${TRUE_RELEASE_MS}ms\n`);

  const rows: { grace: number; held: Awaited<ReturnType<typeof scenarioHeldStill>>; flick: Awaited<ReturnType<typeof scenarioFlick>>; ref: { vx: number; vy: number } }[] = [];

  for (const grace of CANDIDATES) {
    console.log(`--- RELEASE_GRACE_MS=${grace} ---`);
    buildWith(grace);
    const compiled = await WebAssembly.compile(readFileSync(WASM_PATH));

    const ref = await canonicalReference(compiled);
    const refSpeed = Math.sqrt(ref.vx * ref.vx + ref.vy * ref.vy);
    console.log(`  canonical reference throw: vx=${ref.vx.toFixed(3)} vy=${ref.vy.toFixed(3)} speed=${refSpeed.toFixed(3)} px/ms`);

    const held = await scenarioHeldStill(compiled, grace);
    const flick = await scenarioFlick(compiled, grace, ref);

    console.log(`  held-still (2s), premature launches:  ${held.premature}/${held.trials}`);
    console.log(`  flick (${TRUE_RELEASE_MS}ms), premature launches: ${flick.premature}/${flick.trials}` +
      (flick.premature > 0 ? `  [speed ratio vs canonical: ${stats(flick.speedRatios)}]  [fired early by (ms): ${stats(flick.earlyMs)}]` : ""));
    console.log(`  flick, scripted release still fired correctly: ${flick.realFired}/${flick.trials - flick.premature} of the non-premature trials ` +
      `(${flick.neverFired} never fired at all)\n`);

    rows.push({ grace, held, flick, ref });
  }

  console.log("=== summary ===");
  console.log("grace(ms)  held-still premature   flick premature (of N)   premature speed ratio (mean)   fired early (mean ms)   scripted release missed");
  for (const r of rows) {
    const ratioMean = r.flick.speedRatios.length ? (r.flick.speedRatios.reduce((a, b) => a + b, 0) / r.flick.speedRatios.length).toFixed(2) : "-";
    const earlyMean = r.flick.earlyMs.length ? (r.flick.earlyMs.reduce((a, b) => a + b, 0) / r.flick.earlyMs.length).toFixed(0) : "-";
    console.log(`${String(r.grace).padStart(9)}  ${String(r.held.premature).padStart(4)}/${r.held.trials}` +
      `${String(r.flick.premature).padStart(21)}/${r.flick.trials}` +
      `${String(ratioMean).padStart(24)}` +
      `${String(earlyMean).padStart(23)}` +
      `${String(r.flick.neverFired).padStart(25)}`);
  }

  console.log("\nA premature launch's speed ratio close to 1.0 means it is barely distinguishable from the");
  console.log("real throw (harmless); close to 0 means a bare nudge fired as if it were a full release.");
  console.log(`emulator/wasm/dist/emu.wasm is now built with RELEASE_GRACE_MS=${CANDIDATES[CANDIDATES.length - 1]} - `);
  console.log("rebuild it clean (bun run emulator/wasm/build.ts) before trusting it for anything else.");
}

main();
