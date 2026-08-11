/**
 * Generates handwriting stroke paths for the RP2350 AMOLED animation.
 *
 * Reads a Hershey single-stroke font, lays the message out over the 368x448
 * panel, humanises it so it reads as written rather than plotted, then emits
 * both a PNG preview (to judge the look without flashing) and a C header.
 *
 *   bun tools/gen-strokes.ts --font scriptc --preview
 */

import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { deflateSync } from "node:zlib";
import { join, dirname } from "node:path";

// tldraw's own stroke pipeline, vendored verbatim from
// tldraw/tldraw @ packages/tldraw/src/lib/shapes/shared/freehand/.
// `ingest` applies streamline and builds the centreline; `computeRadii` fills
// the per-point radius from tldraw's pressure / thinning / taper model.
import { ingest, computeRadii, pointX, pointY, radii, pointCount } from "./tldraw-freehand/core";
import type { StrokeOptions } from "./tldraw-freehand/types";

const ROOT = join(import.meta.dir, "..");

const PANEL_W = 368;
const PANEL_H = 448;

// ---------------------------------------------------------------- Hershey

type Glyph = { left: number; right: number; strokes: Array<Array<[number, number]>> };

function parseJhf(text: string): Map<number, Glyph> {
  // Records are: 5-char id, 3-char vertex count, then 2 chars per vertex.
  // A record may wrap onto following lines, so consume by character budget.
  const lines = text.split(/\r?\n/);
  const glyphs = new Map<number, Glyph>();
  let ascii = 32;

  for (let i = 0; i < lines.length; i++) {
    let line = lines[i];
    if (line.trim() === "") continue;

    const count = parseInt(line.slice(5, 8).trim(), 10);
    if (!Number.isFinite(count)) continue;

    let data = line.slice(8);
    while (data.length < count * 2 && i + 1 < lines.length) {
      i++;
      data += lines[i];
    }

    const val = (c: string) => c.charCodeAt(0) - 82; // 'R'
    const left = val(data[0]);
    const right = val(data[1]);

    const strokes: Array<Array<[number, number]>> = [];
    let current: Array<[number, number]> = [];

    for (let v = 1; v < count; v++) {
      const a = data[v * 2];
      const b = data[v * 2 + 1];
      if (a === undefined || b === undefined) break;
      if (a === " " && b === "R") {
        if (current.length > 1) strokes.push(current);
        current = [];
        continue;
      }
      current.push([val(a), val(b)]);
    }
    if (current.length > 1) strokes.push(current);

    glyphs.set(ascii, { left, right, strokes });
    ascii++;
  }
  return glyphs;
}

// ------------------------------------------------------- deterministic noise

// Reproducible builds matter here: the same message must always produce the
// same header, so no Math.random anywhere.
function makeRng(seed: number) {
  let s = seed >>> 0;
  return () => {
    s ^= s << 13; s >>>= 0;
    s ^= s >> 17;
    s ^= s << 5; s >>>= 0;
    return s / 0x100000000;
  };
}

// ------------------------------------------------------------------ layout

type Pt = { x: number; y: number };
type Stroke = Pt[];

type LayoutOpts = {
  size: number; top: number; lineGap: number; seed: number;
  jitter: number; slant: number; tracking: number; rough: number; spaceScale: number;
  sizeVary: number; drift: number;
};

// Join two pen positions with a curve instead of lifting. Control point sits
// past the midpoint and below the baseline, which is what produces the loops
// and stray crossings you get writing fast without taking the pen off.
function connector(a: Pt, b: Pt, dip: number, rng: () => number): Pt[] {
  const dist = Math.hypot(b.x - a.x, b.y - a.y);
  const n = Math.max(2, Math.min(10, Math.round(dist / 2)));
  const mx = (a.x + b.x) / 2 + (rng() - 0.5) * dist * 0.3;
  const my = (a.y + b.y) / 2 + dip * (0.6 + rng() * 0.8);
  const out: Pt[] = [];
  for (let i = 1; i < n; i++) {
    const t = i / n;
    const u = 1 - t;
    out.push({
      x: u * u * a.x + 2 * u * t * mx + t * t * b.x,
      y: u * u * a.y + 2 * u * t * my + t * t * b.y,
    });
  }
  return out;
}

/**
 * Lays the message out as joined-up handwriting: every word becomes ONE
 * continuous path, because the thing that makes real handwriting look like
 * handwriting is that the pen does not come off the paper between letters.
 * Words are separated by genuine pen lifts; everything inside a word is
 * bridged with a connector curve.
 */
function layout(lines: string[], glyphs: Map<number, Glyph>, opts: LayoutOpts): Stroke[] {
  const rng = makeRng(opts.seed);
  const out: Stroke[] = [];
  const baseScale = opts.size / 21; // Hershey cap box is ~21 units

  lines.forEach((text, lineIndex) => {
    const advanceOf = (ch: string, g: Glyph, sc: number) =>
      (g.right - g.left) * sc * (ch === " " ? opts.spaceScale : 1) + opts.tracking;

    // Measure with the nominal scale so the line can be centred.
    let width = 0;
    for (const ch of text) {
      const g = glyphs.get(ch.charCodeAt(0));
      if (g) width += advanceOf(ch, g, baseScale);
    }
    width -= opts.tracking;

    let penX = (PANEL_W - width) / 2;
    const baseY = opts.top + lineIndex * opts.lineGap;

    const lineTilt = (rng() - 0.5) * 0.03;
    const lineLift = (rng() - 0.5) * 4;

    // Baseline wanders as a random walk rather than independent per-letter
    // noise, so the line sags and recovers the way a real one does.
    let walk = 0;

    let word: Stroke = [];
    const flush = () => {
      if (word.length > 1) out.push(word);
      word = [];
    };

    for (const ch of text) {
      const g = glyphs.get(ch.charCodeAt(0));
      if (!g) continue;

      if (ch === " ") {
        flush();
        penX += advanceOf(ch, g, baseScale);
        continue;
      }

      // Letters vary in size a lot in real writing; this is the single biggest
      // cue that it was not set in a font.
      const sc = baseScale * (1 + (rng() - 0.5) * opts.sizeVary);
      const advance = advanceOf(ch, g, sc) - opts.tracking;

      walk += (rng() - 0.5) * opts.drift;
      walk *= 0.82; // pull back toward the baseline so it cannot run away

      const dy = walk + (rng() - 0.5) * opts.jitter;
      const dx = (rng() - 0.5) * opts.jitter * 0.5;
      const rot = (rng() - 0.5) * 0.12;

      const roughPhase = rng() * Math.PI * 2;
      const roughFreq = 0.3 + rng() * 0.3;
      let arc = 0;

      const place = ([gx, gy]: [number, number]): Pt => {
        let x = (gx - g.left) * sc;
        let y = gy * sc;
        x += -y * opts.slant;

        const cx = advance / 2;
        const rx = x - cx;
        const nx = rx * Math.cos(rot) - y * Math.sin(rot) + cx;
        const ny = rx * Math.sin(rot) + y * Math.cos(rot);

        arc += 1;
        const wob = Math.sin(arc * roughFreq + roughPhase) * opts.rough;

        return {
          x: penX + nx + dx + wob,
          y: baseY + ny + dy + lineLift + (penX - PANEL_W / 2) * lineTilt + wob * 0.6,
        };
      };

      // Every stroke of the glyph joins the running word path, including bars
      // and dots: crossing a t or dotting an i in one pass is exactly what
      // fast handwriting does, and it is where the stray loops come from.
      for (const s of g.strokes) {
        const pts = s.map(place);
        if (pts.length === 0) continue;
        if (word.length > 0) {
          word.push(...connector(word[word.length - 1], pts[0], opts.size * 0.14, rng));
        }
        word.push(...pts);
      }

      penX += advance + opts.tracking;
    }
    flush();
  });

  return out;
}

/**
 * The thing that stops handwriting looking like a font: a real pen has mass.
 * The hand aims at where the letter should go, but the pen lags, overshoots,
 * and rounds off corners it cannot turn in time. Loops are not drawn on
 * purpose, they are what overshoot looks like.
 *
 * So instead of emitting the skeleton, we emit the trajectory of a virtual pen
 * chasing it: a damped second-order follower. The same letter shape produces a
 * different result every time because the pen arrives at it with a different
 * velocity, which is exactly the variation a font cannot give you.
 *
 * Low stiffness and light damping give a loose, loopy scrawl; raising either
 * tightens it back toward the skeleton.
 */
function penFollow(path: Stroke, stiffness: number, damping: number, lead: number): Stroke {
  if (path.length < 3) return path;

  const out: Stroke = [];
  let px = path[0].x;
  let py = path[0].y;
  let vx = 0;
  let vy = 0;

  // Keep chasing past the end so the pen settles instead of stopping dead.
  const total = path.length + 24;
  for (let i = 0; i < total; i++) {
    const t = Math.min(path.length - 1, Math.floor(i * lead));
    const target = path[t];

    vx += (target.x - px) * stiffness - vx * damping;
    vy += (target.y - py) * stiffness - vy * damping;
    px += vx;
    py += vy;

    out.push({ x: px, y: py });
    if (t >= path.length - 1 && Math.hypot(vx, vy) < 0.15) break;
  }
  return out;
}

// Resample to roughly even spacing and smooth, so the pen advances at a
// believable constant speed instead of jumping between sparse Hershey vertices.
function resample(stroke: Stroke, step: number): Stroke {
  const out: Stroke = [stroke[0]];
  let carry = 0;
  for (let i = 1; i < stroke.length; i++) {
    const a = stroke[i - 1];
    const b = stroke[i];
    const dx = b.x - a.x;
    const dy = b.y - a.y;
    const len = Math.hypot(dx, dy);
    if (len < 1e-6) continue;
    let t = step - carry;
    while (t <= len) {
      out.push({ x: a.x + (dx * t) / len, y: a.y + (dy * t) / len });
      t += step;
    }
    carry = (carry + len) % step;
  }
  const last = stroke[stroke.length - 1];
  out.push({ x: last.x, y: last.y });
  return out;
}

function smooth(stroke: Stroke, passes: number): Stroke {
  let s = stroke;
  for (let p = 0; p < passes; p++) {
    if (s.length < 3) break;
    const n: Stroke = [s[0]];
    for (let i = 1; i < s.length - 1; i++) {
      n.push({
        x: (s[i - 1].x + 2 * s[i].x + s[i + 1].x) / 4,
        y: (s[i - 1].y + 2 * s[i].y + s[i + 1].y) / 4,
      });
    }
    n.push(s[s.length - 1]);
    s = n;
  }
  return s;
}

// ----------------------------------------------------------- tldraw's ink

// Reproduced from tldraw/tldraw @ packages/tldraw/src/lib/shapes/draw/getPath.ts
// (`simulatePressureSettings`) and packages/tldraw/src/lib/primitives/utils.ts
// (`modulate`), so the stroke is shaped by tldraw's real draw-tool parameters
// rather than anything invented here. This is the freehand-pen branch: dash
// 'draw', isPen false, which is what you get drawing with a mouse or finger.
const easeOutSine = (t: number) => Math.sin((t * Math.PI) / 2);

function modulate(value: number, a: [number, number], b: [number, number], clamp: boolean) {
  const [low, high] = a;
  const [min, max] = b;
  const t = (value - low) / (high - low);
  let result = min + t * (max - min);
  if (clamp) {
    if (max < min) result = Math.max(Math.min(result, min), max);
    else result = Math.min(Math.max(result, min), max);
  }
  return result;
}

function tldrawDrawOptions(strokeWidth: number): StrokeOptions {
  return {
    size: strokeWidth,
    thinning: 0.5,
    streamline: modulate(strokeWidth, [9, 16], [0.64, 0.74], true),
    smoothing: 0.62,
    easing: easeOutSine,
    simulatePressure: true,
    last: true, // the stroke is finished, so tldraw applies its end cap
  };
}

type InkPt = { x: number; y: number; r: number };

// Run one centreline through tldraw's pipeline and read back the points it
// decided on, each with the radius its pressure model gave it.
function inkStroke(stroke: Stroke, strokeWidth: number): InkPt[] {
  const options = tldrawDrawOptions(strokeWidth);
  ingest(stroke, options);
  computeRadii(options);

  const out: InkPt[] = [];
  for (let i = 0; i < pointCount; i++) {
    out.push({ x: pointX[i], y: pointY[i], r: radii[i] });
  }
  return out;
}

// ------------------------------------------------------------------- PNG

function writePng(path: string, w: number, h: number, gray: Uint8Array) {
  const raw = Buffer.alloc((w + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w + 1)] = 0;
    gray.subarray(y * w, y * w + w).forEach((v, x) => {
      raw[y * (w + 1) + 1 + x] = v;
    });
  }
  const idat = deflateSync(raw, { level: 9 });

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
  ihdr[8] = 8;  // bit depth
  ihdr[9] = 0;  // greyscale
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(
    path,
    Buffer.concat([
      Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
      chunk("IHDR", ihdr),
      chunk("IDAT", idat),
      chunk("IEND", Buffer.alloc(0)),
    ]),
  );
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

// --------------------------------------------------------------- rasteriser

// Mirrors what the firmware does, so the preview is honest: the same discs at
// the same tldraw-derived radii, stamped in the same order.
function raster(strokes: InkPt[][]): Uint8Array {
  const img = new Uint8Array(PANEL_W * PANEL_H).fill(255);

  const dot = (cx: number, cy: number, r: number) => {
    const x0 = Math.max(0, Math.floor(cx - r));
    const x1 = Math.min(PANEL_W - 1, Math.ceil(cx + r));
    const y0 = Math.max(0, Math.floor(cy - r));
    const y1 = Math.min(PANEL_H - 1, Math.ceil(cy + r));
    for (let y = y0; y <= y1; y++) {
      for (let x = x0; x <= x1; x++) {
        const d = Math.hypot(x + 0.5 - cx, y + 0.5 - cy);
        if (d <= r) img[y * PANEL_W + x] = 0;
      }
    }
  };

  for (const s of strokes) for (const p of s) dot(p.x, p.y, Math.max(0.5, p.r));
  return img;
}

// -------------------------------------------------------------------- main

const args = process.argv.slice(2);
const argOf = (name: string, dflt: string) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] ? args[i + 1] : dflt;
};

const fontName = argOf("font", "scripts");
const size = parseFloat(argOf("size", "40"));
const lineGap = parseFloat(argOf("gap", "104"));
const seed = parseInt(argOf("seed", "7"), 10);
const jitter = parseFloat(argOf("jitter", "1.8"));
const slant = parseFloat(argOf("slant", "0.20"));
const sizeVary = parseFloat(argOf("vary", "0.22"));
const drift = parseFloat(argOf("drift", "5.0"));
// tldraw's own stroke width for a draw shape is theme.strokeWidth *
// STROKE_SIZES[size], +1, at canvas scale. Our glyphs are ~40px tall against
// tldraw's ~4.5px default, so this is that number scaled to this panel.
const strokeWidth = parseFloat(argOf("stroke", "7"));
const step = parseFloat(argOf("step", "1.2"));
const tracking = parseFloat(argOf("tracking", "-3.5"));
const rough = parseFloat(argOf("rough", "0.35"));
const spaceScale = parseFloat(argOf("space", "0.62"));

const MESSAGE = ["Thanks", "for the", "meetup", "@tldraw!"];

// Centre the block vertically: baselines sit at top + n*gap, and the glyph box
// hangs above the baseline, so the block spans roughly [top - size, top + 2*gap].
const blockHeight = (MESSAGE.length - 1) * lineGap + size;
const top = parseFloat(argOf("top", String(Math.round((PANEL_H - blockHeight) / 2 + size))));

const jhf = readFileSync(join(ROOT, "tools", "fonts", `${fontName}.jhf`), "latin1");
const glyphs = parseJhf(jhf);

const centrelines = layout(MESSAGE, glyphs, { size, top, lineGap, seed, jitter, slant, tracking, rough, spaceScale, sizeVary, drift })
  // Densify before handing over: tldraw expects the stream of closely spaced
  // samples a real pointer produces, not sparse font vertices. Smoothing is
  // left to tldraw's own streamline pass rather than done here.
  .map((s) => resample(s, step));

const strokes = centrelines.map((s) => inkStroke(s, strokeWidth));

const totalPoints = strokes.reduce((n, s) => n + s.length, 0);
const maxRadius = strokes.reduce((m, s) => s.reduce((k, p) => Math.max(k, p.r), m), 0);
console.log(
  `font=${fontName} glyphs=${glyphs.size} strokes=${strokes.length} points=${totalPoints} ` +
  `strokeWidth=${strokeWidth} maxRadius=${maxRadius.toFixed(2)}`,
);

writePng(join(ROOT, "preview", `preview-${fontName}.png`), PANEL_W, PANEL_H, raster(strokes));
console.log(`wrote preview/preview-${fontName}.png`);

// C header ------------------------------------------------------------------

const header: string[] = [];
header.push("// Generated by tools/gen-strokes.ts. Do not edit by hand.");
header.push(`// font=${fontName} size=${size} seed=${seed} strokeWidth=${strokeWidth}`);
header.push("// Radii come from tldraw's own freehand pipeline (ingest + computeRadii),");
header.push("// vendored under tools/tldraw-freehand/ from tldraw/tldraw.");
header.push("#ifndef STROKES_H");
header.push("#define STROKES_H");
header.push("");
header.push("#include <stdint.h>");
header.push("");
header.push(`#define STROKE_COUNT ${strokes.length}`);
header.push(`#define POINT_COUNT ${totalPoints}`);
header.push("");
header.push("// Positions are Q4 fixed point (1/16 px); r is the tldraw radius, also Q4,");
header.push("// which caps well under 16 px so a byte is plenty.");
header.push("typedef struct { int16_t x, y; uint8_t r; } stroke_pt_t;");
header.push("");
header.push("static const stroke_pt_t stroke_points[POINT_COUNT] = {");
for (const s of strokes) {
  const body = s
    .map((p) => {
      const r = Math.min(255, Math.max(8, Math.round(p.r * 16)));
      return `{${Math.round(p.x * 16)},${Math.round(p.y * 16)},${r}}`;
    })
    .join(",");
  header.push(`    ${body},`);
}
header.push("};");
header.push("");
header.push("// Index of the first point of each stroke, plus a terminating total.");
header.push(`static const uint16_t stroke_offsets[STROKE_COUNT + 1] = {`);
{
  const offs: number[] = [];
  let acc = 0;
  for (const s of strokes) { offs.push(acc); acc += s.length; }
  offs.push(acc);
  for (let i = 0; i < offs.length; i += 16) {
    header.push("    " + offs.slice(i, i + 16).join(",") + ",");
  }
}
header.push("};");
header.push("");
header.push("#endif // STROKES_H");

mkdirSync(join(ROOT, "firmware"), { recursive: true });
writeFileSync(join(ROOT, "firmware", "strokes.h"), header.join("\n") + "\n");
console.log(`wrote firmware/strokes.h`);
