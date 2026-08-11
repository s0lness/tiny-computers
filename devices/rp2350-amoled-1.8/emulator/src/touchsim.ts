// Simulates the FT3168 touch controller's imperfections on top of real
// pointer input, because the stroke pipeline in main.c exists specifically
// to survive them (see its comments on glitches, dropouts and strays). This
// is NOT a port of firmware code: the real controller's failure modes are
// not documented registers, they are what main.c's own comments and the
// task brief describe from having watched it on real hardware:
//
//   - it reports at a fixed rate (default 60Hz here; main.c sets the scan
//     period register to target 100Hz, see FT3168_PERIOD_MS)
//   - it drops contact mid-stroke, arriving as a run of zero-finger reports
//     (main.c's LIFT_DEBOUNCE_MS exists to survive exactly this)
//   - it occasionally reports a single stray contact at a wrong position
//     while nothing is touching it (main.c's two-report pendingStart exists
//     to reject exactly this)
//
// Dropouts and strays are modelled as a per-report Bernoulli draw with
// probability rate*periodSec, which approximates a Poisson process at the
// rates the sliders name (events per second).

import { PANEL_W, PANEL_H, type TouchSimConfig } from "./constants";

export interface TouchReport {
  fingers: number; // 0 or 1, this board only ever has one finger
  x: number;
  y: number;
}

function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}

export class TouchSim {
  cfg: TouchSimConfig;

  // Real input, set every tick from actual pointer events.
  private realDown = false;
  private realX = 0;
  private realY = 0;

  // The controller's last-reported state, refreshed only at reportRateHz.
  private repFingers = 0;
  private repX = 0;
  private repY = 0;
  private nextReportAt = 0;

  // Stats, for the on-page profiler-style readout.
  simDropouts = 0;
  simStrays = 0;

  constructor(cfg: TouchSimConfig) {
    this.cfg = cfg;
  }

  setPointer(down: boolean, x: number, y: number): void {
    this.realDown = down;
    this.realX = clamp(x, 0, PANEL_W - 1);
    this.realY = clamp(y, 0, PANEL_H - 1);
  }

  resetStats(): void {
    this.simDropouts = 0;
    this.simStrays = 0;
  }

  // Called every engine tick; only actually refreshes the report at the
  // configured scan rate, mirroring a controller that is polled far faster
  // than it actually samples.
  poll(nowMs: number): TouchReport {
    const periodMs = 1000 / Math.max(1, this.cfg.reportRateHz);
    if (nowMs >= this.nextReportAt) {
      this.nextReportAt = nowMs + periodMs;
      this.refresh(periodMs / 1000);
    }
    return { fingers: this.repFingers, x: this.repX, y: this.repY };
  }

  private refresh(periodSec: number): void {
    if (this.realDown) {
      if (this.cfg.dropoutsEnabled && Math.random() < this.cfg.dropoutsPerSec * periodSec) {
        // Contact lost for this report. Real FT3168_Get_Point does not touch
        // the struct when finger count is 0, so the last coordinate is left
        // in place; the report's x/y are irrelevant since fingers=0.
        this.repFingers = 0;
        this.simDropouts++;
        return;
      }
      this.repFingers = 1;
      this.repX = this.realX;
      this.repY = this.realY;
      return;
    }

    // No real touch down: maybe a stray single-report contact at a nearby
    // wrong position, otherwise genuinely nothing.
    if (this.cfg.straysEnabled && Math.random() < this.cfg.straysPerSec * periodSec) {
      const jitter = 40;
      this.repFingers = 1;
      this.repX = clamp(this.repX + (Math.random() * 2 - 1) * jitter, 0, PANEL_W - 1);
      this.repY = clamp(this.repY + (Math.random() * 2 - 1) * jitter, 0, PANEL_H - 1);
      this.simStrays++;
      return;
    }
    this.repFingers = 0;
  }
}
