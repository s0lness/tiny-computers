// Simulates a touch controller's real-world imperfections on top of clean
// pointer input, because firmware that handles touch robustly (dropout
// bridging, glitch rejection, stroke-start confirmation) only exercises
// that code against input that actually misbehaves. A mouse drag never
// does. This is NOT a port of any firmware: the failure modes modelled here
// are generic properties real capacitive touch controllers have (this
// project's own FT3168 measured 1-3 dropouts/sec while drawing, which is
// where the default rate below comes from), not registers or #defines from
// a specific chip.
//
//   - it reports at a fixed rate (default 60Hz), not continuously
//   - it drops contact mid-stroke, arriving as a run of zero-finger reports
//   - it occasionally reports a single stray contact at a wrong position
//     while nothing is touching it
//
// Dropouts and strays are modelled as a per-report Bernoulli draw with
// probability rate*periodSec, which approximates a Poisson process at the
// rates the sliders name (events per second).
//
// Panel bounds are passed in rather than imported as constants, because
// this emulator is generic across devices (panel size comes from the
// firmware's own emu_device() descriptor, see wasm.ts): the one edit this
// file needed for the rewrite. Everything else here is unchanged from the
// original rp2350-amoled-1.8 emulator.

import type { TouchSimConfig } from "./constants";

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
  private panelW: number;
  private panelH: number;

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

  constructor(cfg: TouchSimConfig, panelW: number, panelH: number) {
    this.cfg = cfg;
    this.panelW = panelW;
    this.panelH = panelH;
  }

  // Called after a wasm module reload, in case the new device declares a
  // different panel size.
  setBounds(panelW: number, panelH: number): void {
    this.panelW = panelW;
    this.panelH = panelH;
  }

  setPointer(down: boolean, x: number, y: number): void {
    this.realDown = down;
    this.realX = clamp(x, 0, this.panelW - 1);
    this.realY = clamp(y, 0, this.panelH - 1);
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
      this.repX = clamp(this.repX + (Math.random() * 2 - 1) * jitter, 0, this.panelW - 1);
      this.repY = clamp(this.repY + (Math.random() * 2 - 1) * jitter, 0, this.panelH - 1);
      this.simStrays++;
      return;
    }
    this.repFingers = 0;
  }
}
