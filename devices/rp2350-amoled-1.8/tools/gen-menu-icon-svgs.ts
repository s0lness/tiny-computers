/**
 * Emits one SVG per menu icon, sized for a real pen plotting on paper (the
 * reMarkable, via ~/projects/volante's `bun tools/volante.ts svg <file>`),
 * not for a rasteriser. This is a companion to tools/preview-menu-icons.ts
 * (the pixel-accurate PNG capture from the real firmware) - that one answers
 * "what does the panel show", this one answers "what does the object look
 * like drawn at a size a hand would actually draw it".
 *
 * CENTRELINES, not filled outlines - chosen deliberately. A real pen tracing
 * a FILLED shape's boundary draws the boundary and leaves the middle empty:
 * on paper that reads as a hollow shell, not as thick ink. The firmware's
 * own ink (shapes.c) is a filled/stroked region because it is choosing
 * pixel coverage; a physical pen has its own fixed nib width, so the right
 * analogue is the SAME thing draw_icon_sketch's own centreline already is -
 * a path down the middle of the stroke - and let the pen's own line weight
 * carry the "thick ink" the brief asks for, rather than asking the SVG to
 * simulate a thick stroke that the plotter would then hollow out. Every
 * path below is therefore either an open centreline (chrono's crown, the
 * hourglass's sand line, the coil, the pencil squiggle) or a closed
 * silhouette OUTLINE meant to be read as a single traced boundary (the
 * chrono ring, the hourglass glass) - never a shape whose INTERIOR carries
 * meaning that tracing its edge would lose. stroke-width below is a small,
 * fixed reference weight for viewing the SVG on screen, not a claim about
 * final ink weight on paper.
 *
 * Geometry mirrors firmware/apps/menu.c's constants by hand (this is a
 * comparison tool, not a build dependency the firmware includes), noted at
 * each block so a future edit to menu.c's numbers can be kept in sync.
 *
 *   bun tools/gen-menu-icon-svgs.ts
 */
import { writeFileSync, mkdirSync } from "node:fs";
import { join } from "node:path";

const ROOT = join(import.meta.dir, "..");
const OUT_DIR = join(ROOT, "preview", "svg");

// Icon design space: 0..96, matching ICON_W/ICON_H in menu.c. Physical size
// on paper: SIZE_MM square, comfortably inside an A5-ish sheet (the
// reMarkable 2's own panel is roughly 157x210mm) with margin to spare.
const UNIT = 96;
const SIZE_MM = 130;
const PAD = 5; // viewBox padding, in design units, so nothing touches the edge

type Pt = { x: number; y: number };

function svgHeader(title: string): string {
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="${-PAD} ${-PAD} ${UNIT + 2 * PAD} ${UNIT + 2 * PAD}" width="${SIZE_MM}mm" height="${SIZE_MM}mm">
<title>${title}</title>
<style>path,circle{fill:none;stroke:#000;stroke-width:1.2;stroke-linecap:round;stroke-linejoin:round;}</style>
`;
}
const SVG_FOOTER = `</svg>\n`;

function pathFromPoints(pts: Pt[], close: boolean): string {
  const d = pts.map((p, i) => `${i === 0 ? "M" : "L"}${p.x.toFixed(2)},${p.y.toFixed(2)}`).join(" ");
  return `<path d="${d}${close ? " Z" : ""}"/>\n`;
}

function write(name: string, body: string, title: string) {
  mkdirSync(OUT_DIR, { recursive: true });
  const svg = svgHeader(title) + body + SVG_FOOTER;
  const path = join(OUT_DIR, `${name}.svg`);
  writeFileSync(path, svg);
  console.log(`wrote preview/svg/${name}.svg`);
}

// ============================================================== sketch ===
// Mirrors draw_icon_sketch() exactly: 5 waypoints, straight lead-in/out,
// three quadratic Beziers through the midpoints. The model icon - included
// so the other three can be compared against it stroke-for-stroke on the
// same sheet, not just on screen.
{
  const fx = [8, 90, 8, 92, 84];
  const fy = [8, 20, 48, 72, 94];
  const P = fx.map((x, i) => ({ x: (x * UNIT) / 100, y: (fy[i] * UNIT) / 100 }));
  const mid = (a: Pt, b: Pt) => ({ x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 });
  const M01 = mid(P[0], P[1]), M12 = mid(P[1], P[2]), M23 = mid(P[2], P[3]), M34 = mid(P[3], P[4]);

  const d =
    `M${P[0].x.toFixed(2)},${P[0].y.toFixed(2)} ` +
    `L${M01.x.toFixed(2)},${M01.y.toFixed(2)} ` +
    `Q${P[1].x.toFixed(2)},${P[1].y.toFixed(2)} ${M12.x.toFixed(2)},${M12.y.toFixed(2)} ` +
    `Q${P[2].x.toFixed(2)},${P[2].y.toFixed(2)} ${M23.x.toFixed(2)},${M23.y.toFixed(2)} ` +
    `Q${P[3].x.toFixed(2)},${P[3].y.toFixed(2)} ${M34.x.toFixed(2)},${M34.y.toFixed(2)} ` +
    `L${P[4].x.toFixed(2)},${P[4].y.toFixed(2)}`;
  write("sketch", `<path d="${d}"/>\n`, "sketch (the model - unchanged)");
}

// ================================================================ chrono ==
// Mirrors draw_icon_chrono(): CHRONO_R_OUT=34, CHRONO_STROKE=10,
// CHRONO_R_IN=24, CHRONO_R_MID=29, CHRONO_CROWN_LEN=16, CHRONO_COMP_H=79.
// The dial's centreline is the ring's own MID radius, not its outer edge -
// stroking that circle at the firmware's own band width reproduces the
// annulus; here it is left as a plain circle outline for the pen. The
// crown is a single straight centreline, base to tip, matching the one
// shapes_fill_capsule_aa_land call in the firmware.
{
  const R_OUT = 34, STROKE = 10, R_IN = R_OUT - STROKE, R_MID = (R_OUT + R_IN) / 2;
  const CROWN_LEN = 16;
  const COMP_H = R_MID + CROWN_LEN + R_OUT;
  const top = (UNIT - COMP_H) / 2;
  const cx = UNIT / 2;
  const cy = top + R_MID + CROWN_LEN;

  const crownBaseY = cy - R_MID;
  const crownTipY = crownBaseY - CROWN_LEN;

  const circle =
    `<path d="M${(cx + R_MID).toFixed(2)},${cy.toFixed(2)} ` +
    `A${R_MID},${R_MID} 0 1,1 ${(cx - R_MID).toFixed(2)},${cy.toFixed(2)} ` +
    `A${R_MID},${R_MID} 0 1,1 ${(cx + R_MID).toFixed(2)},${cy.toFixed(2)} Z"/>\n`;
  const crown = pathFromPoints([{ x: cx, y: crownBaseY }, { x: cx, y: crownTipY }], false);
  write("chrono", circle + crown, "chrono (redrawn: ring + one crown flick)");
}

// ========================================================= timer, glass ===
// Mirrors ensure_timer_bulb_table() and draw_icon_timer(): the same ellipse
// hw(row) = A*sqrt(1-((row-P)/B)^2) formula, TIMER_HALF_OUT=28,
// TIMER_HALF_NECK=5, TIMER_CHAMBER_H=44, TIMER_BULB_PEAK_ROW=13,
// TIMER_MARGIN=2, TIMER_NECK_H=4. Emits ONE closed outline (the glass,
// traced continuously: top cap, down the left wall, across the bottom
// cap, up the right wall, back across the top cap - the same "one
// continuous piece of ink" the firmware's own capsule march plus flat
// caps already forms, see draw_icon_timer's header comment) and two small
// open marks for the sand: a line at the sand's own top (not the dip
// crater detail the firmware renders - too fine to survive at this
// comparison's resolution either, per the brief's own "detail that
// survives is the only detail worth drawing") and a small heap triangle
// resting on the bottom cap.
function hourglassBulbTable(): number[] {
  const OUT = 28, NECK = 5, CHAMBER_H = 44, PEAK = 13;
  const p = PEAK, m = CHAMBER_H - 1;
  const v = (OUT * OUT - NECK * NECK) / ((m - p) * (m - p) - p * p);
  const aSq = OUT * OUT + v * p * p;
  const a = Math.sqrt(aSq);
  const b = Math.sqrt(aSq / v);
  const hw: number[] = [];
  for (let row = 0; row < CHAMBER_H; row++) {
    const t = (row - p) / b;
    const under = 1 - t * t;
    let h = under > 0 ? a * Math.sqrt(under) : 0;
    if (h < NECK) h = NECK;
    hw.push(h);
  }
  return hw;
}
{
  const MARGIN = 2, HALF_OUT = 28, HALF_NECK = 5, NECK_H = 4, CHAMBER_H = 44;
  const hw = hourglassBulbTable();
  const top = MARGIN;
  const cx = UNIT / 2;
  const neckY = top + CHAMBER_H;
  const bottomTop = neckY + NECK_H;

  const leftDown: Pt[] = hw.map((h, row) => ({ x: cx - h, y: top + row }));
  const leftNeck: Pt[] = [{ x: cx - HALF_NECK, y: neckY }, { x: cx - HALF_NECK, y: bottomTop }];
  const leftBottomDown: Pt[] = hw.map((h, row) => ({ x: cx - hw[CHAMBER_H - 1 - row], y: bottomTop + row }));
  const bottomCap: Pt[] = [{ x: cx - HALF_OUT, y: bottomTop + CHAMBER_H - 1 }, { x: cx + HALF_OUT, y: bottomTop + CHAMBER_H - 1 }];
  const rightBottomUp: Pt[] = [...leftBottomDown].reverse().map((p) => ({ x: 2 * cx - p.x, y: p.y }));
  const rightNeck: Pt[] = [{ x: cx + HALF_NECK, y: bottomTop }, { x: cx + HALF_NECK, y: neckY }];
  const rightUp: Pt[] = [...leftDown].reverse().map((p) => ({ x: 2 * cx - p.x, y: p.y }));
  const topCap: Pt[] = [{ x: cx + HALF_OUT, y: top }, { x: cx - HALF_OUT, y: top }];

  const outline: Pt[] = [...leftDown, ...leftNeck, ...leftBottomDown, ...bottomCap, ...rightBottomUp, ...rightNeck, ...rightUp, ...topCap];

  // Sand line: same rowSandTop the firmware computes (CHAMBER_H - 60%),
  // a plain straight line rather than the firmware's own dip crater -
  // that curvature is a handful of pixels on the real panel and would not
  // survive being drawn at hand scale either.
  const sandDepthRows = Math.floor((CHAMBER_H * 3) / 5);
  const rowSandTop = CHAMBER_H - sandDepthRows;
  const sandInset = 3; // TIMER_OUTLINE/2
  const sandY = top + rowSandTop;
  const sandHw = hw[rowSandTop] - sandInset;
  const sandLine: Pt[] = [{ x: cx - sandHw, y: sandY }, { x: cx + sandHw, y: sandY }];

  // Heap: small triangle resting on the bottom cap, same footprint as the
  // firmware's own heapHw={1,2,3,4}.
  const heapRows = 4, heapBase = 4;
  const heapTop = bottomTop + CHAMBER_H - 1 - heapRows;
  const heap: Pt[] = [
    { x: cx, y: heapTop },
    { x: cx + heapBase, y: heapTop + heapRows },
    { x: cx - heapBase, y: heapTop + heapRows },
  ];

  const body = pathFromPoints(outline, true) + pathFromPoints(sandLine, false) + pathFromPoints(heap, true);
  write("timer-hourglass", body, "timer, option A: hourglass (cleaned up)");
}

// ========================================================== timer, coil ==
// Mirrors draw_icon_timer_coil() exactly: COIL_TURNS=2.25, COIL_R_OUTER=40,
// COIL_R_INNER=6, COIL_POINTS=72, spiralling inward from 12 o'clock. One
// continuous centreline - the firmware's own capsule chain, minus the
// width, since a real pen supplies its own.
{
  const TURNS = 2.25, R_OUTER = 40, R_INNER = 6, POINTS = 72;
  const cx = UNIT / 2, cy = UNIT / 2;
  const pts: Pt[] = [];
  for (let i = 0; i < POINTS; i++) {
    const t = i / (POINTS - 1);
    const theta = t * TURNS * 2 * Math.PI;
    const r = R_OUTER + (R_INNER - R_OUTER) * t;
    pts.push({ x: cx + r * Math.sin(theta), y: cy - r * Math.cos(theta) });
  }
  write("timer-coil", pathFromPoints(pts, false), "timer, option B: coil (proposed alternative)");
}

console.log(`\nAll four written to preview/svg/. Centrelines, not filled outlines - see this file's header comment for why.`);
