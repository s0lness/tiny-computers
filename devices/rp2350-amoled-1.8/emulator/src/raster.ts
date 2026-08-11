// Faithful port of the anti-aliased capsule rasteriser in firmware/main.c
// (draw_capsule, lines 136-182) and the dirty-rect bookkeeping around it.
//
// The firmware stores ink in the 6-bit green channel of an RGB565
// framebuffer (see px_to_gray/gray_to_px, main.c lines 116-128) because it
// has no spare 165KB for a separate coverage buffer. That round trip
// quantises every read-back to steps of 4 out of 255, which is a real but
// very fine hardware artifact. This emulator keeps ink as a plain 0..255
// grey buffer (0 = full black ink, 255 = white paper) with no such
// quantisation: the coverage math, the MIN composition and the geometry are
// all identical to the firmware, only the RGB565 bit-depth round trip is
// skipped. See README.md, "What is approximated".

import { PANEL_W, PANEL_H } from "./constants";

export interface DirtyRect {
  minX: number;
  minY: number;
  maxX: number;
  maxY: number;
}

export function emptyDirty(): DirtyRect {
  return { minX: PANEL_W, minY: PANEL_H, maxX: -1, maxY: -1 };
}

export function dirtyIsEmpty(d: DirtyRect): boolean {
  return d.maxX < d.minX || d.maxY < d.minY;
}

export function fullDirty(): DirtyRect {
  return { minX: 0, minY: 0, maxX: PANEL_W - 1, maxY: PANEL_H - 1 };
}

// Merges src into dst in place (dst grows to cover both). Used to accumulate
// several engine ticks' worth of dirty rects between animation frames.
export function mergeDirty(dst: DirtyRect, src: DirtyRect): void {
  if (dirtyIsEmpty(src)) return;
  growDirty(dst, src.minX, src.minY, src.maxX, src.maxY);
}

function growDirty(d: DirtyRect, minX: number, minY: number, maxX: number, maxY: number) {
  if (minX < d.minX) d.minX = minX;
  if (minY < d.minY) d.minY = minY;
  if (maxX > d.maxX) d.maxX = maxX;
  if (maxY > d.maxY) d.maxY = maxY;
}

export class Panel {
  // 0 = black ink, 255 = white paper. One byte per pixel, row-major.
  readonly gray: Uint8ClampedArray;

  constructor() {
    this.gray = new Uint8ClampedArray(PANEL_W * PANEL_H).fill(255);
  }

  clear(): void {
    this.gray.fill(255);
  }

  // Direct port of draw_capsule(). ax/ay/r0 -> bx/by/r1, a round-capped
  // capsule with linearly interpolated radius, composited with MIN
  // (darkest wins). This is the load-bearing part of the whole emulator:
  // do not switch this to alpha blending, see main.c's comment on why that
  // reintroduces banding.
  drawCapsule(
    ax: number, ay: number, r0: number,
    bx: number, by: number, r1: number,
    dirty: DirtyRect
  ): void {
    const maxR = Math.max(r0, r1) + 1.0;
    let minX = Math.floor(Math.min(ax, bx) - maxR);
    let maxX = Math.ceil(Math.max(ax, bx) + maxR);
    let minY = Math.floor(Math.min(ay, by) - maxR);
    let maxY = Math.ceil(Math.max(ay, by) + maxR);
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > PANEL_W - 1) maxX = PANEL_W - 1;
    if (maxY > PANEL_H - 1) maxY = PANEL_H - 1;
    if (minX > maxX || minY > maxY) return;

    const abx = bx - ax, aby = by - ay;
    const abLenSq = abx * abx + aby * aby;
    const gray = this.gray;

    for (let iy = minY; iy <= maxY; iy++) {
      const py = iy + 0.5;
      const row = iy * PANEL_W;
      for (let ix = minX; ix <= maxX; ix++) {
        const px = ix + 0.5;
        let t = 0.0;
        if (abLenSq > 0.0001) {
          t = ((px - ax) * abx + (py - ay) * aby) / abLenSq;
          if (t < 0.0) t = 0.0;
          else if (t > 1.0) t = 1.0;
        }
        const cx = ax + abx * t, cy = ay + aby * t;
        const dx = px - cx, dy = py - cy;
        const d = Math.sqrt(dx * dx + dy * dy);
        const r = r0 + (r1 - r0) * t;
        let coverage = r + 0.5 - d;
        if (coverage <= 0.0) continue;
        if (coverage > 1.0) coverage = 1.0;
        const ink = Math.round((1.0 - coverage) * 255.0);

        const idx = row + ix;
        const cur = gray[idx]!;
        if (ink < cur) gray[idx] = ink;
      }
    }

    growDirty(dirty, minX, minY, maxX, maxY);
  }

  // Writes the grey buffer into an RGBA ImageData, restricted to the given
  // rect (defaults to the full panel). Mirrors the firmware treating grey
  // as neutral (R=G=B).
  blitTo(img: ImageData, rect?: DirtyRect): void {
    const r = rect && !dirtyIsEmpty(rect) ? rect : { minX: 0, minY: 0, maxX: PANEL_W - 1, maxY: PANEL_H - 1 };
    const gray = this.gray;
    const data = img.data;
    for (let y = r.minY; y <= r.maxY; y++) {
      let idx = y * PANEL_W + r.minX;
      let o = idx * 4;
      for (let x = r.minX; x <= r.maxX; x++, idx++, o += 4) {
        const g = gray[idx]!;
        data[o] = g;
        data[o + 1] = g;
        data[o + 2] = g;
        data[o + 3] = 255;
      }
    }
  }
}
