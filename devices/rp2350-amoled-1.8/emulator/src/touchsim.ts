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
//   - it occasionally reports a real, HELD contact at a position far from
//     where the finger actually is, sometimes for more than one report in
//     a row before correcting itself
//
// That last one (positionJitter*) was added 2026-08-14, later than the
// first three, and for a sharper reason than "controllers are imperfect in
// general": the palette feature's own hold gesture (firmware/apps/sketch.c)
// shipped, worked in the emulator, and did not open at all on real
// hardware. A TOUCH_POLL_SELFTEST session with the owner's finger held
// deliberately still measured splits=10 (sketch.c's own "confirmed jump,
// close this stroke and open a new one" counter) in 40 seconds - the
// reported position was jumping hundreds of pixels while the finger sat
// still, and this file had no way to produce that: dropouts stop reports
// arriving, strays fabricate a contact where there is none, but neither
// ever moves the reported position of a contact that IS genuinely down.
// See feature-sketch-palette's own repro test
// (emulator/wasm/tests/repro-touch-dropout-palette-open.ts) for how this
// was calibrated against that measured splits=10/40s figure, and decision
// 0008's own argument for why the honest fix, when a filter never fires in
// testing, is to teach the simulation the real behaviour rather than trust
// a threshold that was never actually exercised.
//
// Dropouts, strays and jitter-episode-starts are all modelled as a
// per-report Bernoulli draw with probability rate*periodSec, which
// approximates a Poisson process at the rates the sliders (or config
// fields) name (events per second).
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

  // In-progress jitter episode, if any - see the header comment and
  // refresh()'s own comment for what this models and why it holds a
  // position rather than picking a fresh random one every report.
  private jitterRemaining = 0;
  private jitterOffsetX = 0;
  private jitterOffsetY = 0;

  // Stats, for the on-page profiler-style readout.
  simDropouts = 0;
  simStrays = 0;
  simJitterEpisodes = 0;

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
    this.simJitterEpisodes = 0;
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
        // An in-progress jitter episode is left exactly where it was
        // (jitterRemaining untouched): a dropout does not un-stick a
        // wrongly-locked centroid, it just means this one report is lost
        // too, same as it would for a truthful one.
        this.repFingers = 0;
        this.simDropouts++;
        return;
      }

      // Start a new jitter episode if none is running. Picked here, not
      // continuously, so a whole episode shares ONE displacement direction
      // and magnitude - see the small per-report wobble below for why
      // consecutive reports in the same episode are close to each other but
      // not identical.
      if (this.jitterRemaining <= 0 && this.cfg.positionJitterEnabled &&
          Math.random() < this.cfg.positionJitterPerSec * periodSec) {
        const angle = Math.random() * Math.PI * 2;
        const mag = this.cfg.positionJitterMinPx +
          Math.random() * (this.cfg.positionJitterMaxPx - this.cfg.positionJitterMinPx);
        this.jitterOffsetX = Math.cos(angle) * mag;
        this.jitterOffsetY = Math.sin(angle) * mag;
        // 1..positionJitterMaxHoldReports reports. A run of exactly 1 is a
        // lone bad reading (sketch.c counts it a "glitch": too far to
        // accept, never confirmed by a second one nearby). A run of 2+ is
        // what sketch.c can "confirm" into a "split" - the SAME wrong spot
        // has to be reported again, not merely a repeat of the true one.
        this.jitterRemaining = 1 + Math.floor(Math.random() * this.cfg.positionJitterMaxHoldReports);
        this.simJitterEpisodes++;
      }

      if (this.jitterRemaining > 0) {
        this.jitterRemaining--;
        // A small wobble around the episode's own offset, not the exact
        // same pixel every report: sketch.c's own newReport test is
        // coordinate equality, and an exact repeat would read as "nothing
        // changed" rather than as a second sighting of the same wrong spot
        // to confirm against (see CONFIRM_PX in sketch.c - a few pixels of
        // wobble stays comfortably inside it).
        const wobble = 6;
        this.repFingers = 1;
        this.repX = clamp(this.realX + this.jitterOffsetX + (Math.random() * 2 - 1) * wobble, 0, this.panelW - 1);
        this.repY = clamp(this.realY + this.jitterOffsetY + (Math.random() * 2 - 1) * wobble, 0, this.panelH - 1);
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
