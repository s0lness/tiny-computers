/**
 * Converts a real handwriting capture (from the local tldraw rig) into the
 * firmware's stroke table. No font is involved anywhere in this path: the
 * geometry is whatever the hand actually drew.
 *
 *   bun tools/capture-server.ts          # draw, press "Send to device"
 *   bun tools/gen-from-capture.ts        # writes preview/ and firmware/strokes.h
 */

import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { deflateSync } from "node:zlib";
import { join, dirname } from "node:path";

import { ingest, computeRadii, pointX, pointY, radii, pointCount } from "./tldraw-freehand/core";
import type { StrokeOptions } from "./tldraw-freehand/types";

const ROOT = join(import.meta.dir, "..");
const PANEL_W = 368;
const PANEL_H = 448;

type Pt = { x: number; y: number; z?: number };
type InkPt = { x: number; y: number; r: number };

const args = process.argv.slice(2);
const argOf = (n: string, d: string) => {
  const i = args.indexOf(`--${n}`);
  return i >= 0 && args[i + 1] ? args[i + 1] : d;
};
const has = (n: string) => args.includes(`--${n}`);

const margin = parseFloat(argOf("margin", "14"));
const strokeScale = parseFloat(argOf("stroke-scale", "1"));
const rotate = has("rotate");
const keepDots = has("keep-dots");

// ---------------------------------------------------------------- tldraw ink

const easeOutSine = (t: number) => Math.sin((t * Math.PI) / 2);

function modulate(v: number, [lo, hi]: number[], [min, max]: number[], clamp: boolean) {
  const t = (v - lo) / (hi - lo);
  let r = min + t * (max - min);
  if (clamp) r = max < min ? Math.max(Math.min(r, min), max) : Math.min(Math.max(r, min), max);
  return r;
}

// From tldraw/tldraw packages/tldraw/src/lib/shapes/draw/getPath.ts
function drawOptions(strokeWidth: number, isPen: boolean): StrokeOptions {
  const PEN_EASING = (t: number) => t * 0.65 + Math.sin((t * Math.PI) / 2) * 0.35;
  return isPen
    ? {
        size: 1 + strokeWidth * 1.2,
        thinning: 0.62,
        streamline: 0.62,
        smoothing: 0.62,
        simulatePressure: false,
        easing: PEN_EASING,
        last: true,
      }
    : {
        size: strokeWidth,
        thinning: 0.5,
        streamline: modulate(strokeWidth, [9, 16], [0.64, 0.74], true),
        smoothing: 0.62,
        easing: easeOutSine,
        simulatePressure: true,
        last: true,
      };
}

// tldraw's own default draw width per size step, at canvas scale.
const STROKE_WIDTH: Record<string, number> = { s: 3, m: 4.5, l: 6, xl: 11 };

function ink(points: Pt[], strokeWidth: number, isPen: boolean): InkPt[] {
  const options = drawOptions(strokeWidth, isPen);
  ingest(points, options);
  computeRadii(options);
  const out: InkPt[] = [];
  for (let i = 0; i < pointCount; i++) out.push({ x: pointX[i], y: pointY[i], r: radii[i] });
  return densify(out);
}

/**
 * The renderer stamps a disc per point, so consecutive points have to overlap
 * or the stroke breaks into a dotted line. A fast flick of the hand leaves
 * samples several pixels apart, which is exactly where that showed up. Walk
 * each gap and insert points, interpolating the radius, so the stamped discs
 * always overlap and the stroke is solid at any writing speed.
 */
function densify(s: InkPt[]): InkPt[] {
  if (s.length < 2) return s;
  const out: InkPt[] = [s[0]];
  for (let i = 1; i < s.length; i++) {
    const a = s[i - 1];
    const b = s[i];
    const d = Math.hypot(b.x - a.x, b.y - a.y);
    // Step well under the brush radius so discs overlap rather than touch.
    const step = Math.max(0.6, Math.min(a.r, b.r) * 0.5);
    const n = Math.floor(d / step);
    for (let k = 1; k < n; k++) {
      const t = k / n;
      out.push({
        x: a.x + (b.x - a.x) * t,
        y: a.y + (b.y - a.y) * t,
        r: a.r + (b.r - a.r) * t,
      });
    }
    out.push(b);
  }
  return out;
}

// -------------------------------------------------------------------- PNG

let crcTable: number[] | null = null;
function crc32(buf: Buffer) {
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
  for (let y = 0; y < h; y++) {
    raw[y * (w + 1)] = 0;
    for (let x = 0; x < w; x++) raw[y * (w + 1) + 1 + x] = gray[y * w + x];
  }
  const chunk = (type: string, data: Buffer) => {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const body = Buffer.concat([Buffer.from(type, "ascii"), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(body) >>> 0);
    return Buffer.concat([len, body, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8;
  ihdr[9] = 0;
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(
    path,
    Buffer.concat([
      Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
      chunk("IHDR", ihdr),
      chunk("IDAT", deflateSync(raw, { level: 9 })),
      chunk("IEND", Buffer.alloc(0)),
    ]),
  );
}

function raster(strokes: InkPt[][]): Uint8Array {
  const img = new Uint8Array(PANEL_W * PANEL_H).fill(255);
  for (const s of strokes) {
    for (const p of s) {
      const r = Math.max(0.5, p.r);
      const x0 = Math.max(0, Math.floor(p.x - r));
      const x1 = Math.min(PANEL_W - 1, Math.ceil(p.x + r));
      const y0 = Math.max(0, Math.floor(p.y - r));
      const y1 = Math.min(PANEL_H - 1, Math.ceil(p.y + r));
      for (let y = y0; y <= y1; y++)
        for (let x = x0; x <= x1; x++)
          if (Math.hypot(x + 0.5 - p.x, y + 0.5 - p.y) <= r) img[y * PANEL_W + x] = 0;
    }
  }
  return img;
}

// -------------------------------------------------------------------- main

const capture = JSON.parse(readFileSync(join(ROOT, "capture", "handwriting.json"), "utf8"));

type Raw = { points: Pt[]; size: string; isPen: boolean };
let raw: Raw[] = capture.strokes.map((s: any) => ({
  size: s.size ?? "m",
  isPen: !!s.isPen,
  points: s.points.map((p: Pt) => ({ x: s.x + p.x, y: s.y + p.y, z: p.z })),
}));

// A one-point stroke is a stray tap, not a mark. Keeping it would also drag the
// bounding box out and shrink everything else, so drop unless asked otherwise.
const dropped = raw.filter((s) => s.points.length < 2).length;
if (!keepDots) raw = raw.filter((s) => s.points.length >= 2);

if (rotate) {
  for (const s of raw) s.points = s.points.map((p) => ({ x: p.y, y: -p.x, z: p.z }));
}

let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
for (const s of raw)
  for (const p of s.points) {
    minX = Math.min(minX, p.x); maxX = Math.max(maxX, p.x);
    minY = Math.min(minY, p.y); maxY = Math.max(maxY, p.y);
  }

const srcW = maxX - minX;
const srcH = maxY - minY;
const fit = Math.min((PANEL_W - 2 * margin) / srcW, (PANEL_H - 2 * margin) / srcH);
const offX = (PANEL_W - srcW * fit) / 2 - minX * fit;
const offY = (PANEL_H - srcH * fit) / 2 - minY * fit;

const strokes = raw.map((s) =>
  ink(
    s.points.map((p) => ({ x: p.x * fit + offX, y: p.y * fit + offY, z: p.z })),
    STROKE_WIDTH[s.size] * fit * strokeScale,
    s.isPen,
  ),
);

const total = strokes.reduce((n, s) => n + s.length, 0);
console.log(
  `captured ${capture.strokes.length} strokes` +
    (dropped && !keepDots ? ` (dropped ${dropped} single-point)` : "") +
    `\nsource ${srcW.toFixed(0)}x${srcH.toFixed(0)} -> fit ${(fit * 100).toFixed(1)}%` +
    `  used ${(srcW * fit).toFixed(0)}x${(srcH * fit).toFixed(0)} of ${PANEL_W}x${PANEL_H}` +
    `\nstrokes ${strokes.length}  points ${total}`,
);

writePng(join(ROOT, "preview", "preview-capture.png"), PANEL_W, PANEL_H, raster(strokes));
console.log("wrote preview/preview-capture.png");

const h: string[] = [];
h.push("// Generated by tools/gen-from-capture.ts from a real handwriting capture.");
h.push("// Geometry is the hand that drew it; radii come from tldraw's freehand pipeline.");
h.push("#ifndef STROKES_H");
h.push("#define STROKES_H");
h.push("");
h.push("#include <stdint.h>");
h.push("");
h.push(`#define STROKE_COUNT ${strokes.length}`);
h.push(`#define POINT_COUNT ${total}`);
h.push("");
h.push("typedef struct { int16_t x, y; uint8_t r; } stroke_pt_t;");
h.push("");
h.push("static const stroke_pt_t stroke_points[POINT_COUNT] = {");
for (const s of strokes) {
  h.push(
    "    " +
      s
        .map((p) => {
          const r = Math.min(255, Math.max(8, Math.round(p.r * 16)));
          return `{${Math.round(p.x * 16)},${Math.round(p.y * 16)},${r}}`;
        })
        .join(",") +
      ",",
  );
}
h.push("};");
h.push("");
h.push(`static const uint16_t stroke_offsets[STROKE_COUNT + 1] = {`);
{
  const offs: number[] = [];
  let acc = 0;
  for (const s of strokes) { offs.push(acc); acc += s.length; }
  offs.push(acc);
  for (let i = 0; i < offs.length; i += 16) h.push("    " + offs.slice(i, i + 16).join(",") + ",");
}
h.push("};");
h.push("");
h.push("#endif // STROKES_H");

mkdirSync(join(ROOT, "firmware"), { recursive: true });
writeFileSync(join(ROOT, "firmware", "strokes.h"), h.join("\n") + "\n");
console.log("wrote firmware/strokes.h");
