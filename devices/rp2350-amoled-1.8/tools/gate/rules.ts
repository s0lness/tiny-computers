// The cross-cutting rules, as machinery. Nothing here names an app, and
// nothing here names this board: the numbers live in
// rules/rp2350-amoled-1.8.ts, the same split tools/invariants already uses
// so that moving this to `puck` later means a new rules file rather than a
// fork of the checker (docs/decisions/0006).
//
// The point of this file is that these rules hold for every app on every
// tick BY CONSTRUCTION. Each of them was, before this, either a per-app
// assertion someone remembered to write or a paragraph in a decision
// record. Both of those are opt-in, and every bug in this list was found
// in one app and was possible in all of them.

import type { Device, Rect } from "./device";

export interface Limits {
  panelW: number;
  panelH: number;
  /** Row length must be a multiple of this (decision 0001). */
  pushRowMultiple: number;
  /** Total pushed pixels allowed in one tick. */
  maxPushedPixelsPerTick: number;
  /** Number of pushes allowed in one tick. */
  maxPushesPerTick: number;
  /** Pixels along every edge the case hides (gfx.h PANEL_BEZEL_MARGIN_PX). */
  bezelMarginPx: number;
  /** Coverage at or above which a pixel counts as ink rather than a fringe. */
  inkGrayMax: number;
}

export interface Violation {
  rule: string;
  why: string;
  see: string;
  detail: string;
}

// gfx.h's px_to_gray, in TypeScript: the framebuffer is byte-swapped
// RGB565 and the panel is used as monochrome, so green's 6 bits carry the
// coverage. 0 is full ink, 252 is paper.
export function pxToGray(px: number): number {
  const v = ((px >> 8) | (px << 8)) & 0xffff;
  return ((v >> 5) & 0x3f) << 2;
}

/* ---- rule PUSH-GEOMETRY (bug 3, and the panel-corrupting half of bug 2)
 *
 * Decision 0001: AMOLED_1IN8_DisplayWindows issues one DMA transfer per
 * row of the window, and the panel corrupts any window whose row length is
 * not a multiple of 8 pixels. gfx_push is what makes a rectangle legal and
 * an app is never in a position to violate it - which is exactly the kind
 * of claim that is true until someone adds a second push path. This is
 * the standing check that nobody has.
 */
export function checkPushGeometry(pushes: Rect[], lim: Limits): Violation[] {
  const out: Violation[] = [];
  for (const r of pushes) {
    const bad: string[] = [];
    if (r.w <= 0 || r.h <= 0) bad.push(`non-positive extent ${r.w}x${r.h}`);
    if (r.x < 0 || r.y < 0 || r.x + r.w > lim.panelW || r.y + r.h > lim.panelH) {
      bad.push(`outside the ${lim.panelW}x${lim.panelH} panel`);
    }
    if (r.w % lim.pushRowMultiple !== 0) {
      bad.push(`row length ${r.w} is not a multiple of ${lim.pushRowMultiple}`);
    }
    if (bad.length > 0) {
      out.push({
        rule: "push-geometry",
        why: "the panel corrupts any window whose row length is not a multiple of 8 pixels; a window outside the panel is a DMA out of bounds",
        see: "docs/decisions/0001-push-min-width.md",
        detail: `push (${r.x},${r.y}) ${r.w}x${r.h}: ${bad.join("; ")}`,
      });
    }
  }
  return out;
}

/* ---- rule PUSH-BUDGET (bug 1)
 *
 * The palette rebooted the board because its render cost was unbounded: a
 * near-full panel redrawn per drained touch sample, and core0 never got
 * back to the watchdog. There is now a per-frame cost test for the palette
 * alone, which is the shape of failsafe this project keeps having to
 * replace: it protects the app that already hurt someone.
 *
 * The emulator cannot measure time (decision 0003), so it counts. What it
 * counts is what the hardware actually charges for: pushed pixels, and the
 * number of transfers. Those are the two terms in the 12ms full-panel
 * push. See rules/rp2350-amoled-1.8.ts for where the numbers come from.
 */
export function checkPushBudget(pushes: Rect[], pushedPixels: number, lim: Limits): Violation[] {
  const out: Violation[] = [];
  if (pushedPixels > lim.maxPushedPixelsPerTick) {
    const panels = (pushedPixels / (lim.panelW * lim.panelH)).toFixed(2);
    out.push({
      rule: "push-budget",
      why: "work per tick has to be bounded by something other than how fast input arrives, or core0 never gets back to the watchdog",
      see: "docs/decisions/0008-the-emulator-seam-is-in-the-wrong-place.md",
      detail: `pushed ${pushedPixels} px in one tick (${panels} full panels), budget ${lim.maxPushedPixelsPerTick}`,
    });
  }
  if (pushes.length > lim.maxPushesPerTick) {
    out.push({
      rule: "push-budget",
      why: "each push is a separate run of per-row DMA transfers; their count is a cost in its own right, and emu_shim.c silently drops any past 128",
      see: "docs/decisions/0001-push-min-width.md",
      detail: `${pushes.length} pushes in one tick, budget ${lim.maxPushesPerTick}`,
    });
  }
  return out;
}

/* ---- rule PUSHED-OR-INVISIBLE (bug 2)
 *
 * A pixel that changes outside a pushed rectangle never reaches the real
 * panel. It looks correct in the emulator and is broken on hardware, which
 * makes it exactly the class of bug this emulator exists to catch. It has
 * shipped four times: a timer push one column too narrow, cap pixels
 * outside their band, palette gutter residue, a menu halo two pixels wider
 * than its column.
 *
 * Implemented as a mask rather than a rectangle-membership test per pixel,
 * so a full repaint costs a fill and not 165,000 loops over a rect list.
 */
export class PushCoverage {
  private mask: Uint8Array;
  private w: number;
  private h: number;

  constructor(w: number, h: number) {
    this.w = w;
    this.h = h;
    this.mask = new Uint8Array(w * h);
  }

  paint(pushes: Rect[]): void {
    this.mask.fill(0);
    for (const r of pushes) {
      const x0 = Math.max(0, r.x);
      const y0 = Math.max(0, r.y);
      const x1 = Math.min(this.w, r.x + r.w);
      const y1 = Math.min(this.h, r.y + r.h);
      for (let y = y0; y < y1; y++) this.mask.fill(1, y * this.w + x0, y * this.w + x1);
    }
  }

  covered(i: number): boolean {
    return this.mask[i] !== 0;
  }
}

export interface OutsideChange {
  count: number;
  first: { x: number; y: number; from: number; to: number } | null;
}

export function findChangesOutside(
  prev: Uint16Array,
  cur: Uint16Array,
  cover: PushCoverage,
  w: number,
  h: number
): OutsideChange {
  let count = 0;
  let first: OutsideChange["first"] = null;
  const n = w * h;
  for (let i = 0; i < n; i++) {
    if (prev[i] === cur[i]) continue;
    if (cover.covered(i)) continue;
    count++;
    if (first === null) {
      first = { x: i % w, y: (i / w) | 0, from: prev[i]!, to: cur[i]! };
    }
  }
  return { count, first };
}

/* ---- rule BEZEL (bug 4)
 *
 * gfx.h names PANEL_BEZEL_MARGIN_PX and says, in prose, that nothing a
 * child needs to see belongs inside it. Nothing enforces it, and the
 * emulator has no bezel to hide anything behind, so the only test that has
 * ever caught this is a person holding the device.
 *
 * WHAT THIS CAN HONESTLY ASSERT. It cannot tell an app's chrome from a
 * child's own drawing, and the sketchpad is entitled to put ink wherever a
 * finger goes. So the rule is scoped to what the app paints of its own
 * accord: it is evaluated on a SETTLED frame, and the gate only ever
 * touches the glass well inside the safe area. Ink in the band on such a
 * frame was put there by the app's own layout, and is under the case.
 */
export function checkBezel(fb: Uint16Array, lim: Limits, context: string): Violation[] {
  const { panelW: w, panelH: h, bezelMarginPx: m, inkGrayMax } = lim;
  let worst = 256;
  let worstAt: { x: number; y: number } | null = null;
  let inkPixels = 0;

  const consider = (x: number, y: number) => {
    const g = pxToGray(fb[y * w + x]!);
    if (g <= inkGrayMax) {
      inkPixels++;
      if (g < worst) { worst = g; worstAt = { x, y }; }
    }
  };
  for (let y = 0; y < h; y++) {
    const edgeRow = y < m || y >= h - m;
    if (edgeRow) {
      for (let x = 0; x < w; x++) consider(x, y);
    } else {
      for (let x = 0; x < m; x++) consider(x, y);
      for (let x = w - m; x < w; x++) consider(x, y);
    }
  }

  if (inkPixels === 0) return [];
  const at = worstAt as { x: number; y: number } | null;
  return [{
    rule: "bezel",
    why: `the case hides ${m}px along every edge; ink there is painted and never seen`,
    see: "firmware/runtime/gfx.h",
    detail: `${context}: ${inkPixels} ink pixels (gray <= ${inkGrayMax}) inside the ${m}px band` +
      (at ? `, darkest ${worst} at (${at.x},${at.y})` : ""),
  }];
}

/* ---- the per-tick watcher ------------------------------------------- */

export interface WatchStats {
  ticks: number;
  /** Peak over BUDGETED ticks only: the one enter() tick the runtime pushes
   *  a full panel on by design is not news and would hide everything else. */
  maxPushedPixels: number;
  maxPushes: number;
  /** The context that produced maxPushedPixels. */
  peakContext: string;
  totalPushes: number;
  ticksWithPushes: number;
}

/**
 * Wraps a Device so that every tick is checked. `tick()` here is the only
 * way the gate advances the clock, which is what makes "on every tick"
 * true rather than aspirational.
 */
export class TickWatcher {
  readonly violations: Violation[] = [];
  readonly stats: WatchStats = {
    ticks: 0, maxPushedPixels: 0, maxPushes: 0, peakContext: "(nothing pushed)",
    totalPushes: 0, ticksWithPushes: 0,
  };
  private prev: Uint16Array;
  private cover: PushCoverage;
  private dev: Device;
  private lim: Limits;
  private context = "";
  private budgetArmed = true;

  constructor(dev: Device, lim: Limits) {
    this.dev = dev;
    this.lim = lim;
    this.prev = new Uint16Array(dev.fb());
    this.cover = new PushCoverage(lim.panelW, lim.panelH);
  }

  /** Labels every violation raised from here until the next call. */
  setContext(context: string): void {
    this.context = context;
  }

  /**
   * The one exemption, and it is narrow: the tick an app is entered on.
   * app.h makes the runtime clear the framebuffer and push the WHOLE panel
   * once after enter() returns, so that tick costs a full panel by design
   * and says nothing about whether the app's steady state is bounded.
   */
  disarmBudgetForOneTick(): void {
    this.budgetArmed = false;
  }

  observe(): void {
    const pushes = this.dev.pushes();
    const pushedPixels = this.dev.pushedPixels();
    const cur = this.dev.fb();

    this.stats.ticks++;
    this.stats.totalPushes += pushes.length;
    if (pushes.length > 0) this.stats.ticksWithPushes++;
    if (this.budgetArmed) {
      if (pushedPixels > this.stats.maxPushedPixels) {
        this.stats.maxPushedPixels = pushedPixels;
        this.stats.peakContext = this.context;
      }
      if (pushes.length > this.stats.maxPushes) this.stats.maxPushes = pushes.length;
    }

    const found: Violation[] = [];
    found.push(...checkPushGeometry(pushes, this.lim));
    if (this.budgetArmed) found.push(...checkPushBudget(pushes, pushedPixels, this.lim));
    this.budgetArmed = true;

    this.cover.paint(pushes);
    const outside = findChangesOutside(this.prev, cur, this.cover, this.lim.panelW, this.lim.panelH);
    if (outside.count > 0 && outside.first) {
      const f = outside.first;
      found.push({
        rule: "pushed-or-invisible",
        why: "a pixel that changes outside a pushed rectangle never reaches the panel: correct in the emulator, broken on the board",
        see: "emulator/docs/findings-app-fuzzing.md",
        detail: `${outside.count} pixels changed outside this tick's ${pushes.length} pushed rect(s), first at (${f.x},${f.y}) 0x${f.from.toString(16)} -> 0x${f.to.toString(16)}`,
      });
    }

    for (const v of found) this.violations.push({ ...v, detail: `${this.context}: ${v.detail}` });
    this.prev.set(cur);
  }

  /** The bezel rule, evaluated on whatever is on screen right now. */
  checkSettledBezel(): void {
    for (const v of checkBezel(this.dev.fb(), this.lim, this.context)) this.violations.push(v);
  }
}
