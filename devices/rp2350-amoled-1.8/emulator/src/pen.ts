// Faithful port of the pen state machine in firmware/main.c: stroke_begin,
// stroke_sample, stroke_end (lines 211-292), and pressure_to_radius
// (lines 217-230). This is tldraw's draw-tool model reimplemented from the
// firmware's own reimplementation, not from tldraw's source: streamline
// toward the raw point, derive pressure from speed, rate-limit pressure,
// ease it into a radius, taper both ends.
//
// Four constants here are lifted from main.c but are NOT exposed as sliders
// (the task's tunable list did not include them): the 0.35 start-radius
// factor at arc==0, the 0.35/0.65 start-taper blend, the three end-taper
// scales [0.7, 0.45, 0.25] and their 1.2px step, and the 1..8px radius
// clamp. Keep these in sync with main.c by hand if they ever change there.

import type { Tunables } from "./constants";
import type { Panel, DirtyRect } from "./raster";

const RADIUS_MIN = 1.0;
const RADIUS_MAX = 8.0;
const START_RADIUS_FACTOR = 0.35; // stroke_begin: g_radius = pressure_to_radius(0.5) * 0.35
const START_TAPER_BASE = 0.35; // stroke_sample: r *= (0.35 + 0.65 * arc/START_TAPER_LEN)
const START_TAPER_SPAN = 0.65;
const END_TAPER_SCALES = [0.7, 0.45, 0.25] as const;
const END_TAPER_STEP_PX = 1.2;

function easeOutSine(t: number): number {
  return Math.sin((t * Math.PI) / 2.0);
}

function pressureToRadius(pressure: number, c: Tunables): number {
  let r = c.PEN_SIZE * easeOutSine(0.5 - c.PEN_THINNING * (0.5 - pressure));
  if (r < RADIUS_MIN) r = RADIUS_MIN;
  if (r > RADIUS_MAX) r = RADIUS_MAX;
  return r;
}

export class PenState {
  sx = 0;
  sy = 0;
  pressure = 0.5;
  arcLen = 0;
  radius = 0;
  dirX = 0;
  dirY = 0;

  reset(): void {
    this.pressure = 0.5;
    this.arcLen = 0;
    this.radius = 0;
    this.dirX = 0;
    this.dirY = 0;
  }

  // stroke_begin(). Draws an initial dot at the start radius.
  begin(panel: Panel, dirty: DirtyRect, x: number, y: number, c: Tunables): void {
    this.sx = x;
    this.sy = y;
    this.pressure = 0.5;
    this.arcLen = 0.0;
    this.dirX = 0.0;
    this.dirY = 0.0;
    this.radius = pressureToRadius(this.pressure, c) * START_RADIUS_FACTOR;
    panel.drawCapsule(this.sx, this.sy, this.radius, this.sx, this.sy, this.radius, dirty);
  }

  // stroke_sample(). `bridge` marks the first sample after a dropout gap:
  // it snaps straight to the report instead of streamlining toward it, and
  // leaves pressure alone so width carries continuously across the gap.
  sample(panel: Panel, dirty: DirtyRect, x: number, y: number, bridge: boolean, c: Tunables): void {
    const prevX = this.sx, prevY = this.sy, prevR = this.radius;

    const k = bridge ? 1.0 : 1.0 - c.STREAMLINE;
    const nx = this.sx + (x - this.sx) * k;
    const ny = this.sy + (y - this.sy) * k;
    const dist = Math.sqrt((nx - prevX) * (nx - prevX) + (ny - prevY) * (ny - prevY));
    if (dist < c.DEDUPE_PX) return; // finger resting: drop the jitter, keep old state

    this.sx = nx;
    this.sy = ny;

    if (!bridge) {
      const target = 1.0 - Math.min(1.0, dist / c.SPEED_MAX);
      this.pressure += (target - this.pressure) * c.PRESSURE_LERP;
    }
    let r = pressureToRadius(this.pressure, c);

    this.arcLen += dist;
    if (this.arcLen < c.START_TAPER_LEN) {
      r *= START_TAPER_BASE + START_TAPER_SPAN * (this.arcLen / c.START_TAPER_LEN);
    }

    panel.drawCapsule(prevX, prevY, prevR, this.sx, this.sy, r, dirty);

    this.radius = r;
    this.dirX = (nx - prevX) / dist;
    this.dirY = (ny - prevY) / dist;
  }

  // stroke_end(). Three shrinking capsules stepping along the last travel
  // direction, tapering the stroke to a point.
  end(panel: Panel, dirty: DirtyRect): void {
    let curX = this.sx, curY = this.sy, curR = this.radius;
    for (const scale of END_TAPER_SCALES) {
      const nx = curX + this.dirX * END_TAPER_STEP_PX;
      const ny = curY + this.dirY * END_TAPER_STEP_PX;
      const nr = this.radius * scale;
      panel.drawCapsule(curX, curY, curR, nx, ny, nr, dirty);
      curX = nx;
      curY = ny;
      curR = nr;
    }
  }
}
