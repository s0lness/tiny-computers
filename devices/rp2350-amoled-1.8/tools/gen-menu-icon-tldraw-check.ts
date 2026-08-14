/**
 * Design-time check: feed the coil and chrono-crown centrelines through
 * tldraw's own vendored freehand pipeline (tools/tldraw-freehand/, the
 * library the owner asked to be found - it is already in this tree, see
 * that folder's own header comment and AGENTS.md's "The ink itself is
 * tldraw's, not ours") and render what ITS pressure/taper model would do
 * with the same paths, for comparison against the hand-tuned radii actually
 * shipped in firmware/apps/menu.c (draw_icon_chrono's crown, a fixed
 * 5px->2px linear taper; draw_icon_timer_coil's spiral, a sine hump between
 * COIL_W_MIN and COIL_W_MAX).
 *
 * This does NOT feed the firmware: shapes.h's own capsule/tapered-quad
 * primitives already implement the same "signed distance -> AA coverage,
 * MIN-composited" ink technique sketch.c derived FROM tldraw's model (see
 * AGENTS.md's "Pen shape follows tldraw's draw tool" paragraph) - re-running
 * tldraw's JS pipeline on real silicon would cost a JS runtime this device
 * does not have, for a result shapes.c's own C already produces. What this
 * script buys is a second opinion on the SHAPE of the taper: does the
 * library built for exactly this kind of ink agree the hand-picked numbers
 * above read as a natural stroke, not an invented one.
 *
 *   bun tools/gen-menu-icon-tldraw-check.ts
 */
import { writeFileSync, mkdirSync } from "node:fs";
import { deflateSync } from "node:zlib";
import { join, dirname } from "node:path";

import { ingest, computeRadii, pointX, pointY, radii, pointCount } from "./tldraw-freehand/core";
import type { StrokeOptions } from "./tldraw-freehand/types";

const ROOT = join(import.meta.dir, "..");
const W = 200, H = 200;

// Same tldraw "draw tool, mouse/finger" options gen-strokes.ts uses
// (packages/tldraw/src/lib/shapes/draw/getPath.ts's simulatePressureSettings),
// just with `size` matched to each stroke's own peak width below.
function drawOptions(size: number): StrokeOptions {
  const easeOutSine = (t: number) => Math.sin((t * Math.PI) / 2);
  return { size, thinning: 0.5, streamline: 0.65, smoothing: 0.62, easing: easeOutSine, simulatePressure: true, last: true };
}

type Pt = { x: number; y: number };
type InkPt = { x: number; y: number; r: number };

function inkStroke(centreline: Pt[], size: number): InkPt[] {
  ingest(centreline, drawOptions(size));
  computeRadii(drawOptions(size));
  const out: InkPt[] = [];
  for (let i = 0; i < pointCount; i++) out.push({ x: pointX[i], y: pointY[i], r: radii[i] });
  return out;
}

function raster(strokes: InkPt[][]): Uint8Array {
  const img = new Uint8Array(W * H).fill(255);
  const dot = (cx: number, cy: number, r: number) => {
    const x0 = Math.max(0, Math.floor(cx - r)), x1 = Math.min(W - 1, Math.ceil(cx + r));
    const y0 = Math.max(0, Math.floor(cy - r)), y1 = Math.min(H - 1, Math.ceil(cy + r));
    for (let y = y0; y <= y1; y++)
      for (let x = x0; x <= x1; x++)
        if (Math.hypot(x + 0.5 - cx, y + 0.5 - cy) <= r) img[y * W + x] = 0;
  };
  for (const s of strokes) for (const p of s) dot(p.x, p.y, Math.max(0.5, p.r));
  return img;
}

let crcTable: number[] | null = null;
function crc32(buf: Buffer): number {
  if (!crcTable) {
    crcTable = [];
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c >>> 0;
    }
  }
  let c = 0xffffffff;
  for (const b of buf) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
function writePng(path: string, w: number, h: number, gray: Uint8Array) {
  const raw = Buffer.alloc((w + 1) * h);
  for (let y = 0; y < h; y++) { raw[y * (w + 1)] = 0; raw.set(gray.subarray(y * w, y * w + w), y * (w + 1) + 1); }
  const idat = deflateSync(raw, { level: 9 });
  const chunk = (type: string, data: Buffer) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const body = Buffer.concat([Buffer.from(type, "ascii"), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(body) >>> 0);
    return Buffer.concat([len, body, crc]);
  };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 0;
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr), chunk("IDAT", idat), chunk("IEND", Buffer.alloc(0)),
  ]));
}

// ---- the coil centreline, same math as draw_icon_timer_coil(), scaled up
// to this 200x200 check canvas so tldraw's own streamline/size logic (which
// is scale-sensitive) sees a stroke of a realistic size rather than a tiny
// one.
const SCALE = 2.0;
const coil: Pt[] = [];
{
  const TURNS = 2.25, R_OUTER = 40 * SCALE, R_INNER = 6 * SCALE, POINTS = 72;
  const cx = 100, cy = 100;
  for (let i = 0; i < POINTS; i++) {
    const t = i / (POINTS - 1);
    const theta = t * TURNS * 2 * Math.PI;
    const r = R_OUTER + (R_INNER - R_OUTER) * t;
    coil.push({ x: cx + r * Math.sin(theta), y: cy - r * Math.cos(theta) });
  }
}
const coilInk = inkStroke(coil, 11 * SCALE); // 11 = 2*COIL_W_MAX
writePng(join(ROOT, "preview", "tldraw-check-coil.png"), W, H, raster([coilInk]));
console.log("wrote preview/tldraw-check-coil.png");
const coilRs = coilInk.map((p) => p.r / SCALE);
console.log(
  `  coil, tldraw's own radii (design units): start=${coilRs[0].toFixed(1)} peak=${Math.max(...coilRs).toFixed(1)} end=${coilRs[coilRs.length - 1].toFixed(1)}` +
  ` -- shipped firmware uses a fixed sine hump ${1.5}->${5.0}->${1.5}`,
);

// ---- the chrono crown centreline: base to tip, straight.
const crown: Pt[] = [{ x: 100, y: 100 + 16 * SCALE }, { x: 100, y: 100 - 16 * SCALE }];
const crownInk = inkStroke(crown, 10 * SCALE); // 10 = CHRONO_STROKE
writePng(join(ROOT, "preview", "tldraw-check-crown.png"), W, H, raster([crownInk]));
console.log("wrote preview/tldraw-check-crown.png");
const crownRs = crownInk.map((p) => p.r / SCALE);
console.log(
  `  crown, tldraw's own radii (design units): start=${crownRs[0].toFixed(1)} peak=${Math.max(...crownRs).toFixed(1)} end=${crownRs[crownRs.length - 1].toFixed(1)}` +
  ` -- shipped firmware uses a fixed linear taper 5.0->2.0`,
);

// ---- THE LIMITATION FOUND HERE, worth recording rather than silently
// working around: with the "draw tool" options above (no start/end.taper -
// the same options gen-strokes.ts uses for real handwriting), the pipeline
// derives width from the DISTANCE between consecutive input points relative
// to `size` (core.ts's computeRadii, the `sp`/`rp` pressure-simulation
// math). A real captured stroke naturally slows at its own start and end,
// which is what tapers it; both centrelines checked here are procedural and
// EVENLY spaced (a spiral sampled at constant arc-length-ish steps, a
// straight line), so there is no speed signal to read and both come out
// close to constant width instead - confirmed above (crown: 6.4->6.4->5.5,
// barely tapered; coil: 7.0 rising to a flat ~9.1, no taper at the tail at
// all). Re-running the SAME coil centreline with `start:{taper:true},
// end:{taper:true}` added DOES taper it correctly (0 -> peak -> 0, checked
// separately) - so this is not a hole in the library, it is the "draw
// tool" preset choosing not to opt in, because for its real use case
// (mouse/finger capture) it does not need to. A synthetic centreline with
// no genuine speed variation needs the explicit taper options; this is the
// one thing worth knowing before feeding a procedural shape (as opposed to
// a captured one) through this pipeline again.
console.log(
  "\n  LIMITATION: the draw-tool preset (no start/end.taper) does not taper a\n" +
  "  procedurally-sampled, evenly-spaced centreline on its own - it needs real\n" +
  "  speed variation to read pressure from. start:{taper:true},end:{taper:true}\n" +
  "  fixes it (checked separately, not shipped here). This is why menu.c's\n" +
  "  crown/coil radii are hand-specified in C rather than read back from this\n" +
  "  pipeline at build time - same choice draw_icon_sketch's own fr=[2,3,4,3,1]\n" +
  "  already made, for the same reason.",
);
