// Everything the gate knows about THIS board. rules.ts, exercise.ts,
// touch.ts, device.ts and freshness.ts know none of it, so a second device
// gets a second file here rather than a fork of the checker - the same
// split tools/invariants/rules/ already uses, and for the same reason
// (docs/decisions/0006).
//
// THE BUDGET NUMBERS ARE MEASURED, NOT CHOSEN. See `bun run
// tools/gate/run.ts --measure`, which prints what every app actually costs
// and is how these were set. Re-run it if an app changes shape; a budget
// that has drifted far from what the apps do is a budget that has stopped
// meaning anything.

import type { Limits } from "../rules";

export const PANEL_W = 368;
export const PANEL_H = 448;
export const PANEL_AREA = PANEL_W * PANEL_H; // 164,864

/** emu_device()'s buttons[1].longPressMs. */
export const PWR_LONG_PRESS_MS = 1500;

export const LIMITS: Limits = {
  panelW: PANEL_W,
  panelH: PANEL_H,

  // decision 0001. gfx_push rounds every row length up to a multiple of
  // PUSH_GRAN_W=8 (gfx.c; the 64px minimum was the superseded earlier fix,
  // see AGENTS.md), so every legal window is already a multiple of 8 wide;
  // this is the check that a second push path does not appear that is not
  // gfx_push.
  pushRowMultiple: 8,

  // WHAT THE APPS ACTUALLY COST, measured with --measure over the whole
  // stimulus list, excluding the one enter() tick the runtime pushes a
  // full panel on by design:
  //
  //   chrono   peak 170,624 px/tick  (1.03 panels)   peak  3 pushes
  //   draw     peak 164,864 px/tick  (1.00 panels)   peak 16 pushes
  //   timer    peak 164,864 px/tick  (1.00 panels)   peak  5 pushes
  //   four     peak 164,864 px/tick  (1.00 panels)   peak  2 pushes
  //
  // One full panel in a tick is normal here and always will be: a mode
  // change repaints the screen (the timer's alarm, Connect Four's board
  // reset, the sketchpad's shake-to-erase), and one 12ms push is a cost
  // this board pays happily at 60Hz-ish.
  //
  // What killed the board was not ONE full panel, it was HUNDREDS in a
  // single tick: palette_render_frame() whitened ~165,000 pixels per
  // drained touch sample, and core1 polls the controller at up to 1.4kHz
  // (sensors.h), so one tick's drain loop could issue that render, and its
  // push, hundreds of times before core0 got back to the watchdog.
  //
  // So the budget is set at THREE full panels per tick. That is three
  // times the worst thing any healthy app does, which leaves room for an
  // app that genuinely repaints twice in one tick, and it is two orders of
  // magnitude below what the palette bug produced. A budget tight enough
  // to catch the next honest app doing something slightly new would be a
  // budget that fires on honest apps, and a gate that cries wolf gets
  // re-run until it agrees (four.c's own test comment makes this argument
  // about a different threshold, and it was right).
  maxPushedPixelsPerTick: 3 * PANEL_AREA,

  // Push COUNT is the second term, and it is the one the palette bug
  // actually blew up: each push is its own run of per-row DMA transfers
  // with its own setup. Measured peak across every app is 16, all of it
  // the sketchpad's palette redrawing its cells in one tick. emu_shim.c
  // silently DROPS any push past 128 in a tick (MAX_PUSHES), which means
  // above that number the emulator stops being able to see the rule
  // `pushed-or-invisible` violated at all - so the budget has to sit well
  // below it or the instrument lies. 32 is twice the observed peak and a
  // quarter of the point where the recorder stops recording.
  maxPushesPerTick: 32,

  // gfx.h PANEL_BEZEL_MARGIN_PX. Deliberately rough (a photograph taken at
  // an angle cannot measure a bezel to the pixel), and deliberately in one
  // place so the owner can correct it in one edit.
  bezelMarginPx: 10,

  // px_to_gray maps paper to 252 and full ink to 0. A pixel at or below
  // 128 is at least half covered, which is a piece of drawing rather than
  // an anti-aliasing fringe leaking one pixel past a shape that is itself
  // outside the band. Fringes are not what this rule is about: decision
  // 0009 is explicit that the greys at an edge are how a curve exists at
  // all on a pixel grid.
  inkGrayMax: 128,
};

/**
 * The app arena (app.h APP_ARENA_BYTES) and where the headroom rule fires.
 *
 * app.h: "an app that cannot fit in the arena is a build-time bug, not a
 * runtime condition to handle" - the runtime's answer to overflow is a red
 * trap screen, forever. The gate is where builds are judged, so the gate is
 * where that bug has to become visible, BEFORE the byte that trips the
 * trap: an app at 100% is a child looking at a red screen, and an app at
 * 95% is one small feature away from it with no instrument saying so.
 *
 * The fail line is 90% of capacity. The capacity itself is NOT restated
 * here: the gate reads it from the firmware (emu_arena_capacity()), so
 * APP_ARENA_BYTES changing in app.h moves the line with it instead of this
 * file silently disagreeing - a second copy of that number is exactly the
 * drift decision 0014 is about. The largest app today (the sketchpad,
 * measured by this same rule's report line) sits near 81%, so today's apps
 * pass with real margin, and an app that crosses 90% is doing something new
 * that deserves a decision, not a silent landing. If an app legitimately
 * needs more, this fraction moves in this file with a written reason, the
 * same contract the push budgets above already have.
 */
export const ARENA_FAIL_FRACTION = 0.9;

/**
 * The menu grid's ceiling (decision 0013): twelve apps at 4x3 cells of
 * 112px. A thirteenth does not fall off the screen - menu.c clamps rows and
 * narrows cells instead - so nothing visibly breaks, the targets just go
 * under a child's fingertip, which no automated rule downstream can see.
 * This is the arithmetic form of that decision: past twelve, a second menu
 * design is owed BEFORE the app lands, not after.
 */
export const APP_COUNT_MAX = 12;

/**
 * Where the clock-driven and settle-cadence probes put the finger. Panel
 * (portrait) coordinates, comfortably inside GESTURE_INSET_PX.
 *
 * The centre is here because it is where the sketchpad's palette opens.
 * The other two are here because a probe that only ever looks at the
 * middle of the screen is a probe about the middle of the screen.
 */
export const PROBE_HOLDS = [
  { x: Math.round(PANEL_W / 2), y: Math.round(PANEL_H / 2) },
  { x: 110, y: 140 },
  { x: PANEL_W - 110, y: PANEL_H - 140 },
];

/**
 * Long enough to pass any long-press threshold the apps have (the
 * sketchpad's is 550ms), plus the confirmation window in front of it.
 */
export const PROBE_HOLD_MS = 900;

/** Long enough for the palette's own pop-in (188ms) several times over. */
export const PROBE_WINDOW_MS = 700;
