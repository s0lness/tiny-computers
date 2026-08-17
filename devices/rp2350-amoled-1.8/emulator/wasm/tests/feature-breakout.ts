// feature-breakout: what firmware/apps/breakout.c is supposed to do,
// asserted against the REAL compiled firmware. Run with:
//
//   bun run emulator/wasm/build.ts
//   bun run emulator/wasm/tests/feature-breakout.ts
//
// Breakout is in the default app table unconditionally, like every real app
// on this device (docs/decisions/0012): it reads app_frame_t.tilt like
// every other orientation-aware app and touches no hardware of its own, so
// no build flag is needed - see breakout.c's own header comment for the
// whole argument.
//
// This is the `feature-*` half of the pair this directory's convention asks
// for (AGENTS.md's "Regression tests"): clean input, a readable statement of
// intent. The `repro-*` half is repro-breakout-residue.ts, which audits a
// long continuous run for the anti-residue invariant and pins the
// fixed-timestep clock's determinism. There is no touch-dropout partner,
// deliberately: touch is read in exactly one narrow place (the restart tap
// from game over - see breakout.c's own header comment, "WHY THIS EXCEPTION
// TO READS NO TOUCH IS SAFE TO MAKE NARROWLY"), and it is a single discrete
// press-and-release with no position and no drag to get wrong, unlike the
// press-drag-release gestures the dropout-profile tests in this directory
// exist to stress.
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
// other test in this directory uses against its app (see feature-tiltball.ts).
const PLAY_L = 26, PLAY_R = LAND_W - 26, PLAY_T = 26, PLAY_B = LAND_H - 26;
const BALL_R = 9;
const PADDLE_Y = 300, PADDLE_HALF_W = 46;
const PADDLE_CENTER_X = (PLAY_L + PLAY_R) / 2; // 224
const PADDLE_TRAVEL_MAX = (PLAY_R - PLAY_L) / 2 - PADDLE_HALF_W; // 152
const PADDLE_GX_FULL = 0.4;
const N_BRICKS = 18; // 3 rows x 6, see breakout.c's own N_ROWS/BRICKS_PER_ROW
const BEZEL = 10; // gfx.h's PANEL_BEZEL_MARGIN_PX

// Lives/game-over constants, lifted from breakout.c's own #define block the
// same way everything above already is.
const START_LIVES = 3;
const LIFE_LOST_FREEZE_MS = 550;
const LIFE_DOT_R = 6, LIFE_DOT_GAP = 18;
const LIFE_DOT_X0 = PLAY_L + 12, LIFE_DOT_Y = PLAY_T + 12;
const TAP_ARM_SAMPLES = 4, TAP_ARM_MS = 40;
// The deterministic respawn point reset_ball_and_paddle() always uses -
// ARC_CX == LAND_W/2 == PADDLE_CENTER_X, clear of the wall and the paddle
// (see breakout.c's own header comment geometry check) - so a black pixel
// there shortly after a respawn or a restart is a clean, position-based
// "the ball is back" signal.
const RESPAWN_X = PADDLE_CENTER_X, RESPAWN_Y = PADDLE_Y - 100;

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
    // The argument here is PANEL-space g (every call site below reads that
    // way), but emu_sensor_vector() hands it straight to the same
    // tilt_submit_device_g() a real IMU sample goes through - DEVICE axes,
    // not panel ones. firmware/runtime/tilt.c's device_to_panel() is now a
    // real, measured mapping ((dx,dy,dz) -> (dy,dx,-dz), commit b8c06d2),
    // not the identity, so this has to undo it on the way in - same
    // formula feature-tilt.ts's gravity() and feature-clock.ts's own
    // gravity() already use, repeated here rather than duplicated
    // differently. It is currently its own inverse (swap X/Y, negate Z), so
    // "undo" and "apply" are the same call; if device_to_panel() ever stops
    // being self-inverse this has to become a real inverse instead.
    tilt(x: number, y: number, z: number) { e.emu_sensor_vector(1, y, x, -z); },
    touch(down: boolean, x: number, y: number) { e.emu_touch(down ? 1 : 0, x, y); }, // panel coords
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

// Was "countBrickInk" over SATURATED pixels back when bricks were a
// colour palette - a red/orange/yellow/... brick failed both isWhite and
// isBlack by having one RGB565 channel far from the others, cleanly telling
// it apart from the ball/paddle's own anti-aliased BLACK-vs-white edges.
// Now that bricks are black rings (decision: black & white, see breakout.c's
// header "ONE INK" section), that same "neither white nor black" bucket
// catches the ANTI-ALIASED GREY FRINGE of every curve on screen instead -
// the wall's rings still dominate it (a thin ring's edge-to-area ratio is
// far higher than a filled disc's, so the wall contributes much more fringe
// than the ball or paddle's own edges do), which is why the thresholds
// below (>1000 at entry, <30% at the low point, >=95% once regrown) still
// hold with real margin - measured 3610px at entry, falling to 440px at the
// wall's own low point. This is deliberately still framebuffer-only,
// per AGENTS.md's "Regression tests" convention (never an internal), so it
// still cannot distinguish "the wall is mostly gone" from "the wall never
// existed" with certainty - only the entry-vs-later comparison can.
function countWallInk(fb: Uint8Array): number {
  let n = 0;
  for (let ly = 0; ly < LAND_H; ly++) {
    for (let lx = 0; lx < LAND_W; lx++) {
      const p = landPixel(fb, lx, ly);
      if (!isWhite(p) && !isBlack(p)) n++;
    }
  }
  return n;
}

// Reads the lives row directly (LIFE_DOT_X0 + i*LIFE_DOT_GAP, LIFE_DOT_Y),
// one dot centre at a time - each dot is either fully black or fully absent
// (paper white) at its own centre pixel, so this is a clean per-dot read,
// not a fringe-sensitive one.
function livesShown(fb: Uint8Array): number {
  let n = 0;
  for (let i = 0; i < START_LIVES; i++) {
    if (isBlack(landPixel(fb, Math.round(LIFE_DOT_X0 + i * LIFE_DOT_GAP), Math.round(LIFE_DOT_Y)))) n++;
  }
  return n;
}

// Is there black ink at breakout's own deterministic respawn point? A tight
// box (not just the one centre pixel), since the ball only sits EXACTLY at
// RESPAWN_X/Y for the very first physics step after a respawn - one frame
// (FRAME_MS=16ms) later it has already moved a few px along its start
// angle, so this checks a small neighbourhood rather than one exact pixel.
function ballAtRespawn(fb: Uint8Array): boolean {
  for (let dy = -6; dy <= 6; dy += 3) {
    for (let dx = -6; dx <= 6; dx += 3) {
      if (isBlack(landPixel(fb, Math.round(RESPAWN_X + dx), Math.round(RESPAWN_Y + dy)))) return true;
    }
  }
  return false;
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
    const ink = countWallInk(dev.fb());
    check("the wall is on screen at entry", ink > 1000, `${ink} grey-fringe (wall) pixels`);
  }

  // ---- the paddle follows tilt.gx, clamped at the rails ------------------
  //
  // emu_sensor_vector() injects PANEL-axis gravity (emu_abi.h), and
  // runtime_core.c's tilt_for_app() rotates that into a LANDSCAPE app's own
  // space before breakout.c ever sees it: landscape gx comes from PANEL gy,
  // not panel gx (see runtime_core.c's own derivation, "dlx = dpy"). So to
  // put a given value on app_frame_t.tilt.gx, this feeds it as the panel
  // vector's Y component - the same rotation feature-tiltball.ts's own
  // gravityFor() already has to account for.
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

  // ---- entry: three lives shown, wordlessly ------------------------------
  {
    const { dev } = await enterBreakout();
    const n = livesShown(dev.fb());
    check("three lives are shown at entry, as three solid dots", n === START_LIVES, `${n} dot(s) found`);
  }

  // ---- a lost ball costs a life, and the wall is NOT reset ---------------
  //
  // The floor is now a floor, not a fourth wall (the owner's own overrule -
  // see breakout.c's header comment). Pin the paddle hard to one rail via a
  // sustained extreme tilt so it can no longer catch a ball that ends up on
  // the other side of the field, then just wait: left alone, the ball
  // eventually finds the gap. This is deliberately the SAME "left entirely
  // alone" shape the wall-clear test below already uses, just with the
  // paddle deliberately taken out of the picture instead of centred.
  let lostOnce: { dev: Device; t: number; inkBeforeLoss: number } | null = null;
  {
    const { dev, t: t0 } = await enterBreakout();
    const inkAtEntry = countWallInk(dev.fb());
    let t = t0;
    const stepMs = 40, ceilingMs = 120000;
    let livesAfterFirstLoss = START_LIVES;
    let inkJustBeforeLoss = inkAtEntry;
    let sawLoss = false;
    for (let elapsed = 0; elapsed < ceilingMs && !sawLoss; elapsed += stepMs) {
      dev.tilt(0, -1, 0); // landscape gx=-1: paddle pinned to its left rail
      t += stepMs;
      dev.tick(t);
      const n = livesShown(dev.fb());
      if (n < START_LIVES) {
        sawLoss = true;
        livesAfterFirstLoss = n;
      } else {
        inkJustBeforeLoss = countWallInk(dev.fb());
      }
    }
    check("left with the paddle pinned away from it, the ball is eventually lost and a life is spent",
      sawLoss && livesAfterFirstLoss === START_LIVES - 1,
      sawLoss ? `lives now ${livesAfterFirstLoss}` : `never lost within ${ceilingMs / 1000}s of sim time`);
    if (sawLoss) {
      // Give the freeze a moment to settle, then check the wall: kept, not
      // reset - breakout.c's header comment's own "WHAT HAPPENS TO THE
      // BRICKS ON A LIFE LOST" argument.
      t = run(dev, t, LIFE_LOST_FREEZE_MS + 200, [0, -1, 0]);
      const inkAfter = countWallInk(dev.fb());
      check("...and the wall is kept exactly as it was, not reset",
        Math.abs(inkAfter - inkJustBeforeLoss) <= inkAtEntry * 0.05,
        `ink just before loss=${inkJustBeforeLoss}, after the freeze=${inkAfter} (entry=${inkAtEntry})`);
      lostOnce = { dev, t, inkBeforeLoss: inkJustBeforeLoss };
    }
  }

  // ---- zero lives reaches game over, and the table holds still -----------
  let gameOver: { dev: Device; t: number } | null = null;
  if (lostOnce) {
    const { dev } = lostOnce;
    let t = lostOnce.t;
    const stepMs = 40, ceilingMs = 240000;
    let reachedZero = false;
    for (let elapsed = 0; elapsed < ceilingMs && !reachedZero; elapsed += stepMs) {
      dev.tilt(0, -1, 0); // keep the paddle pinned away, same as above
      t += stepMs;
      dev.tick(t);
      if (livesShown(dev.fb()) === 0) reachedZero = true;
    }
    check("kept away from the ball long enough, all three lives are eventually spent",
      reachedZero, reachedZero ? "0 lives shown" : `never reached 0 within ${ceilingMs / 1000}s of sim time`);

    if (reachedZero) {
      // Hold well past one ordinary life-lost freeze (550ms): if this were
      // just another respawn pause the ball would already be back by now.
      // Sample repeatedly rather than once, so a late respawn cannot slip
      // through between two checks.
      let staysGone = true;
      for (let i = 0; i < 8; i++) {
        dev.tilt(0, -1, 0);
        t += 400;
        dev.tick(t);
        if (livesShown(dev.fb()) !== 0 || ballAtRespawn(dev.fb())) { staysGone = false; break; }
      }
      check("...and the table then holds still - no ball, no lives - well past one ordinary freeze, waiting for a tap",
        staysGone, staysGone ? "held for 3.2s with 0 lives and no ball" : "recovered on its own (not a real game over)");
      gameOver = { dev, t };
    }
  }

  // ---- a tap restarts the game -------------------------------------------
  if (gameOver) {
    const { dev } = gameOver;
    let t = gameOver.t;
    // Hold contact long enough to arm dino.c's own arming rule
    // (TAP_ARM_SAMPLES over TAP_ARM_MS), comfortably past both.
    for (let i = 0; i < 6; i++) {
      dev.touch(true, 184, 224); // panel centre; breakout's restart ignores position
      t += FRAME_MS;
      dev.tick(t);
    }
    dev.touch(false, 184, 224);
    t += FRAME_MS;
    dev.tick(t);
    // A couple more settled frames for the fresh ball's first push to land.
    t += FRAME_MS;
    dev.tick(t);
    const n = livesShown(dev.fb());
    check("a tap from game over brings all three lives back", n === START_LIVES, `${n} dot(s) shown after the tap`);
    check("...and drops a fresh ball in at the deterministic start position",
      ballAtRespawn(dev.fb()), "no ink found near the respawn point");
  } else {
    check("a tap restarts the game", false, "skipped - never reached a confirmed game over above");
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

  // ---- the wall breaks bricks, then a restart regrows it ------------------
  //
  // Before lives existed, this test drove an UNTOUCHED, UNTILTED device
  // (paddle parked dead centre) for up to 260s of simulated time and always
  // saw the wall clear and regrow on its own - that was the whole point of
  // "no floor to guard": the toy could never stop playing itself. It cannot
  // any more, and measuring that honestly is worth doing here rather than
  // quietly deleting the coverage: a centred, NEVER-corrected paddle only
  // physically catches the ball within a ~92x32px band around its own spine
  // (ball_paddle_bounce's rr=16, half the ~180px-wide LENS SHAPE actually
  // drawn - a pre-existing gap between the visual paddle and its hitbox,
  // unrelated to this feature, that simply had no consequence before there
  // was a life to spend on a miss), so three lives are spent inside about
  // 15 SECONDS of simulated time even when a second, independent run of this
  // same wasm was driven with an idealised controller (tilt set every tick
  // from the ball's own tracked x, bypassing reaction time entirely, tried
  // as a probe during development, not asserted here since it is not this
  // file's job to grade paddle physics) - three lives is simply not enough
  // to reach a natural full clear against this paddle's real hit zone. That
  // is a genuine, reportable consequence of the owner's own request for a
  // fail state, not a bug in this test or in breakout.c.
  //
  // So this checks the two halves of "the wall breaks, then comes back"
  // SEPARATELY, each in the way it can actually still happen unattended:
  // some real breakage before the three lives run out (still the ball's own
  // doing, still no touch and no tilt), and the SAME regrow wave a mid-game
  // clear already uses, reached this time through the restart tap instead
  // of through a natural full clear - see breakout.c's header comment's
  // "GAME OVER, AND HOW SHE STARTS AGAIN" on why that is the same event,
  // reused, not a second animation.
  {
    const { dev, t: t0 } = await enterBreakout();
    const initialInk = countWallInk(dev.fb());
    let t = t0;
    let minInk = initialInk;
    const stepMs = 40, ceilingMs = 60000;
    let overAt = -1;
    for (let elapsed = 0; elapsed < ceilingMs && overAt < 0; elapsed += stepMs) {
      t += stepMs;
      dev.tick(t);
      if (elapsed % 1000 !== 0) continue;
      const ink = countWallInk(dev.fb());
      if (ink < minInk) minInk = ink;
      if (livesShown(dev.fb()) === 0) overAt = t;
    }
    check("left entirely alone, the ball breaks real bricks before the paddle's narrow catch zone lets all three lives go",
      overAt >= 0 && minInk < initialInk * 0.9,
      overAt >= 0 ? `wall ink fell from ${initialInk} to ${minInk} before game over at t=${overAt}`
        : `never reached game over within ${ceilingMs / 1000}s`);

    if (overAt >= 0) {
      // Past the freeze, holding at game over - restart, then give the
      // regrow wave (CELEB_TOTAL_MS, under 2s) time to finish.
      t = run(dev, t, LIFE_LOST_FREEZE_MS + 200, null);
      for (let i = 0; i < 6; i++) { dev.touch(true, 184, 224); t += FRAME_MS; dev.tick(t); }
      dev.touch(false, 184, 224);
      t = run(dev, t, 3000, null);
      const inkAfter = countWallInk(dev.fb());
      // 90%, not the 95% the entry-vs-regrown comparison elsewhere in this
      // file uses: countWallInk sums grey fringe across the WHOLE frame, and
      // the ball and paddle land at slightly different sub-pixel positions
      // after a restart than they happened to sit at in the original entry
      // snapshot, which shifts their own small share of that fringe - 90%
      // is still a real, comfortable margin over "the wall came back".
      check("...and a tap's regrow wave brings the wall back, the same way a mid-game clear's own does",
        inkAfter >= initialInk * 0.90, `ink back to ${inkAfter} of an entry value of ${initialInk}`);
    }
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
