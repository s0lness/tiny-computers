/**
 * Design-time conversion from a vendored Lucide icon (third_party/lucide/)
 * to the C constant tables firmware/apps/menu.c's Lucide-derived icon
 * functions use, plus a fast approximate PNG preview so the shape can be
 * judged before spending a wasm rebuild cycle on it (the AUTHORITATIVE
 * preview is still tools/preview-menu-icons.ts, which reads the real
 * compiled firmware - this one is for quick iteration only, same relation
 * gen-strokes.ts's own PNG has to the real device).
 *
 * Prints C arrays to stdout (hand-ported into menu.c, same discipline
 * every other baked-constant icon in that file already uses - see
 * draw_icon_sketch's fx/fy/fr, or the chrono icon's hand-wobble tables)
 * rather than writing a generated header menu.c includes: this runs once
 * per candidate, by a person choosing a candidate, not on every build.
 *
 *   bun tools/gen-lucide-menu-icons.ts <icon-name> [--stroke N]
 *   bun tools/gen-lucide-menu-icons.ts timer
 *   bun tools/gen-lucide-menu-icons.ts hourglass --stroke 2.5
 */
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { deflateSync } from "node:zlib";
import { join, dirname } from "node:path";
import { flattenLucideIcon, type Pt } from "./lucide-convert";

const ROOT = join(import.meta.dir, "..");
const ICON_BOX = 96; // ICON_W/ICON_H in menu.c
const SCALE = ICON_BOX / 24; // Lucide's own viewBox is always 24x24 - 4x

const args = process.argv.slice(2);
const name = args[0];
if (!name) {
  console.error("usage: bun tools/gen-lucide-menu-icons.ts <icon-name> [--stroke N]");
  process.exit(1);
}
const strokeArgIdx = args.indexOf("--stroke");
// Lucide's own stroke-width is 2 (in its 24-unit space); the faithful
// conversion is therefore 2*SCALE=8px at this device's 96px icon box,
// close to the chrono ring's old 10px band - see this repo's own report
// on whether that reads as strong enough at 96px viewed by a child.
const lucideStrokeWidth = strokeArgIdx >= 0 ? parseFloat(args[strokeArgIdx + 1]) : 2;
const strokeHalfPx = (lucideStrokeWidth * SCALE) / 2;

const svg = readFileSync(join(ROOT, "third_party", "lucide", "icons", `${name}.svg`), "utf8");
const flat = flattenLucideIcon(svg);

const toIcon = (p: Pt) => ({ x: p.x * SCALE, y: p.y * SCALE });

console.log(`// ${name}.svg, Lucide (third_party/lucide/), stroke ${lucideStrokeWidth} -> ${strokeHalfPx.toFixed(1)}px radius at ${ICON_BOX}px`);
console.log(`// circles=${flat.circles.length} subpaths=${flat.subpaths.length}`);

for (const c of flat.circles) {
  const cx = c.cx * SCALE, cy = c.cy * SCALE, r = c.r * SCALE;
  console.log(`// circle: cx=${cx.toFixed(2)} cy=${cy.toFixed(2)} r=${r.toFixed(2)} (annulus outer=${(r + strokeHalfPx).toFixed(2)} inner=${(r - strokeHalfPx).toFixed(2)})`);
}

flat.subpaths.forEach((sp, i) => {
  const pts = sp.map(toIcon);
  const xs = pts.map((p) => p.x.toFixed(2)).join("f, ") + "f";
  const ys = pts.map((p) => p.y.toFixed(2)).join("f, ") + "f";
  console.log(`static const float s_xxxSub${i}X[${pts.length}] = { ${xs} };`);
  console.log(`static const float s_xxxSub${i}Y[${pts.length}] = { ${ys} };`);
});

// -------------------------------------------------------------- preview --
function raster(flat: { circles: { cx: number; cy: number; r: number }[]; subpaths: Pt[][] }, strokeHalf: number): Uint8Array {
  const img = new Uint8Array(ICON_BOX * ICON_BOX).fill(255);
  const dot = (cx: number, cy: number, r: number) => {
    const x0 = Math.max(0, Math.floor(cx - r)), x1 = Math.min(ICON_BOX - 1, Math.ceil(cx + r));
    const y0 = Math.max(0, Math.floor(cy - r)), y1 = Math.min(ICON_BOX - 1, Math.ceil(cy + r));
    for (let y = y0; y <= y1; y++)
      for (let x = x0; x <= x1; x++)
        if (Math.hypot(x + 0.5 - cx, y + 0.5 - cy) <= r) img[y * ICON_BOX + x] = 0;
  };
  const segDot = (x0: number, y0: number, x1: number, y1: number, r: number) => {
    const steps = Math.max(1, Math.ceil(Math.hypot(x1 - x0, y1 - y0) / (r * 0.5)));
    for (let i = 0; i <= steps; i++) {
      const t = i / steps;
      dot(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, r);
    }
  };
  for (const c of flat.circles) {
    const cx = c.cx * SCALE, cy = c.cy * SCALE, r = c.r * SCALE;
    const n = 96;
    for (let i = 0; i < n; i++) {
      const a0 = (i / n) * Math.PI * 2, a1 = ((i + 1) / n) * Math.PI * 2;
      segDot(cx + r * Math.cos(a0), cy + r * Math.sin(a0), cx + r * Math.cos(a1), cy + r * Math.sin(a1), strokeHalf);
    }
  }
  for (const sp of flat.subpaths) {
    const pts = sp.map(toIcon);
    for (let i = 1; i < pts.length; i++) segDot(pts[i - 1].x, pts[i - 1].y, pts[i].x, pts[i].y, strokeHalf);
  }
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

const img = raster(flat, strokeHalfPx);
const path = join(ROOT, "preview", `lucide-design-${name}.png`);
writePng(path, ICON_BOX, ICON_BOX, img);
console.log(`\n// wrote preview/lucide-design-${name}.png (approximate - see this file's header comment)`);
