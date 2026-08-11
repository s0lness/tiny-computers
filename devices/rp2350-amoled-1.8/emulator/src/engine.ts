// Faithful port of the touch-handling half of main()'s while(true) loop in
// firmware/main.c (lines 858-993): stroke start needs two agreeing reports,
// stray single reports go nowhere, a mid-stroke jump is accepted if it is
// within the speed-derived allowance, rejected-but-confirmed jumps split
// the stroke instead of drawing a line across the gap, and a lift is only
// believed after LIFT_DEBOUNCE_MS with no contact. Variable names below
// mirror the C locals (fingerDown, pendingStart, haveCand, bridging, ...)
// on purpose, so this can be diffed against main.c by eye.
//
// Driven by TouchSim reports instead of real I2C reads. The dedupe-on-
// unchanged-report logic (main.c's `newReport`) is what makes this correct
// even though the engine is ticked far faster than any configured report
// rate, exactly as in the firmware (polled ~5000Hz against a ~60Hz
// controller).

import { PANEL_W, PANEL_H, type Tunables } from "./constants";
import { Panel, emptyDirty, fullDirty, type DirtyRect } from "./raster";
import { PenState } from "./pen";
import { TouchSim } from "./touchsim";

// Shake-to-erase tuning (main.c lines 91-97). JOLT_* are not reproduced
// (there is no simulated accelerometer here, see README): the emulator's
// shake control fires wipeErase() directly, but still respects the two
// behavioural rules that matter for feel: suppressed while a finger is
// down, and a cooldown so one shake cannot erase twice.
const ERASE_COOLDOWN_MS = 1200;
const WIPE_BANDS = 16;
const WIPE_BAND_DELAY_MS = 15;

export interface EngineStats {
  fingerDown: boolean;
  glitches: number;
  dropouts: number;
  strays: number;
  splits: number;
  simDropouts: number;
  simStrays: number;
  reportsPerSec: number;
}

export type RenderFn = (dirty: DirtyRect) => void;

export class Engine {
  readonly panel = new Panel();
  readonly pen = new PenState();
  readonly touch: TouchSim;

  private fingerDown = false;
  private lastRawX = 0;
  private lastRawY = 0;
  private lastSampleMs = 0;
  private bridging = false;
  private pendingStart = false;
  private pendX = 0;
  private pendY = 0;
  private haveCand = false;
  private candX = 0;
  private candY = 0;
  private lastReportX = -1;
  private lastReportY = -1;

  private glitches = 0;
  private dropouts = 0;
  private strays = 0;
  private splits = 0;
  private reportCount = 0;
  private lastStatMs = 0;
  private lastReportedFingers = -1;
  reportsPerSec = 0;

  private eraseCooldownUntil = 0;
  private erasing = false;
  private patternShown = false;

  constructor(touch: TouchSim, private tunables: Tunables) {
    this.touch = touch;
  }

  setTunables(t: Tunables): void {
    this.tunables = t;
  }

  isFingerDown(): boolean {
    return this.fingerDown;
  }

  isErasing(): boolean {
    return this.erasing;
  }

  // Port of draw_test_pattern() (main.c lines 356-373): a grey ramp plus
  // the same draw_capsule() calls the pen uses, at the three radii the
  // pressure model spans, plus one tapered capsule. Shown once at boot,
  // cleared on first touch, exactly like the firmware. Call once before the
  // first tick; the returned rect is the initial full-panel paint.
  showBootPattern(): DirtyRect {
    const dirty = emptyDirty();
    for (let y = 40; y < 110; y++) {
      const row = y * PANEL_W;
      for (let x = 0; x < PANEL_W; x++) {
        this.panel.gray[row + x] = Math.round((x * 255) / (PANEL_W - 1));
      }
    }
    this.panel.drawCapsule(40, 170, 1, 320, 215, 1, dirty);
    this.panel.drawCapsule(40, 250, 3, 320, 295, 3, dirty);
    this.panel.drawCapsule(40, 330, 6, 320, 375, 6, dirty);
    this.panel.drawCapsule(40, 410, 1, 320, 430, 7, dirty);
    this.patternShown = true;
    return fullDirty();
  }

  // One iteration of the firmware's while(true) body, touch-handling half
  // only (button/IMU/profiler bookkeeping live in device.ts / main.ts).
  // Returns the dirty rect for this tick (may be empty).
  tick(nowMs: number): DirtyRect {
    const dirty = emptyDirty();
    const c = this.tunables;
    const report = this.touch.poll(nowMs);
    if (report.fingers !== this.lastReportedFingers) {
      this.lastReportedFingers = report.fingers;
      this.reportCount++;
    }
    const haveTouch = report.fingers !== 0;

    if (haveTouch && this.patternShown) {
      // First touch wipes the boot test pattern; do not draw with it.
      this.patternShown = false;
      this.panel.clear();
      return fullDirty();
    } else if (haveTouch) {
      const x = clampInt(report.x, 0, PANEL_W - 1);
      const y = clampInt(report.y, 0, PANEL_H - 1);

      const newReport = x !== this.lastReportX || y !== this.lastReportY;
      this.lastReportX = x;
      this.lastReportY = y;

      if (!newReport) {
        // Nothing new from the controller: leave all stroke state be.
      } else if (!this.fingerDown) {
        if (!this.pendingStart) {
          this.pendingStart = true;
          this.pendX = x;
          this.pendY = y;
        } else {
          this.pendingStart = false;
          this.fingerDown = true;
          this.haveCand = false;
          this.lastSampleMs = nowMs;
          this.pen.begin(this.panel, dirty, this.pendX, this.pendY, c);
          this.lastRawX = x;
          this.lastRawY = y;
          this.pen.sample(this.panel, dirty, x, y, false, c);
        }
      } else {
        const jx = x - this.lastRawX, jy = y - this.lastRawY;
        const dtMs = nowMs - this.lastSampleMs;
        let allow = c.MAX_SPEED_PX_PER_MS * dtMs;
        if (allow < c.MIN_JUMP_ALLOW_PX) allow = c.MIN_JUMP_ALLOW_PX;
        if (allow > c.MAX_JUMP_PX) allow = c.MAX_JUMP_PX;

        const jumpSq = jx * jx + jy * jy;
        const confirmed =
          this.haveCand &&
          (x - this.candX) * (x - this.candX) + (y - this.candY) * (y - this.candY) <= c.CONFIRM_PX * c.CONFIRM_PX;

        if (jumpSq <= allow * allow) {
          this.haveCand = false;
          this.lastRawX = x;
          this.lastRawY = y;
          this.lastSampleMs = nowMs;
          this.pen.sample(this.panel, dirty, x, y, this.bridging, c);
          this.bridging = false;
        } else if (confirmed) {
          this.haveCand = false;
          this.pen.end(this.panel, dirty);
          this.pen.begin(this.panel, dirty, x, y, c);
          this.lastRawX = x;
          this.lastRawY = y;
          this.lastSampleMs = nowMs;
          this.bridging = false;
          this.splits++;
        } else {
          this.candX = x;
          this.candY = y;
          this.haveCand = true;
          this.glitches++;
        }
      }
    } else if (this.pendingStart && !this.fingerDown) {
      this.pendingStart = false;
      this.lastReportX = -1;
      this.lastReportY = -1;
      this.strays++;
    } else if (this.fingerDown) {
      if (nowMs - this.lastSampleMs >= c.LIFT_DEBOUNCE_MS) {
        this.fingerDown = false;
        this.bridging = false;
        this.lastReportX = -1;
        this.lastReportY = -1;
        this.pen.end(this.panel, dirty);
      } else {
        if (!this.bridging) this.dropouts++;
        this.bridging = true;
      }
    }

    if (nowMs - this.lastStatMs >= 1000) {
      this.reportsPerSec = this.reportCount;
      this.reportCount = 0;
      this.lastStatMs = nowMs;
    }

    return dirty;
  }

  stats(): EngineStats {
    return {
      fingerDown: this.fingerDown,
      glitches: this.glitches,
      dropouts: this.dropouts,
      strays: this.strays,
      splits: this.splits,
      simDropouts: this.touch.simDropouts,
      simStrays: this.touch.simStrays,
      reportsPerSec: this.reportsPerSec,
    };
  }

  resetCounters(): void {
    this.glitches = 0;
    this.dropouts = 0;
    this.strays = 0;
    this.splits = 0;
    this.touch.resetStats();
  }

  // Shake-to-erase (main.c's shake_poll_and_check + wipe_erase, minus the
  // jolt-window logic there is no simulated IMU for). Animates as 16 bands
  // top to bottom, not an instant blank, exactly like wipe_erase(). `render`
  // is called after each band so the caller can push it to the canvas.
  async shake(nowMs: number, render: RenderFn): Promise<boolean> {
    if (this.fingerDown || this.erasing || nowMs < this.eraseCooldownUntil) return false;
    this.eraseCooldownUntil = nowMs + ERASE_COOLDOWN_MS;
    this.erasing = true;
    const bandH = Math.floor(PANEL_H / WIPE_BANDS);
    try {
      for (let b = 0; b < WIPE_BANDS; b++) {
        const y0 = b * bandH;
        const y1 = b === WIPE_BANDS - 1 ? PANEL_H : y0 + bandH;
        for (let y = y0; y < y1; y++) {
          const row = y * PANEL_W;
          for (let x = 0; x < PANEL_W; x++) this.panel.gray[row + x] = 255;
        }
        render({ minX: 0, minY: y0, maxX: PANEL_W - 1, maxY: y1 - 1 });
        await delay(WIPE_BAND_DELAY_MS);
      }
    } finally {
      this.pen.reset();
      this.pendingStart = false;
      this.haveCand = false;
      this.lastReportX = -1;
      this.lastReportY = -1;
      this.erasing = false;
    }
    return true;
  }
}

function clampInt(v: number, lo: number, hi: number): number {
  const r = Math.round(v);
  return r < lo ? lo : r > hi ? hi : r;
}

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
