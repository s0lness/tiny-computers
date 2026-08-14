/**
 * Design-time generator for the chrono icon's THIRD pass, 2026-08-14.
 *
 * Round 1 (commit before e8e1548) was the ring/wedge/crown-rect/neck-rect/
 * tab-diamond silhouette, all exact geometric primitives. Round 2 (e8e1548)
 * replaced it with a plain annulus and one crown flick - simpler, provably
 * connected, but the owner's verdict was "j'aimais bien celui qu'on avait
 * avant" (he liked the old one) and the coordinator's read was specific:
 * the filled wedge is what said "a dial with something on it", and the
 * annulus-plus-nub read as "a ring with a nub" instead.
 *
 * So round 3 brings the OLD SILHOUETTE back (ring, wedge, crown+neck,
 * tab), but redraws its three straight-edged parts by hand rather than by
 * compass and ruler ("des traits moins droits"), and fixes the one part
 * that was floating (the tab, deliberately held "clear of the ring" -
 * "les pixels qui sortent").
 *
 * This script computes the HAND WOBBLE for those straight edges and bakes
 * the result into small constant arrays for firmware/apps/menu.c to use
 * directly - the same shape of decision draw_icon_sketch already made for
 * its own fx/fy/fr waypoints (fixed constants, not a runtime formula), for
 * the same reason: the wobble is a design choice made once, not something
 * that needs recomputing on device.
 *
 * TWO stages, each a real, separate use of the tools already in this repo:
 *
 *   1. HUMANISING - gen-strokes.ts's own technique (a seeded RNG, small
 *      per-point jitter, kept deterministic so a rebuild reproduces the
 *      exact same wobble), applied to a handful of control points along
 *      each straight edge instead of to letterforms.
 *   2. SMOOTHING - fed through tldraw's own ingest() (streamline pass,
 *      tools/tldraw-freehand/core.ts), which is the vendored pipeline the
 *      brief points at: raw jittered control points read as noise, not as
 *      a hand's wobble, until streamlined the way a real pointer capture
 *      would be. This is the same pipeline gen-strokes.ts and
 *      gen-from-capture.ts both run every stroke through.
 *
 * (Width/taper is NOT read back from tldraw here - see
 * tools/gen-menu-icon-tldraw-check.ts's own finding: computeRadii does not
 * taper a procedural, evenly-spaced path without explicit start/end.taper.
 * The crown and tab strokes below specify their own radii by hand, same as
 * the coil.)
 *
 *   bun tools/gen-chrono-icon.ts
 */
import { ingest, pointX, pointY, pointCount } from "./tldraw-freehand/core";

function makeRng(seed: number) {
  let s = seed >>> 0;
  return () => {
    s ^= s << 13; s >>>= 0;
    s ^= s >> 17;
    s ^= s << 5; s >>>= 0;
    return s / 0x100000000;
  };
}

type Pt = { x: number; y: number };

// Smooths a short list of raw, jittered control points into a hand-wobble
// curve via tldraw's own streamline pass, then resamples it to `outN`
// evenly-spaced points by walking the smoothed polyline's own arc length -
// the smoothed path is what actually gets used, the raw points are only
// the noise source.
function humanise(raw: Pt[], outN: number, streamline = 0.35): Pt[] {
  ingest(raw, { streamline, size: 8, simulatePressure: false });
  const smoothed: Pt[] = [];
  for (let i = 0; i < pointCount; i++) smoothed.push({ x: pointX[i], y: pointY[i] });

  const lens = [0];
  for (let i = 1; i < smoothed.length; i++) {
    lens.push(lens[i - 1] + Math.hypot(smoothed[i].x - smoothed[i - 1].x, smoothed[i].y - smoothed[i - 1].y));
  }
  const total = lens[lens.length - 1];
  const out: Pt[] = [];
  for (let k = 0; k < outN; k++) {
    const target = (k / (outN - 1)) * total;
    let i = 1;
    while (i < lens.length - 1 && lens[i] < target) i++;
    const t = (target - lens[i - 1]) / Math.max(1e-6, lens[i] - lens[i - 1]);
    const a = smoothed[i - 1], b = smoothed[i];
    out.push({ x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t });
  }
  return out;
}

const rng = makeRng(20260814);
const jit = (amp: number) => (rng() - 0.5) * 2 * amp;

// ---- 1. WEDGE's straight edge: was `leftX[i] = ccx` for every row, dead
// vertical, 24 rows tall (rowStart..CHRONO_R_OUT-1 at CHRONO_R_OUT=34,
// CHRONO_R_IN=24 - see menu.c). 9 raw control points (more points than a
// single gentle bend needs, so the smoothed result still changes direction
// more than once - a lone bend reads as "tilted", not "hand-drawn"),
// +-3.5px, humanised at a LOW streamline (more of the raw wobble survives -
// tldraw's own streamline parameter is inverted from its name: higher
// values smooth MORE, see this file's humanise() for the derivation),
// resampled to the full 24-row table.
{
  const ROWS = 24;
  const raw: Pt[] = [];
  for (let i = 0; i < 9; i++) raw.push({ x: jit(3.5), y: (i / 8) * (ROWS - 1) });
  const wobbled = humanise(raw, ROWS, 0.2);
  const table = wobbled.map((p) => Math.round(p.x));
  console.log(`WEDGE edge offsets (${ROWS} rows, add to ccx):`);
  console.log(`  {${table.join(", ")}}`);
}

// ---- 2. CROWN+NECK: was two exact rects (12x6 neck, 24x12 crown), stacked
// dead straight, centred on ccx. Raw control points from a base INSIDE the
// ring's own ink (guaranteeing the connection the same way the round-2
// crown flick did) up to the crown's own tip, jittered, humanised at low
// streamline for the same reason as the wedge above.
//
// SHORTENED AND CALMED after the first render: at CHRONO_CROWN_LEN=20 with
// +-3px jitter and a taper down to a 2px point, this and the tab below
// rendered as two thin spikes off the top of the ring - "horns", not "a
// crown and a tab" (checked against the actual rendered PNG, not by
// eye on these numbers alone). The fix pulls two levers at once: shorter
// reach (14px, not 20) and gentler jitter (+-1.8px, not +-3), so the wobble
// reads as a hand's small imprecision rather than a zigzag; the OTHER
// lever - not tapering all the way to a point - lives in menu.c itself
// (the tip radius draw_icon_chrono() interpolates to), not here.
{
  const raw: Pt[] = [
    { x: 0, y: 0 },
    { x: jit(1.8), y: 3 },
    { x: jit(1.8), y: 7 },
    { x: jit(1.5), y: 10 },
    { x: jit(1.2), y: 14 },
  ];
  const wobbled = humanise(raw, 6, 0.3);
  console.log(`\nCROWN path (6 points, base at ring centreline, +y = up toward tip):`);
  for (const p of wobbled) console.log(`  {${p.x.toFixed(1)}, ${(-p.y).toFixed(1)}},`);
}

// ---- 3. TAB: was a diamond centred CHRONO_TAB_DIST=46 from the ring's
// centre, held CHRONO_TAB_GAP=4 clear of the ring - the floating mark.
// Rebuilt as a short connected stroke: base on the ring's own centreline
// (same trick as the crown), tip out near the old tab's own position.
// Raw control points along that radius, jittered off the straight line.
// Same shortening/calming as the crown above, same reason.
{
  const raw: Pt[] = [
    { x: 0, y: 0 },
    { x: jit(1.5), y: 3 },
    { x: jit(1.8), y: 6 },
    { x: jit(1.2), y: 9 },
  ];
  const wobbled = humanise(raw, 4, 0.3);
  console.log(`\nTAB path (4 points, local frame: +x along the radius outward, +y perpendicular):`);
  for (const p of wobbled) console.log(`  {${p.x.toFixed(1)}, ${p.y.toFixed(1)}},`);
}
