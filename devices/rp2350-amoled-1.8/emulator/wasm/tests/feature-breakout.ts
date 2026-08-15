// feature-breakout: what firmware/apps/breakout.c is supposed to do,
// asserted against the REAL compiled firmware. Run with:
//
//   bun run emulator/wasm/build.ts
//   bun run emulator/wasm/tests/feature-breakout.ts
//
// Breakout is in the default app table unconditionally, the same as
// firmware/apps/level.c (docs/decisions/0012): it reads app_frame_t.tilt
// like every other app and touches no hardware of its own, so no build flag
// is needed - see breakout.c's own header comment for the whole argument.
//
// This is the `feature-*` half of the pair this directory's convention asks
// for (AGENTS.md's "Regression tests"): clean input, a readable statement of
// intent. The `repro-*` half is repro-breakout-residue.ts, which audits a
// long continuous run for the anti-residue invariant and pins the
// fixed-timestep clock's determinism. There is no touch-dropout partner,
// deliberately: like level.c, this app reads no touch at all.
//
// Every assertion is on the framebuffer, never on an internal, so anything
// that fails here is something a person looking at the device could also
// have seen.
//
// HOW TILT IS DRIVEN. emu_sensor_vector() (emu_abi.h) injects a gravity
// vector in PANEL axes, g, into the ONE shared "gravity" sensor every
// orientation-aware app reads through (firmware/runtime/tilt.h). The shim
// clamps and quantises it to what a QMI8658 at +-8g could actually report,
// so nothing below passes on a reading the hardware could not produce.
//
// WHAT THIS CANNOT CHECK, the same gap every tilt-driven test in this tree
// names: nothing here proves how the real part is soldered.
// device_to_panel() in tilt.c is a HYPOTHESIS (identity) until the on-board
// axis ritual settles it - see tilt.h. So this pins the app's behaviour
// given a direction, and says nothing about which physical tip produces
// that direction. Only a person tilting the real board answers that.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const LAND_H = PANEL_W; // 368

let APP_BREAKOUT = -1;

// Lifted from breakout.c's own #define block - the same convention every
// other test in this directory uses against its app (see feature-level.ts).
const PLAY_L = 26, PLAY_R = LAND_W - 26, PLAY_T = 26, PLAY_B = LAND_H - 26;
const BALL_R = 9;
const PADDLE_Y = 300, PADDLE_HALF_W = 46;
const PADDLE_CENTER_X = (PLAY_L + PLAY_R) / 2; // 224
const PADDLE_TRAVEL_MAX = (PLAY_R - PLAY_L) / 2 - PADDLE_HALF_W; // 152
const PADDLE_GX_FULL = 0.4;
const N_BRICKS = 10;
const BEZEL = 10; // gfx.h's PANEL_BEZEL_MARGIN_PX

const FRAME_MS = 16;

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
    tilt(x: number, y: number, z: number) { e.emu_sensor_vector(1, x, y, z); }, // g, panel axes
    appSwitch(i: number) { e.emu_app_switch(i); },
    appCurrent(): number { return e.emu_app_current(); },
    fb(): Uint8Array { return new Uint8Array(memory.buffer, e.emu_fb(), PANEL_W * PANEL_H * 2).slice(); },
    pushes(): { x: number; y: number; w: number; h: number }[] {
      const n = e.emu_push_count();
      const out = [];
      for (let i = 0; i < n; i++) out.push({ x: e.emu_push_x(i), y: e.emu_push_y(i), w: e.emu_push_w(i), h: e.emu_push_h(i) });
      return out;
    },
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

// px_to_gray/px_swap (gfx.h) round trip, in TypeScript, plus RGB565 channels.
function landPixel(fb: Uint8Array, lx: number, ly: number): { r: number; g: number; b: number } {
  const px = PANEL_W - 1 - ly, py = lx;
  const i = (py * PANEL_W + px) * 2;
  const swapped = (fb[i]! << 8) | fb[i + 1]!;
  const v = ((swapped & 0xff) << 8) | (swapped >> 8); // px_swap back to the packed RGB565
  return { r: (v >> 11) & 0x1f, g: (v >> 5) & 0x3f, b: v & 0x1f };
}
const isWhite = (p: { r: number; g: number; b: number }) => p.r >= 30 && p.g >= 60 && p.b >= 30;
const isBlack = (p: { r: number; g: number; b: number }) => p.r <= 1 && p.g <= 1 && p.b <= 1;

// The paddle is the WIDEST contiguous run of black pixels on the exact
// PADDLE_Y scanline. The ball is also black, but at most BALL_R*2+3 pixels
// wide on any single scanline, far short of the paddle's own ~92px core, so
// this is robust to the ball crossing that same row at the same instant.
function paddleCentre(fb: Uint8Array): number | null {
  let bestLen = 0, bestMid = -1;
  let runStart = -1;
  for (let lx = 0; lx <= LAND_W; lx++) {
    const black = lx < LAND_W && isBlack(landPixel(fb, lx, PADDLE_Y));
    if (black && runStart < 0) runStart = lx;
    if (!black && runStart >= 0) {
      const len = lx - runStart;
      if (len > bestLen) { bestLen = len; bestMid = (runStart + lx - 1) / 2; }
      runStart = -1;
    }
  }
  return bestLen >= 40 ? bestMid : null; // 40: comfortably wider than the ball, well under the paddle
}

// The ball's centroid, found only from black pixels CLEAR of the paddle's
// own vertical band (|ly - PADDLE_Y| > 25) - the one place a black pixel is
// unambiguously the ball rather than possibly the paddle.
function ballCentreAwayFromPaddle(fb: Uint8Array): { x: number; y: number; n: number } {
  let sx = 0, sy = 0, n = 0;
  for (let ly = 0; ly < LAND_H; ly++) {
    if (Math.abs(ly - PADDLE_Y) <= 25) continue;
    for (let lx = 0; lx < LAND_W; lx++) {
      if (!isBlack(landPixel(fb, lx, ly))) continue;
      sx += lx; sy += ly; n++;
    }
  }
  return n === 0 ? { x: NaN, y: NaN, n: 0 } : { x: sx / n, y: sy / n, n };
}

function countBrickInk(fb: Uint8Array): number {
  let n = 0;
  for (let ly = 0; ly < LAND_H; ly++) {
    for (let lx = 0; lx < LAND_W; lx++) {
      const p = landPixel(fb, lx, ly);
      if (!isWhite(p) && !isBlack(p)) n++;
    }
  }
  return n;
}

async function enterBreakout(): Promise<{ dev: Device; t: number }> {
  const dev = await loadDevice();
  dev.tick(0);
  dev.appSwitch(APP_BREAKOUT);
  dev.tick(FRAME_MS);
  return { dev, t: FRAME_MS };
}

function run(dev: Device, t: number, ms: number, tilt: [number, number, number] | null): number {
  const end = t + ms;
  while (t < end) {
    if (tilt) dev.tilt(tilt[0], tilt[1], tilt[2]);
    t += FRAME_MS;
    dev.tick(t);
  }
  return t;
}

async function main() {
  console.log("=== feature: breakout ===\n");

  // ---- the app table carries it, on the shared gravity sensor -----------
  {
    const dev = await loadDevice();
    const d = dev.device();
    APP_BREAKOUT = (d.apps || []).indexOf("breakout");
    check("the app table carries 'breakout'", APP_BREAKOUT >= 0, `index ${APP_BREAKOUT} of ${JSON.stringify(d.apps)}`);
    const gravity = (d.sensors || []).find((s: any) => s.id === "gravity");
    check("emu_device() declares the shared continuous 'gravity' sensor", !!gravity && gravity.kind === "gravity");
    if (APP_BREAKOUT < 0) { console.log(`\n${passCount} passed, ${failCount} failed`); process.exit(1); }
  }

  // ---- entering the app lands in it, and paints something --------------
  {
    const { dev } = await enterBreakout();
    check("switched into breakout", dev.appCurrent() === APP_BREAKOUT, `app_current()=${dev.appCurrent()}`);
    const ink = countBrickInk(dev.fb());
    check("the wall is on screen at entry", ink > 1000, `${ink} coloured pixels`);
  }

  // ---- the paddle follows tilt.gx, clamped at the rails ------------------
  //
  // emu_sensor_vector() injects PANEL-axis gravity (emu_abi.h), and
  // runtime_core.c's tilt_for_app() rotates that into a LANDSCAPE app's own
  // space before breakout.c ever sees it: landscape gx comes from PANEL gy,
  // not panel gx (see runtime_core.c's own derivation, "dlx = dpy"). So to
  // put a given value on app_frame_t.tilt.gx, this feeds it as the panel
  // vector's Y component - the same rotation feature-level.ts's
  // gravityFor()/expectedBubble() pair already has to account for.
  {
    for (const wantGx of [-1.0, -0.4, -0.2, 0, 0.2, 0.4, 1.0]) {
      const { dev, t: t0 } = await enterBreakout();
      const panelY = wantGx, panelZ = Math.sqrt(Math.max(0, 1 - wantGx * wantGx));
      run(dev, t0, 1200, [0, panelY, panelZ]);
      const px = paddleCentre(dev.fb());
      const frac = Math.max(-1, Math.min(1, wantGx / PADDLE_GX_FULL));
      const want = PADDLE_CENTER_X + frac * PADDLE_TRAVEL_MAX;
      check(`landscape gx=${wantGx.toFixed(2)}: the paddle centres near x=${want.toFixed(1)}`,
        px !== null && Math.abs(px - want) < 3.0,
        `measured ${px === null ? "not found" : px.toFixed(1)}`);
    }
  }

  // ---- coasting: the paddle FREEZES rather than following a lie ---------
  // A magnitude far from 1g (tilt.c's TILT_TRUST_NONE_G=0.4g and past it)
  // means the puck is being carried, not held still - see breakout.c's own
  // header comment on why reading tilt.gx directly, with no second filter,
  // makes this happen for free. Settle at a known gx first, then feed a
  // wildly different but implausible (multi-g) reading and confirm the
  // paddle does not move to chase it.
  {
    const { dev, t: t0 } = await enterBreakout();
    let t = run(dev, t0, 1200, [0, -0.3, Math.sqrt(1 - 0.09)]); // landscape gx=-0.3: see the panel/landscape rotation note above
    const before = paddleCentre(dev.fb());
    check("precondition: the paddle is off-centre before the carry", before !== null && Math.abs(before - PADDLE_CENTER_X) > 5,
      `before=${before}`);
    // A puck being carried: several g, nowhere near a resting 1g magnitude.
    t = run(dev, t, 600, [3.0, 2.0, 1.0]);
    const during = paddleCentre(dev.fb());
    check("mid-carry, the paddle holds its last position rather than lurching toward the implausible reading",
      during !== null && before !== null && Math.abs(during - before) < 1.0,
      `before=${before?.toFixed(2)} during=${during?.toFixed(2)}`);
  }

  // ---- the ball never leaves the play field: no floor to lose it through -
  {
    const { dev, t: t0 } = await enterBreakout();
    let t = t0;
    let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
    let samples = 0;
    for (let i = 0; i < 2000; i++) {
      t += FRAME_MS;
      dev.tick(t);
      const b = ballCentreAwayFromPaddle(dev.fb());
      if (b.n === 0) continue; // ball is transiently inside the paddle's own band
      samples++;
      if (b.x < minX) minX = b.x;
      if (b.x > maxX) maxX = b.x;
      if (b.y < minY) minY = b.y;
      if (b.y > maxY) maxY = b.y;
    }
    check("the ball's x stays within the play field across 2000 frames (no touch, no tilt)",
      samples > 0 && minX >= PLAY_L - BALL_R - 2 && maxX <= PLAY_R + BALL_R + 2,
      `x range [${minX.toFixed(1)}, ${maxX.toFixed(1)}], field [${PLAY_L},${PLAY_R}], ${samples} samples`);
    check("the ball's y stays within the play field across 2000 frames, INCLUDING the bottom - there is no floor to fall through",
      samples > 0 && minY >= PLAY_T - BALL_R - 2 && maxY <= PLAY_B + BALL_R + 2,
      `y range [${minY.toFixed(1)}, ${maxY.toFixed(1)}], field [${PLAY_T},${PLAY_B}], ${samples} samples`);
  }

  // ---- nothing is ever drawn inside the bezel band -----------------------
  {
    const { dev, t: t0 } = await enterBreakout();
    let t = t0;
    let worstViolation = 0;
    for (let i = 0; i < 300; i++) {
      t += FRAME_MS;
      dev.tick(t);
      if (i % 20 !== 0) continue;
      const fb = dev.fb();
      for (let ly = 0; ly < LAND_H; ly++) {
        const edgeRow = ly < BEZEL || ly >= LAND_H - BEZEL;
        const xs = edgeRow ? Array.from({ length: LAND_W }, (_, k) => k)
          : [...Array.from({ length: BEZEL }, (_, k) => k), ...Array.from({ length: BEZEL }, (_, k) => LAND_W - BEZEL + k)];
        for (const lx of xs) {
          const p = landPixel(fb, lx, ly);
          if (!isWhite(p)) worstViolation++;
        }
      }
    }
    check("nothing is drawn inside the bezel band, sampled across 15 settled-ish frames", worstViolation === 0, `${worstViolation} ink pixel(s) found in the band`);
  }

  // ---- the wall breaks bricks, then clears and regrows -------------------
  // No touch and no tilt: the ball is left entirely to itself, which is the
  // whole point of "no floor to guard" (breakout.c's header comment) - it
  // must be able to clear the wall on its own, eventually, with nobody
  // helping it. Measured empirically at up to ~190s of simulated time for
  // one full clear-and-regrow with these exact constants (a probe run
  // during development); 260s is a generous ceiling, not a tuned number.
  {
    const { dev, t: t0 } = await enterBreakout();
    let t = t0;
    const initialInk = countBrickInk(dev.fb());
    let minInk = initialInk;
    let sawFullWallAgain = false;
    const stepMs = 40;
    const ceilingMs = 260000;
    for (let elapsed = 0; elapsed < ceilingMs; elapsed += stepMs) {
      t += stepMs;
      dev.tick(t);
      if (elapsed % 2000 !== 0) continue;
      const ink = countBrickInk(dev.fb());
      if (ink < minInk) minInk = ink;
      if (elapsed > 5000 && ink >= initialInk * 0.95) { sawFullWallAgain = true; break; }
    }
    check("left entirely alone, the ball eventually breaks most of the wall down",
      minInk < initialInk * 0.3, `wall ink fell from ${initialInk} to as little as ${minInk}`);
    check("...and the wall comes back (clear, celebrate, regrow), unattended",
      sawFullWallAgain, sawFullWallAgain ? "a full wall was seen again" : `never regrew within ${ceilingMs / 1000}s of sim time`);
  }

  // ---- every push this app makes is decision-0001-legal -------------------
  {
    const { dev, t: t0 } = await enterBreakout();
    let t = t0;
    let badPushes = 0, totalPushes = 0;
    for (let i = 0; i < 500; i++) {
      t += FRAME_MS;
      dev.tick(t);
      for (const p of dev.pushes()) {
        totalPushes++;
        if (p.w % 8 !== 0) badPushes++;
      }
    }
    check("every pushed window's row length is a multiple of 8 pixels (decision 0001)",
      badPushes === 0, `${badPushes} bad window(s) out of ${totalPushes} pushes`);
  }

  console.log(`\n${passCount} passed, ${failCount} failed`);
  if (failCount > 0) process.exit(1);
}

main();
