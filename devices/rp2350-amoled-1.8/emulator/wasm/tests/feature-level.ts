// feature-level: what the bubble level (firmware/apps/level.c) is supposed
// to do, asserted against the REAL compiled firmware. Run with:
//
//   bun run emulator/wasm/build.ts
//   bun run emulator/wasm/tests/feature-level.ts
//
// The level is in the default app table unconditionally (runtime_core.c,
// docs/decisions/0012): the two things that used to keep it out (the menu
// could not draw a fifth column, and there was no published orientation
// signal for it to read) are both gone now, so no build flag is needed.
//
// This is the `feature-*` half of the pair this directory's convention asks
// for: clean input, a readable statement of intent. The `repro-*` half is
// repro-level-bubble-residue.ts, which drives the same app through motion
// and guards the defect CLASS a moving object over a static background
// keeps producing here (residue, and pixels changing outside a pushed
// rectangle). There is no touch-dropout partner, deliberately: this app
// reads no touch at all.
//
// Every assertion is on the framebuffer, never on an internal, so anything
// that fails here is something a person looking at the device could also
// have seen.
//
// HOW TILT IS DRIVEN. emu_sensor_vector() (emu_abi.h) injects a gravity
// vector in PANEL axes, g, "+1 on an axis means that axis points at the
// sky", into the ONE shared "gravity" sensor every orientation-aware app on
// this device now reads through (firmware/runtime/tilt.h). The shim clamps
// and quantises it to what a QMI8658 at +-8g could actually report before
// the firmware's own filter ever sees it, so nothing below passes on a
// reading the hardware could not produce; that filter (tilt.c, ported here
// from this app's own original one) and the runtime's panel-to-landscape
// rotation both sit between what this file sends and what level.c reads.
//
// WHAT THIS CANNOT CHECK, and it is the same gap sensors.h's own injection
// comments insist on naming: nothing here proves how the real part is
// soldered. `device_to_panel()` in tilt.c is a HYPOTHESIS (identity) until
// the on-board axis ritual settles it - see tilt.h. So these tests pin the
// app's behaviour given a direction, and say nothing about which physical
// tip produces that direction. Only a person tilting the real board answers
// that.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const LAND_H = PANEL_W; // 368
// g_apps[] = { chrono, sketch("draw"), timer, four, level }. Read from
// emu_device()'s own app list rather than hardcoded, so this file cannot
// silently drive the wrong app if the table order ever changes.
let APP_LEVEL = -1;

// level.c's geometry, lifted rather than re-derived - the same convention
// every other test in this directory uses against its app.
const CX = 224, CY = 184;
const RIM_R = 168, RIM_HALF = 3.5, RIM_GAP = 4;
const BUBBLE_R = 30, BUBBLE_R_LEVEL = 32;
const TRAVEL_MAX = RIM_R - RIM_HALF - BUBBLE_R - RIM_GAP; // 130.5
const FULL_SCALE_DEG = 15;
const ENTER_DEG = 3.0, EXIT_DEG = 3.6, LOST_DEG = 55;
const TARGET_INNER = TRAVEL_MAX * (ENTER_DEG / FULL_SCALE_DEG) + BUBBLE_R; // 56.1
const T_IDLE = 4, T_LEVEL = 10;
const BEZEL = 10; // gfx.h's PANEL_BEZEL_MARGIN_PX

const FRAME_MS = 16;

// How long a tilt is held before the picture is judged. The shared filter
// (firmware/runtime/tilt.c) settles a step change well under this by
// design, and the level verdict needs another 250 ms of dwell on top, so
// this is comfortably past both without making the suite crawl - every
// sqrtf in the drawing loop is a host import (emu_abi.h), so a tick here
// costs far more than a tick on the board ever will. Longer than the
// filter's own resting time constant (tilt.h's TILT_FC_MIN_HZ, ~177 ms)
// would suggest on its own, because the filter is now continuously live
// from the moment the module starts (emu_tick() feeds it every frame,
// whichever app is on screen) rather than seeded fresh the instant this
// app's own test starts feeding it a target.
const SETTLE_MS = 2500;

// The centroid of a disc centred at a geometric x lands on pixel INDEX
// x - 0.5, because a pixel's centre is at index + 0.5. Not an off-by-one:
// the two coordinate spaces genuinely differ by half a pixel, and every
// comparison below converts rather than absorbing it into a tolerance.
const PIXEL_CENTRE_BIAS = 0.5;

// A 13x13 box fits inside a disc of radius R only while its centre is
// within R - sqrt(6^2 + 6^2) of the disc's own centre, so erosion shrinks a
// dot by its box's DIAGONAL half-width, not by 6.
const ERODE_SHRINK = Math.sqrt(72); // 8.485

let passCount = 0, failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
  if (ok) passCount++; else failCount++;
  console.log(`[${ok ? "PASS" : "FAIL"}] ${label}${detail ? " - " + detail : ""}`);
}

async function loadDevice() {
  let memory!: WebAssembly.Memory;
  const dec = new TextDecoder();
  const log: string[] = [];
  const inst = await WebAssembly.instantiate(await WebAssembly.compile(readFileSync(WASM_PATH)), {
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
  return {
    exports: e,
    tick(nowMs: number) { e.emu_tick(nowMs); },
    // x/y/z here are milli-g (this file's own gravityFor() convention);
    // emu_sensor_vector() (emu_abi.h) wants g, and index 1 is the shared
    // "gravity" sensor every orientation-aware app now reads through
    // (emu_device()'s sensors array: 0 "shake", 1 "gravity").
    tilt(x: number, y: number, z: number) { e.emu_sensor_vector(1, x / 1000, y / 1000, z / 1000); },
    appSwitch(i: number) { e.emu_app_switch(i); },
    appCurrent(): number { return e.emu_app_current(); },
    fb(): Uint8Array { return new Uint8Array(memory.buffer, e.emu_fb(), PANEL_W * PANEL_H * 2).slice(); },
    device(): any {
      const ptr = e.emu_device();
      const bytes = new Uint8Array(memory.buffer, ptr);
      let end = 0; while (bytes[end] !== 0) end++;
      return JSON.parse(dec.decode(bytes.subarray(0, end)));
    },
    log,
  };
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

// px_to_gray (gfx.h), in TypeScript. The framebuffer holds byte-swapped
// RGB565 and the 6-bit green channel is this device's ink level, so pure
// white reads back as 252 rather than 255 - that is the format, not a
// rounding bug.
function grayAt(fb: Uint8Array, lx: number, ly: number): number {
  if (lx < 0 || ly < 0 || lx >= LAND_W || ly >= LAND_H) return 252;
  const px = PANEL_W - 1 - ly, py = lx;
  const i = (py * PANEL_W + px) * 2;
  const v = (fb[i]! << 8) | fb[i + 1]!;
  return ((v >> 5) & 0x3f) << 2;
}

const isBlack = (g: number) => g <= 24;
const isWhite = (g: number) => g >= 248;

// The dot's centre, found by ERODING the black mask rather than by taking a
// centroid of everything dark. A centroid would be pulled about by the rim
// and by the target ring whenever the dot overlaps one of them, which it
// does exactly at the tolerance boundary this file most wants to measure.
// Erosion by a 13x13 box deletes every stroke on the dial (the rim is 7 px
// thick, the closed ring 10) and leaves only the interior of the 60 px dot,
// whose centroid is then unbiased by construction. Implemented over a
// summed-area table so it stays O(pixels) rather than O(pixels * 169).
function bubbleCentre(fb: Uint8Array): { x: number; y: number; n: number } {
  const W = LAND_W, H = LAND_H;
  const sat = new Int32Array((W + 1) * (H + 1));
  for (let y = 0; y < H; y++) {
    let rowSum = 0;
    for (let x = 0; x < W; x++) {
      rowSum += isBlack(grayAt(fb, x, y)) ? 1 : 0;
      sat[(y + 1) * (W + 1) + (x + 1)] = sat[y * (W + 1) + (x + 1)] + rowSum;
    }
  }
  const box = (x0: number, y0: number, x1: number, y1: number) =>
    sat[(y1 + 1) * (W + 1) + (x1 + 1)]! - sat[y0 * (W + 1) + (x1 + 1)]!
    - sat[(y1 + 1) * (W + 1) + x0]! + sat[y0 * (W + 1) + x0]!;

  const K = 6; // half-width of the erosion box: 13x13 = 169 pixels
  let sx = 0, sy = 0, n = 0;
  for (let y = K; y < H - K; y++) {
    for (let x = K; x < W - K; x++) {
      if (box(x - K, y - K, x + K, y + K) !== (2 * K + 1) * (2 * K + 1)) continue;
      sx += x; sy += y; n++;
    }
  }
  return n === 0 ? { x: NaN, y: NaN, n: 0 } : { x: sx / n, y: sy / n, n };
}

// The verdict, read off the picture the way a person reads it: is the
// target ring absent, a grey hairline, or closed and black?
//
// Measured over whole annular bands rather than at one sampled point, so
// the dot wandering across the ring cannot change the answer: the closed
// ring's own outer half (61..65) is a band the idle ring never reaches
// (it ends at 60.1), and the dot can only ever clip a small wedge of it.
type Ring = "hidden" | "idle" | "level";
function ringState(fb: Uint8Array): { state: Ring; blackOuter: number; greyInner: number } {
  let blackOuter = 0, greyInner = 0;
  const scan = 70;
  for (let ly = CY - scan; ly <= CY + scan; ly++) {
    for (let lx = CX - scan; lx <= CX + scan; lx++) {
      const dx = lx - CX, dy = ly - CY;
      const r = Math.sqrt(dx * dx + dy * dy);
      const g = grayAt(fb, lx, ly);
      if (r >= 61 && r <= 65 && isBlack(g)) blackOuter++;
      if (r >= 57 && r <= 59 && !isBlack(g) && !isWhite(g)) greyInner++;
    }
  }
  const state: Ring = blackOuter > 800 ? "level" : greyInner > 400 ? "idle" : "hidden";
  return { state, blackOuter, greyInner };
}

// A gravity vector, panel axes, milli-g, for a tilt of `deg` from flat
// whose in-plane part points along panel-axis angle `phiDeg` (0 = panel +x,
// 90 = panel +y).
function gravityFor(deg: number, phiDeg: number): [number, number, number] {
  const t = (deg * Math.PI) / 180, p = (phiDeg * Math.PI) / 180;
  return [1000 * Math.sin(t) * Math.cos(p), 1000 * Math.sin(t) * Math.sin(p), 1000 * Math.cos(t)];
}

// Where level.c should put the dot for that same tilt. Panel +x maps to
// landscape -y and panel +y to landscape +x (gfx.h's rotation, inverted for
// a direction), and the dot runs DOWNHILL, so it is the negation of the
// in-plane reading.
function expectedBubble(deg: number, phiDeg: number): [number, number] {
  const p = (phiDeg * Math.PI) / 180;
  const ux = -Math.sin(p), uy = Math.cos(p);
  const travel = TRAVEL_MAX * Math.min(1, Math.max(0, deg / FULL_SCALE_DEG));
  return [CX + ux * travel, CY + uy * travel];
}

// Holds a tilt for `ms` of 16 ms frames, which is what lets the filter
// settle. Returns the clock it left off at.
function hold(dev: Device, t: number, ms: number, g: [number, number, number]): number {
  const end = t + ms;
  while (t < end) {
    dev.tilt(g[0], g[1], g[2]);
    t += FRAME_MS;
    dev.tick(t);
  }
  return t;
}

async function enterLevel(): Promise<{ dev: Device; t: number }> {
  const dev = await loadDevice();
  dev.tick(0);
  dev.appSwitch(APP_LEVEL);
  dev.tick(FRAME_MS);
  return { dev, t: FRAME_MS };
}

async function main() {
  console.log("=== feature: the bubble level ===\n");

  // ---- the device declares the shared continuous gravity sensor ---------
  {
    const dev = await loadDevice();
    const d = dev.device();
    const gravity = (d.sensors || []).find((s: any) => s.id === "gravity");
    check("emu_device() declares a continuous 'gravity' sensor", !!gravity && gravity.kind === "gravity",
      JSON.stringify(d.sensors));

    APP_LEVEL = (d.apps || []).indexOf("level");
    check("the app table carries 'level'", APP_LEVEL >= 0, `index ${APP_LEVEL} of ${JSON.stringify(d.apps)}`);
    if (APP_LEVEL < 0) {
      console.log(`\n${passCount} passed, ${failCount} failed`);
      process.exit(1);
    }
  }

  // ---- flat: the dot comes home and the ring closes ---------------------
  {
    const { dev, t: t0 } = await enterLevel();
    check("switched into level", dev.appCurrent() === APP_LEVEL, `app_current()=${dev.appCurrent()}`);

    let t = hold(dev, t0, SETTLE_MS, gravityFor(0, 0));
    const fb = dev.fb();
    const b = bubbleCentre(fb);
    const off = Math.hypot(b.x + PIXEL_CENTRE_BIAS - CX, b.y + PIXEL_CENTRE_BIAS - CY);
    check("held flat, the dot sits at the centre of the dial", off < 0.35,
      `centre=(${b.x.toFixed(2)}, ${b.y.toFixed(2)}), off by ${off.toFixed(2)}px, n=${b.n}`);

    const r = ringState(fb);
    check("held flat, the target ring closes: black, not grey", r.state === "level",
      `state=${r.state} blackOuter=${r.blackOuter} greyInner=${r.greyInner}`);

    // The settled dot is the bigger one (BUBBLE_R_LEVEL, 32 rather than 30),
    // which after erosion is the difference between ~23.5 px and ~21.5 px of
    // measured radius.
    const eroded = Math.sqrt(b.n / Math.PI);
    const wantEroded = BUBBLE_R_LEVEL - ERODE_SHRINK;
    check("the dot swells when it settles", Math.abs(eroded - wantEroded) < 1.0,
      `eroded radius ${eroded.toFixed(2)}px, expected ${wantEroded.toFixed(2)} (an unsettled dot would give ${(BUBBLE_R - ERODE_SHRINK).toFixed(2)})`);

    // Nothing may be drawn where the case hides it.
    let worstR = 0;
    for (let ly = 0; ly < LAND_H; ly++) {
      for (let lx = 0; lx < LAND_W; lx++) {
        if (isWhite(grayAt(fb, lx, ly))) continue;
        const r2 = Math.hypot(lx - CX, ly - CY);
        if (r2 > worstR) worstR = r2;
      }
    }
    check("nothing is drawn inside the bezel band", worstR <= LAND_H / 2 - BEZEL,
      `furthest ink is ${worstR.toFixed(1)}px from centre, the case hides past ${LAND_H / 2 - BEZEL}`);
    void t;
  }

  // ---- direction: four tips, four answers -------------------------------
  {
    for (const phi of [0, 90, 180, 270]) {
      const { dev, t: t0 } = await enterLevel();
      hold(dev, t0, SETTLE_MS, gravityFor(FULL_SCALE_DEG, phi));
      const b = bubbleCentre(dev.fb());
      const [ex, ey] = expectedBubble(FULL_SCALE_DEG, phi);
      const err = Math.hypot(b.x + PIXEL_CENTRE_BIAS - ex, b.y + PIXEL_CENTRE_BIAS - ey);
      check(`tilted 15deg toward panel-axis ${phi}deg, the dot is downhill at the rim`, err < 0.6,
        `dot=(${b.x.toFixed(1)}, ${b.y.toFixed(1)}) expected=(${ex.toFixed(1)}, ${ey.toFixed(1)}) err=${err.toFixed(2)}px`);
    }
  }

  // ---- the dot's travel is the tilt, all the way to saturation ----------
  {
    for (const deg of [2, 5, 10, 15, 30]) {
      const { dev, t: t0 } = await enterLevel();
      hold(dev, t0, SETTLE_MS, gravityFor(deg, 90));
      const b = bubbleCentre(dev.fb());
      const [ex, ey] = expectedBubble(deg, 90);
      const err = Math.hypot(b.x + PIXEL_CENTRE_BIAS - ex, b.y + PIXEL_CENTRE_BIAS - ey);
      const travel = Math.hypot(b.x + PIXEL_CENTRE_BIAS - CX, b.y + PIXEL_CENTRE_BIAS - CY);
      check(`at ${deg}deg the dot is ${(TRAVEL_MAX * Math.min(1, deg / FULL_SCALE_DEG)).toFixed(1)}px out`,
        err < 0.6, `measured ${travel.toFixed(1)}px, err=${err.toFixed(2)}px`);
    }
  }

  // ---- the tolerance band is where it was said to be --------------------
  {
    // Just inside: level. Just outside: not level. Approached from
    // outside both times, so the hysteresis is not doing the work.
    for (const [deg, want] of [[2.5, "level"], [3.4, "idle"]] as [number, Ring][]) {
      const { dev, t: t0 } = await enterLevel();
      let t = hold(dev, t0, SETTLE_MS, gravityFor(10, 90)); // start well off
      t = hold(dev, t, SETTLE_MS, gravityFor(deg, 90));
      const r = ringState(dev.fb());
      check(`coming from 10deg, ${deg}deg reads "${want}" (the band is ${ENTER_DEG}deg)`,
        r.state === want, `state=${r.state} blackOuter=${r.blackOuter} greyInner=${r.greyInner}`);
    }
  }

  // ---- hysteresis: it leaves later than it arrives ----------------------
  {
    const { dev, t: t0 } = await enterLevel();
    let t = hold(dev, t0, SETTLE_MS, gravityFor(10, 90));
    const before = ringState(dev.fb()).state;
    t = hold(dev, t, SETTLE_MS, gravityFor(2.0, 90));
    const settled = ringState(dev.fb()).state;
    // 3.3 is past the entry angle (3.0) and short of the exit angle (3.6):
    // reachable only by having been level already.
    t = hold(dev, t, SETTLE_MS, gravityFor(3.3, 90));
    const held = ringState(dev.fb()).state;
    check("hysteresis: 3.3deg keeps a verdict it could not have started",
      before === "idle" && settled === "level" && held === "level",
      `10deg=${before} -> 2.0deg=${settled} -> 3.3deg=${held} (enter ${ENTER_DEG}, exit ${EXIT_DEG})`);

    // ...and past the exit angle it does give it up.
    t = hold(dev, t, SETTLE_MS, gravityFor(5.0, 90));
    check("past the exit angle the verdict is withdrawn", ringState(dev.fb()).state === "idle");
  }

  // ---- the dwell: sweeping through flat does not flash "level" ----------
  {
    const { dev, t: t0 } = await enterLevel();
    let t = hold(dev, t0, SETTLE_MS, gravityFor(8, 90));
    // 8deg to -8deg in 150ms, i.e. straight through the middle at a speed
    // a hand can genuinely produce. The band is 6deg wide out of 16, so
    // roughly 55ms of that pass is spent inside it - well under the 250ms
    // dwell, so the ring must never close.
    let flashed = false;
    const steps = 10;
    for (let i = 1; i <= steps; i++) {
      const deg = 8 - (16 * i) / steps;
      const g = gravityFor(Math.abs(deg), deg >= 0 ? 90 : 270);
      dev.tilt(g[0], g[1], g[2]);
      t += 15;
      dev.tick(t);
      if (ringState(dev.fb()).state === "level") flashed = true;
    }
    check("a sweep straight through flat never flashes the level cue", !flashed);
    // ...but stopping there does close it.
    t = hold(dev, t, SETTLE_MS, gravityFor(0, 90));
    check("stopping at flat after the sweep does close the ring",
      ringState(dev.fb()).state === "level");
  }

  // ---- held on edge: "level" stops meaning anything ---------------------
  {
    const { dev, t: t0 } = await enterLevel();
    let t = hold(dev, t0, SETTLE_MS, gravityFor(0, 0));
    check("precondition: it was level before being stood up",
      ringState(dev.fb()).state === "level");
    t = hold(dev, t, SETTLE_MS, gravityFor(90, 90)); // straight up on its edge
    const fb = dev.fb();
    const r = ringState(fb);
    check(`held on edge (past ${LOST_DEG}deg) the target ring is removed entirely`,
      r.state === "hidden", `state=${r.state} blackOuter=${r.blackOuter} greyInner=${r.greyInner}`);
    const b = bubbleCentre(fb);
    const travel = Math.hypot(b.x + PIXEL_CENTRE_BIAS - CX, b.y + PIXEL_CENTRE_BIAS - CY);
    check("...and the dot is parked against the rim", Math.abs(travel - TRAVEL_MAX) < 0.6,
      `travel=${travel.toFixed(1)}px, rim stop is ${TRAVEL_MAX}`);
    // ...and it comes back.
    t = hold(dev, t, SETTLE_MS, gravityFor(0, 0));
    check("laying it flat again brings the ring back, closed",
      ringState(dev.fb()).state === "level");
  }

  // ---- a still puck costs nothing ---------------------------------------
  {
    const { dev, t: t0 } = await enterLevel();
    let t = hold(dev, t0, SETTLE_MS, gravityFor(6, 45));
    let pushes = 0;
    for (let i = 0; i < 60; i++) {
      dev.tilt(...gravityFor(6, 45));
      t += FRAME_MS;
      dev.tick(t);
      pushes += dev.exports.emu_push_count();
    }
    check("a perfectly still device pushes nothing at all", pushes === 0, `${pushes} pushes over 60 frames`);
  }

  console.log(`\n${passCount} passed, ${failCount} failed`);
  if (failCount > 0) process.exit(1);
}

main();
