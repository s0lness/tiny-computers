// breakout-difficulty: a repeatable, seeded probe for how hard breakout
// (firmware/apps/breakout.c) actually is, against the REAL compiled
// firmware - same discipline as every file under emulator/wasm/tests/
// (decision 0003), not a reimplementation. Run:
//
//   bun run emulator/wasm/build.ts
//   bun tools/breakout-difficulty.ts
//
// WHY THIS EXISTS. Three lives landed (62f6917) and the paddle's hitbox was
// then corrected to match its own drawing (e97f7f5); neither one was ever
// tuned against the new fail state. e97f7f5's own commit message already
// measured the symptom - an idealised ball-tracking controller loses all
// three lives in 11.04s, unchanged by the hitbox fix - and named a
// hypothesis (the shared tilt filter's ~150ms settling time against a ball
// moving up to 0.30px/ms) without testing it. This file tests it, and
// everything else that could plausibly be the binding constraint, against
// the actual compiled game rather than by reasoning about the numbers.
//
// TWO CONTROLLERS, BOTH DRIVING THE REAL FIRMWARE THROUGH emu_sensor_vector.
//
//   IDEALIZED - perfect information (it reads the ball's own screen
//   position off the pushed-rectangle stream every tick, the same channel
//   feature-breakout.ts's own paddleCentre() reads the paddle from, just
//   applied to the ball instead - see "BALL TRACKING" below) and zero
//   reaction time beyond the one tick of latency any polling loop has. It
//   commands tilt EVERY tick from that reading. This is what
//   feature-breakout.ts's own header comment already calls "an idealised
//   controller... bypassing reaction time entirely" - this file is that
//   probe, kept rather than thrown away.
//
//   SLOPPY - a small child's hand, not a script: a reaction delay, a
//   voluntary-correction cadence rather than continuous servoing, aim error
//   on each correction, and a rate-limited hand (a puck cannot snap from
//   rail to rail). Every constant is named and justified where it is
//   defined below; NONE of them are measured, because nothing in this tree
//   can watch a real toddler's hand (decision 0003's own limit, restated for
//   this file specifically in "WHAT THIS PROBE CANNOT VERIFY" at the
//   bottom). Seeded (tools/gate/touch.ts's seededRng/seedFromName, the same
//   generator the touch-dropout regression tests already use), so a result
//   replays exactly and a bad seed cannot be blamed for a flapping number.
//
// ISOLATING THE BINDING CONSTRAINT: A THIRD, DIAGNOSTIC CONTROLLER.
//
// The idealised controller alone cannot tell a filter-lag problem apart from
// a paddle-too-narrow or ball-too-fast problem: all three would produce the
// same symptom (a controller that "should" catch everything still doesn't).
// So this file adds LEAD-COMPENSATED variants of the idealised controller:
// same perfect ball-position reads, but each one extrapolates the ball's
// tracked position forward by a fixed lead time (using the ball's own
// measured velocity between consecutive reads) before commanding tilt - a
// hand that starts moving toward where the ball WILL be, not where it just
// was. Sweeping the lead time answers the question directly: if survival and
// wall-clearance jump once lead approaches the filter's own measured
// settling time and do NOT keep improving much past it, the filter's lag is
// the binding constraint (this is the counterfactual - "if the lag were
// cancelled, would this still be hard?"). If nothing improves regardless of
// lead, the lag was never the constraint and the real one is geometric
// (paddle width, ball speed, or how far the ball falls per traverse) - see
// "GEOMETRY, STATED PLAINLY" below for those numbers, and the filter's own
// step-response/tracking measurements for the lag's size on its own terms.
//
// BALL TRACKING, FROM PUSHED RECTANGLES, NOT A PIXEL BLOB HUNT.
//
// breakout_tick() (breakout.c) pushes at most one rectangle per changed
// shape, smallest-thing-first: the ball's own rect (ball_rect(), a union of
// its old and new position, roughly 21-30px square) is ALWAYS the first
// dirty rect whenever the ball moved that tick, ahead of the paddle
// (~100x120px), the lives row (~52x16px, fixed position), the ripple
// (a FIXED 44x44px box) and any brick that just broke or is mid-regrow
// (~37-41px square, fully grown). So filtering emu_push_*() by size alone
// (<=32px either side) picks the ball out cleanly with no colour reasoning
// and no internal pointer read - the same "framebuffer/push-geometry only"
// convention every file under emulator/wasm/tests/ already holds itself to
// (AGENTS.md's "Regression tests": "something a person... could also have
// observed" - here, the push list is exactly what a person watching the
// dirty-rectangle overlay the emulator UI already draws would see too).
// Converting a panel-space push rect back to the LANDSCAPE x this app's own
// paddle math uses is one line, not a guess: feature-breakout.ts's own
// landPixel() establishes panel_y == landscape x (px=PANEL_W-1-ly,
// py=lx), so a panel rect's y0/h give the landscape x range directly -
// landX = push.y + push.h/2.
//
// PLAYABILITY, DEFINED BEFORE IT IS TUNED TO.
//
// This is a toy built for a very young child (AGENTS.md's own "a finger is
// about 100 pixels wide" section is the same audience). Two numbers, picked
// here rather than backed into after the fact:
//
//   IDEALIZED_TARGET_MS = 45000ms, AND a full 18-brick clear before game
//   over. A controller with perfect information and no human clumsiness is
//   the ceiling on what this game can ask of a hand - if THAT controller
//   cannot comfortably clear the wall, no child ever will, no matter how the
//   sloppy model above is tuned. 45s is generous room past the fastest a
//   clean run plausibly finishes, so failing this means the game is asking
//   for reaction speed no hand has, not that the run was merely unlucky.
//
//   SLOPPY_TARGET_MEDIAN_MS = 20000ms, with a median of at least 4 of 18
//   bricks broken. Long enough that a toddler's few real tilts visibly did
//   something before the table empties (this device's own idiom for "this
//   mattered" - the lives row, the wall thinning), short enough that losing
//   still actually happens most of the time, since three lives and a game
//   over were the owner's own explicit ask (breakout.c's header comment,
//   "IT IS OVERRULED HERE BECAUSE THE OWNER ASKED FOR IT") - a game that
//   cannot be lost by a clumsy hand is not the fail state that was
//   requested. 20s sits well clear of "over before she noticed it started"
//   (a slot machine) without being anywhere near "unlosable".
//
// Both are asserted below (check()), so this file is red before a fix and
// green after one, per this project's own standing rule.
//
// WHAT THIS PROBE CANNOT VERIFY, said before the numbers per decision 0010:
//   - whether SLOPPY's constants (reaction delay, correction cadence, aim
//     error, hand slew rate) actually match a real two-year-old's hand.
//     Nothing in this tree can watch one; these are named, justified
//     assumptions, not measurements, and the first person to hand this
//     board to a real small child should correct them if the sloppy
//     controller's survival does not match what was actually observed.
//   - the device-to-panel axis mapping (tilt.h's own open question) - this
//     file drives tilt exactly the way feature-breakout.ts already does
//     (undoing device_to_panel on the way in) and inherits that same gap.
//   - real accelerometer noise/tremor: the emulator's gravity is a
//     perfectly still, perfectly unit vector (emu_abi.h's own honesty
//     section) - SLOPPY's aim error stands in for a hand's own imprecision,
//     but it is imprecision in WHERE the hand aims, not in what the IMU
//     reports, which is a different (and real) source of noise this probe
//     does not model.
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { seededRng, seedFromName } from "./gate/touch";

const ROOT = join(import.meta.dir, "..");
const WASM_PATH = join(ROOT, "emulator", "wasm", "dist", "emu.wasm");

const PANEL_W = 368, PANEL_H = 448;
const LAND_W = PANEL_H, LAND_H = PANEL_W; // 448 x 368
const FRAME_MS = 16; // 60Hz host loop, same cadence feature-breakout.ts and repro-breakout-residue.ts already drive this app at

// ---- constants lifted from breakout.c's own #define block, the same
// convention every test under emulator/wasm/tests/ already follows against
// this app -------------------------------------------------------------
const PLAY_L = 26, PLAY_R = LAND_W - 26, PLAY_T = 26, PLAY_B = LAND_H - 26;
const BALL_R = 9;
const PADDLE_Y = 300, PADDLE_HALF_W = 46;
const PADDLE_CENTER_X = (PLAY_L + PLAY_R) / 2; // 224
const PADDLE_TRAVEL_MAX = (PLAY_R - PLAY_L) / 2 - PADDLE_HALF_W; // 152
const PADDLE_GX_FULL = 0.4;
const N_BRICKS = 18;
const START_LIVES = 3;
const BALL_SPEED_BASE = 0.15, BALL_SPEED_MAX = 0.28; // px/ms - keep in step with breakout.c's own #define block
const RESPAWN_X = PADDLE_CENTER_X, RESPAWN_Y = PADDLE_Y - 100;
const ROW2_PEAK_Y = 141; // the innermost row, closest to the paddle

const IDEALIZED_TARGET_MS = 45_000;
const SLOPPY_TARGET_MEDIAN_MS = 20_000;
const SLOPPY_TARGET_MEDIAN_BRICKS = 4;
const SLOPPY_SEEDS = 20;

let passCount = 0, failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
  if (ok) passCount++; else failCount++;
  console.log(`[${ok ? "PASS" : "FAIL"}] ${label}${detail ? " - " + detail : ""}`);
}
function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}

// ---- device loader, same shape as preview-breakout.ts / repro-breakout-
// residue.ts's own loadDevice(), plus emu_tilt() for the filter-only probes
// (feature-tilt.ts's own pattern). ---------------------------------------
interface Rect { x: number; y: number; w: number; h: number }

async function loadDevice() {
  let memory!: WebAssembly.Memory;
  const dec = new TextDecoder();
  const inst = await WebAssembly.instantiate(await WebAssembly.compile(readFileSync(WASM_PATH)), {
    env: {
      js_log(_p: number, _l: number) { /* quiet: this file drives thousands of frames */ },
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
  const APP_BREAKOUT = apps.indexOf("breakout");
  if (APP_BREAKOUT < 0) {
    throw new Error("this emu.wasm has no breakout app - rebuild it: bun run emulator/wasm/build.ts");
  }

  return {
    tick(nowMs: number) { e.emu_tick(nowMs); },
    // Undoes tilt.c's device_to_panel() on the way in, exactly like
    // feature-breakout.ts's own tilt(): argument is PANEL-space g.
    tiltPanel(x: number, y: number, z: number) { e.emu_sensor_vector(1, y, x, -z); },
    touch(down: boolean, x: number, y: number) { e.emu_touch(down ? 1 : 0, x, y); },
    appSwitch(i: number) { e.emu_app_switch(i); },
    appCurrent(): number { return e.emu_app_current(); },
    APP_BREAKOUT,
    // What the CURRENT app was handed on the last tick, after the filter
    // AND the landscape rotation - feature-tilt.ts's own oracle, field 0 =
    // gx. Only meaningful while breakout (a landscape app) is current.
    tiltGx(): number { return e.emu_tilt(0); },
    pushes(): Rect[] {
      const n = e.emu_push_count();
      const out: Rect[] = [];
      for (let i = 0; i < n; i++) out.push({ x: e.emu_push_x(i), y: e.emu_push_y(i), w: e.emu_push_w(i), h: e.emu_push_h(i) });
      return out;
    },
    // Live view, not a copy - re-derived every call per tools/gate/device.ts's
    // own note that a growable wasm memory can detach a stale view. Cheap:
    // no allocation, just a typed-array window.
    fbView(): Uint8Array { return new Uint8Array(memory.buffer, e.emu_fb(), PANEL_W * PANEL_H * 2); },
  };
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

async function enterBreakout(dev: Device): Promise<number> {
  dev.tick(0);
  dev.appSwitch(dev.APP_BREAKOUT);
  dev.tick(FRAME_MS);
  return FRAME_MS;
}

// ---- reading the panel, same formulas as feature-breakout.ts's own
// landPixel/isBlack/livesShown/countWallInk - kept here rather than shared,
// since this file's own header already explains why push geometry (not
// pixel scanning) does the heavy lifting; these two remain for the lives
// row (three fixed points, cheap every tick) and a start/end wall-ink sanity
// check (a full scan, so only ever called a handful of times per run). ----
function landPixel(fb: Uint8Array, lx: number, ly: number): { r: number; g: number; b: number } {
  const px = PANEL_W - 1 - ly, py = lx;
  const i = (py * PANEL_W + px) * 2;
  const swapped = (fb[i]! << 8) | fb[i + 1]!;
  const v = ((swapped & 0xff) << 8) | (swapped >> 8);
  return { r: (v >> 11) & 0x1f, g: (v >> 5) & 0x3f, b: v & 0x1f };
}
const isBlack = (p: { r: number; g: number; b: number }) => p.r <= 1 && p.g <= 1 && p.b <= 1;
const isWhite = (p: { r: number; g: number; b: number }) => p.r >= 30 && p.g >= 60 && p.b >= 30;

const LIFE_DOT_X0 = PLAY_L + 12, LIFE_DOT_Y = PLAY_T + 12, LIFE_DOT_GAP = 18;
function livesShown(dev: Device): number {
  const fb = dev.fbView();
  let n = 0;
  for (let i = 0; i < START_LIVES; i++) {
    if (isBlack(landPixel(fb, Math.round(LIFE_DOT_X0 + i * LIFE_DOT_GAP), Math.round(LIFE_DOT_Y)))) n++;
  }
  return n;
}
function wallInk(dev: Device): number {
  const fb = dev.fbView();
  let n = 0;
  for (let i = 0; i < PANEL_W * PANEL_H; i++) {
    const v = (fb[i * 2]! << 8) | fb[i * 2 + 1]!;
    const r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f;
    if (!(isWhite({ r, g, b }) || isBlack({ r, g, b }))) n++;
  }
  return n;
}

// ---- ball tracking, from the push stream - see header comment ----------
function ballPushLandX(pushes: Rect[]): number | null {
  let best: Rect | null = null;
  for (const p of pushes) {
    if (p.w <= 32 && p.h <= 32) { if (!best || p.w * p.h < best.w * best.h) best = p; }
  }
  return best ? best.y + best.h / 2 : null;
}
// Bricks were originally going to be counted the same way (a size filter on
// the push stream), but decision 0001's own alignment padding (align_for_push,
// breakout.c) inflates a brick's push up to ~48px on some sub-pixel
// alignments - measured directly (a one-off diagnostic script, not kept)
// against the real compiled game: real breaks were being missed whenever
// their aligned push landed outside a hand-picked size band, while a later
// REGROW wave's own per-brick pushes (identical size range) got counted as
// new breaks. wallInk() below is slower per call but exact, not a heuristic
// tuned against one build's own rounding - see runSurvival().

// ---- controllers ---------------------------------------------------------
interface Controller {
  reset(): void;
  observe(pushes: Rect[], nowMs: number): void;
  commandGx(nowMs: number): number;
}

// Perfect information, one tick of latency (whatever the previous tick's
// push stream revealed), zero smoothing of its own - see header comment.
class IdealizedController implements Controller {
  private trackedX = RESPAWN_X;
  reset() { this.trackedX = RESPAWN_X; }
  observe(pushes: Rect[], _nowMs: number) {
    const x = ballPushLandX(pushes);
    if (x !== null) this.trackedX = x;
  }
  commandGx(_nowMs: number): number {
    const frac = clamp((this.trackedX - PADDLE_CENTER_X) / PADDLE_TRAVEL_MAX, -1, 1);
    return frac * PADDLE_GX_FULL;
  }
}

// The idealized controller's own reasoning, but extrapolated forward by
// `leadMs` using the ball's own measured velocity between consecutive
// reads - see header comment's "ISOLATING THE BINDING CONSTRAINT".
// leadMs=0 degenerates to IdealizedController exactly.
class LeadCompensatedController implements Controller {
  private x = RESPAWN_X;
  private prevX = RESPAWN_X;
  private prevMs = -1;
  private vx = 0;
  constructor(private leadMs: number) {}
  reset() { this.x = RESPAWN_X; this.prevX = RESPAWN_X; this.prevMs = -1; this.vx = 0; }
  observe(pushes: Rect[], nowMs: number) {
    const x = ballPushLandX(pushes);
    if (x === null) return;
    if (this.prevMs >= 0) {
      const dt = nowMs - this.prevMs;
      if (dt > 0) this.vx = (x - this.prevX) / dt;
    }
    this.prevX = x; this.prevMs = nowMs; this.x = x;
  }
  commandGx(_nowMs: number): number {
    const predicted = this.x + this.vx * this.leadMs;
    const frac = clamp((predicted - PADDLE_CENTER_X) / PADDLE_TRAVEL_MAX, -1, 1);
    return frac * PADDLE_GX_FULL;
  }
}

interface SloppyOpts {
  reactionMs: number;      // delay before a correction can use a ball reading
  decisionPeriodMs: number; // how often a NEW voluntary correction is aimed
  positionErrorPx: number;  // +/- aim error, redrawn fresh each correction
  handRateGPerMs: number;   // how fast the commanded g can change (a puck, not a switch)
}
// Justification for every number lives in the header comment's "SLOPPY"
// paragraph and "WHAT THIS PROBE CANNOT VERIFY" - none of these are
// measurements.
const SLOPPY_DEFAULTS: SloppyOpts = {
  reactionMs: 350,
  decisionPeriodMs: 220,
  positionErrorPx: 40,
  handRateGPerMs: 0.8 / 500, // -0.4g to +0.4g in half a second, a fast deliberate swing
};

class SloppyController implements Controller {
  private rng: () => number;
  private history: { x: number; ms: number }[] = [];
  private lastDecisionMs = -Infinity;
  private aimX = RESPAWN_X;
  private handGx = 0;
  constructor(seed: number, private opts: SloppyOpts = SLOPPY_DEFAULTS) {
    this.rng = seededRng(seed);
  }
  reset() {
    this.history = []; this.lastDecisionMs = -Infinity; this.aimX = RESPAWN_X; this.handGx = 0;
  }
  observe(pushes: Rect[], nowMs: number) {
    const x = ballPushLandX(pushes);
    if (x === null) return;
    this.history.push({ x, ms: nowMs });
    // Trim anything older than a couple of reaction windows - this is a
    // lookup buffer, not a log.
    const horizon = this.opts.reactionMs * 3 + 200;
    while (this.history.length > 1 && nowMs - this.history[0]!.ms > horizon) this.history.shift();
  }
  private delayedX(nowMs: number): number {
    const targetMs = nowMs - this.opts.reactionMs;
    let best = this.history.length ? this.history[0]!.x : RESPAWN_X;
    for (const h of this.history) { if (h.ms <= targetMs) best = h.x; else break; }
    return best;
  }
  commandGx(nowMs: number): number {
    if (nowMs - this.lastDecisionMs >= this.opts.decisionPeriodMs) {
      this.lastDecisionMs = nowMs;
      const err = (this.rng() * 2 - 1) * this.opts.positionErrorPx;
      this.aimX = this.delayedX(nowMs) + err;
    }
    const desiredFrac = clamp((this.aimX - PADDLE_CENTER_X) / PADDLE_TRAVEL_MAX, -1, 1);
    const desiredGx = desiredFrac * PADDLE_GX_FULL;
    const maxStep = this.opts.handRateGPerMs * FRAME_MS;
    this.handGx += clamp(desiredGx - this.handGx, -maxStep, maxStep);
    return this.handGx;
  }
}

// ---- one survival run, any controller ------------------------------------
interface SurvivalResult {
  gameOverMs: number | null;
  lifeLostAtMs: number[];
  bricksBroken: number;
  clearedAtMs: number | null;
  initialInk: number;
  finalInk: number;
}

// "Cleared" is detected the way sim_step() (breakout.c) itself marks it,
// not by a hand-tuned ink threshold: the instant aliveCount hits 0 the ball
// FREEZES (celebrating: "the ball holds its position and velocity
// throughout") for CELEB_PAUSE_MS (400ms) before the regrow wave starts. A
// stretch of ticks with no ball push (ballPushLandX returning null) means
// the ball stopped moving; the only other way that happens is the ball
// being LOST (invisible during LIFE_LOST_FREEZE_MS, 550ms), which is
// already tracked separately via the lives row, so requiring "well clear of
// a recent life loss" tells the two apart with no ink-fraction guesswork.
const CLEAR_STALL_MS = 350; // comfortably past a fixed-step tick, short of CELEB_PAUSE_MS's own 400ms
const CLEAR_STALL_LIFE_LOSS_GUARD_MS = 900; // > LIFE_LOST_FREEZE_MS (550ms)

async function runSurvival(controller: Controller, ceilingMs = 180_000): Promise<SurvivalResult> {
  const dev = await loadDevice();
  let t = await enterBreakout(dev);
  controller.reset();
  const initialInk = wallInk(dev);
  let lastLives = START_LIVES;
  const lifeLostAtMs: number[] = [];
  let bricksBroken = 0;
  let clearedAtMs: number | null = null;
  let gameOverMs: number | null = null;
  let finalInk = initialInk;
  let msSinceBallMove = 0;

  while (t < ceilingMs && gameOverMs === null && clearedAtMs === null) {
    const gx = controller.commandGx(t);
    dev.tiltPanel(0, gx, Math.sqrt(Math.max(0, 1 - gx * gx)));
    t += FRAME_MS;
    dev.tick(t);
    const pushes = dev.pushes();
    controller.observe(pushes, t);

    finalInk = wallInk(dev);
    bricksBroken = clamp(Math.round((1 - finalInk / initialInk) * N_BRICKS), 0, N_BRICKS);

    if (ballPushLandX(pushes) !== null) msSinceBallMove = 0; else msSinceBallMove += FRAME_MS;
    const recentLifeLoss = lifeLostAtMs.length > 0 &&
      (t - lifeLostAtMs[lifeLostAtMs.length - 1]!) < CLEAR_STALL_LIFE_LOSS_GUARD_MS;
    if (msSinceBallMove >= CLEAR_STALL_MS && !recentLifeLoss && clearedAtMs === null) {
      clearedAtMs = t;
      bricksBroken = N_BRICKS;
    }

    const lives = livesShown(dev);
    if (lives < lastLives) { lifeLostAtMs.push(t); lastLives = lives; }
    if (lives === 0) gameOverMs = t;
  }
  return { gameOverMs, lifeLostAtMs, bricksBroken, clearedAtMs, initialInk, finalInk };
}

// ---- filter-only probes: step response and periodic tracking error,
// independent of the game entirely - see header comment. ------------------
async function measureStepResponse(target: number): Promise<void> {
  const dev = await loadDevice();
  await enterBreakout(dev);
  let t = FRAME_MS;
  // Settle at flat (gx=0) well past the resting corner's own tau (~177ms,
  // tilt.h) before the step.
  for (let i = 0; i < 200; i++) { dev.tiltPanel(0, 0, 1); t += FRAME_MS; dev.tick(t); }
  const checkpoints = [16, 50, 100, 150, 200, 300, 500, 800];
  const stepStart = t;
  let idx = 0;
  console.log(`  step 0 -> ${target.toFixed(2)}g (landscape gx):`);
  while (idx < checkpoints.length) {
    dev.tiltPanel(0, target, Math.sqrt(Math.max(0, 1 - target * target)));
    t += FRAME_MS;
    dev.tick(t);
    const elapsed = t - stepStart;
    if (elapsed >= checkpoints[idx]!) {
      const gx = dev.tiltGx();
      console.log(`    t=${elapsed.toFixed(0)}ms  gx=${gx.toFixed(3)}  (${((gx / target) * 100).toFixed(0)}% of target)`);
      idx++;
    }
  }
}

async function measureTrackingRms(halfPeriodMs: number, amp: number): Promise<{ rmsGx: number; rmsPx: number }> {
  const dev = await loadDevice();
  await enterBreakout(dev);
  let t = FRAME_MS;
  const periods = 6;
  const totalMs = halfPeriodMs * 2 * periods;
  let err2Sum = 0, n = 0;
  const settleUntil = halfPeriodMs * 2; // skip the first full period as transient
  while (t < totalMs) {
    const phase = (t % (halfPeriodMs * 2)) / halfPeriodMs; // 0..2
    const commanded = phase <= 1 ? -amp + 2 * amp * phase : amp - 2 * amp * (phase - 1);
    dev.tiltPanel(0, commanded, Math.sqrt(Math.max(0, 1 - commanded * commanded)));
    t += FRAME_MS;
    dev.tick(t);
    if (t > settleUntil) {
      const measured = dev.tiltGx();
      const e = measured - commanded;
      err2Sum += e * e; n++;
    }
  }
  const rmsGx = Math.sqrt(err2Sum / Math.max(1, n));
  const rmsPx = (rmsGx / PADDLE_GX_FULL) * PADDLE_TRAVEL_MAX;
  return { rmsGx, rmsPx };
}

function median(vals: number[]): number {
  const s = [...vals].sort((a, b) => a - b);
  const mid = Math.floor(s.length / 2);
  return s.length % 2 ? s[mid]! : (s[mid - 1]! + s[mid]!) / 2;
}

async function main() {
  console.log("=== breakout-difficulty: what actually binds, measured ===\n");

  // ---- geometry, stated plainly (arithmetic, no wasm - same convention
  // repro-breakout-paddle-hitbox.ts uses for its own claims) --------------
  console.log("-- geometry --");
  const fallDist = PADDLE_Y - PLAY_T; // 274px, top of field to the paddle's own line
  const lastBrickFallDist = PADDLE_Y - ROW2_PEAK_Y; // 159px, innermost row to the paddle line
  console.log(`  play field: ${PLAY_R - PLAY_L}x${PLAY_B - PLAY_T}px, paddle travel: ${2 * PADDLE_TRAVEL_MAX}px (full field width)`);
  console.log(`  paddle half-width: ${PADDLE_HALF_W}px (the static catch tolerance if tracking had zero error)`);
  console.log(`  ball speed: ${BALL_SPEED_BASE}-${BALL_SPEED_MAX}px/ms`);
  console.log(`  fastest possible vertical fall, full field: ${(fallDist / BALL_SPEED_MAX).toFixed(0)}-${(fallDist / BALL_SPEED_BASE).toFixed(0)}ms (top of field to paddle line, speed-only bound)`);
  console.log(`  fastest possible vertical fall, innermost row only: ${(lastBrickFallDist / BALL_SPEED_MAX).toFixed(0)}-${(lastBrickFallDist / BALL_SPEED_BASE).toFixed(0)}ms (last-bounce-to-arrival window can be this short)`);
  console.log();

  // ---- the shared filter's own step response, measured directly ----------
  console.log("-- tilt filter step response (real tilt.c, via emu_tilt) --");
  await measureStepResponse(PADDLE_GX_FULL);
  console.log();

  // ---- tracking error under a reversing target, at a few reversal rates --
  console.log("-- tilt filter tracking error under a periodic (triangle-wave) target --");
  for (const halfPeriodMs of [200, 400, 800, 1500]) {
    const { rmsGx, rmsPx } = await measureTrackingRms(halfPeriodMs, PADDLE_GX_FULL);
    console.log(`  half-period ${halfPeriodMs}ms (full reversal every ${halfPeriodMs}ms): RMS error ${rmsGx.toFixed(3)}g = ${rmsPx.toFixed(1)}px (paddle half-width is ${PADDLE_HALF_W}px)`);
  }
  console.log();

  // ---- the idealized controller, reactive, one tick of latency -----------
  console.log("-- idealized controller (perfect ball tracking, one tick of latency) --");
  const idealized = await runSurvival(new IdealizedController());
  console.log(`  lives lost at: [${idealized.lifeLostAtMs.join(", ")}]ms, game over at ${idealized.gameOverMs}ms`);
  console.log(`  bricks broken: ${idealized.bricksBroken}/${N_BRICKS}, cleared: ${idealized.clearedAtMs !== null ? `yes at ${idealized.clearedAtMs}ms` : "no"}`);
  console.log(`  wall ink: ${idealized.initialInk} -> ${idealized.finalInk}`);
  console.log();

  // ---- lead-compensated sweep: isolates the filter's own contribution ----
  console.log("-- lead-compensated idealized controller, sweeping the lead time --");
  console.log("   (lead=0 is the same controller as above; if survival/clearance improves");
  console.log("    sharply once lead approaches the filter's own settling time and then");
  console.log("    plateaus, the filter's lag is what was binding)");
  const leadResults: { leadMs: number; result: SurvivalResult }[] = [];
  for (const leadMs of [0, 75, 150, 225, 300, 450]) {
    const result = await runSurvival(new LeadCompensatedController(leadMs));
    leadResults.push({ leadMs, result });
    const survivedMs = result.gameOverMs ?? result.clearedAtMs ?? 180_000;
    console.log(`  lead=${leadMs}ms: survived ${survivedMs}ms, bricks ${result.bricksBroken}/${N_BRICKS}, cleared=${result.clearedAtMs !== null}, lives-lost=[${result.lifeLostAtMs.join(",")}]`);
  }
  console.log();

  // ---- the sloppy controller, seeded, many trials -------------------------
  console.log(`-- sloppy controller (a small child's hand, modelled) - ${SLOPPY_SEEDS} seeded trials --`);
  const sloppySurvivals: number[] = [];
  const sloppyBricks: number[] = [];
  for (let i = 0; i < SLOPPY_SEEDS; i++) {
    const seed = seedFromName(`breakout-difficulty-sloppy-${i}`);
    const result = await runSurvival(new SloppyController(seed));
    const survivedMs = result.gameOverMs ?? result.clearedAtMs ?? 180_000;
    sloppySurvivals.push(survivedMs);
    sloppyBricks.push(result.bricksBroken);
  }
  const medSurvival = median(sloppySurvivals);
  const medBricks = median(sloppyBricks);
  console.log(`  survival ms: min=${Math.min(...sloppySurvivals)} median=${medSurvival} max=${Math.max(...sloppySurvivals)}`);
  console.log(`  bricks broken: min=${Math.min(...sloppyBricks)} median=${medBricks} max=${Math.max(...sloppyBricks)}`);
  console.log();

  // ---- the assertions: red before a fix, green after one ------------------
  console.log("-- playability targets (see header comment for why these numbers) --");
  check(`idealized controller survives >= ${IDEALIZED_TARGET_MS}ms`,
    (idealized.gameOverMs === null || idealized.gameOverMs >= IDEALIZED_TARGET_MS),
    `game over at ${idealized.gameOverMs}ms`);
  check(`idealized controller clears the full wall (18/18 bricks) without running out of lives`,
    idealized.clearedAtMs !== null,
    `bricks broken: ${idealized.bricksBroken}/${N_BRICKS}, game over at ${idealized.gameOverMs}ms`);
  check(`sloppy controller's median survival >= ${SLOPPY_TARGET_MEDIAN_MS}ms across ${SLOPPY_SEEDS} seeds`,
    medSurvival >= SLOPPY_TARGET_MEDIAN_MS,
    `median=${medSurvival}ms`);
  check(`sloppy controller's median bricks broken >= ${SLOPPY_TARGET_MEDIAN_BRICKS}`,
    medBricks >= SLOPPY_TARGET_MEDIAN_BRICKS,
    `median=${medBricks}`);

  console.log(`\n${passCount} passed, ${failCount} failed`);
  if (failCount > 0) process.exit(1);
}

main();
