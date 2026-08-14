/**
 * Flattens a vendored Lucide icon (third_party/lucide/icons/*.svg) into the
 * vocabulary our firmware primitives already speak: circles (for
 * shapes_fill_annulus_aa_land) and polylines of points (for
 * shapes_stroke_polyline_aa_land, a chain of round-capped capsule
 * segments). Lucide's own stroke vocabulary - <line>, <circle>, and <path>
 * built from M/L/H/V/A/Z - is deliberately narrow (round caps and joins,
 * light constant weight, corners rounded rather than beveled), which is
 * why a geometric flattener is enough here: nothing needs curve-fitting or
 * simplification, only sampling the arcs finely enough that a 16-segment
 * approximation is indistinguishable from the true ellipse at icon scale.
 *
 * No cubic/quadratic Bezier support (C/S/Q/T): none of the five vendored
 * candidates use them (checked by inspecting each file - Lucide favours
 * arcs for rounded corners, not Beziers), so it was not worth the extra
 * code for paths this generator does not need to read. If a future
 * candidate needs it, this is the file to extend, not to route around.
 *
 * Coordinates stay in Lucide's own 24x24 viewBox; the caller decides how to
 * scale and place them (see tools/gen-lucide-menu-icons.ts).
 */

export type Pt = { x: number; y: number };
export type Circle = { cx: number; cy: number; r: number };
export type FlattenedIcon = { circles: Circle[]; subpaths: Pt[][] };

const ARC_STEPS = 16; // fine enough that a corner-rounding radius (often just 1-2 units
                       // on a 24-unit grid) looks like a true curve, not a facet

function angleBetween(ux: number, uy: number, vx: number, vy: number): number {
  const dot = ux * vx + uy * vy;
  const len = Math.hypot(ux, uy) * Math.hypot(vx, vy);
  let ang = Math.acos(Math.max(-1, Math.min(1, dot / len)));
  if (ux * vy - uy * vx < 0) ang = -ang;
  return ang;
}

/**
 * Flattens one SVG elliptical arc (endpoint parameterisation, per the SVG
 * 1.1 spec appendix F.6) into `ARC_STEPS` points, EXCLUDING the start point
 * (the caller already has it as the path's current point) and INCLUDING
 * the end point. x-axis-rotation is not supported - none of the vendored
 * icons use it (every `a`/`A` command here has rotation 0), so the
 * rotation matrices the full spec needs collapse to the identity and are
 * left out rather than carried as dead code.
 */
function flattenArc(x0: number, y0: number, rx: number, ry: number, largeArc: boolean, sweep: boolean, x1: number, y1: number): Pt[] {
  if (rx === 0 || ry === 0 || (x0 === x1 && y0 === y1)) return [{ x: x1, y: y1 }];
  rx = Math.abs(rx);
  ry = Math.abs(ry);

  const x1p = (x0 - x1) / 2;
  const y1p = (y0 - y1) / 2;
  let lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
  if (lambda > 1) {
    const s = Math.sqrt(lambda);
    rx *= s;
    ry *= s;
  }

  const sign = largeArc !== sweep ? 1 : -1;
  const num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
  const den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
  const co = sign * Math.sqrt(Math.max(0, num / den));
  const cxp = co * ((rx * y1p) / ry);
  const cyp = co * ((-ry * x1p) / rx);
  const cx = (x0 + x1) / 2 + cxp;
  const cy = (y0 + y1) / 2 + cyp;

  const theta1 = angleBetween(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
  let dtheta = angleBetween((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
  if (!sweep && dtheta > 0) dtheta -= 2 * Math.PI;
  if (sweep && dtheta < 0) dtheta += 2 * Math.PI;

  const out: Pt[] = [];
  for (let i = 1; i <= ARC_STEPS; i++) {
    const t = theta1 + (dtheta * i) / ARC_STEPS;
    out.push({ x: cx + rx * Math.cos(t), y: cy + ry * Math.sin(t) });
  }
  return out;
}

// Tokenises a path's numeric argument list. Lucide's own minifier omits
// separators wherever a sign or a decimal point already disambiguates two
// numbers ("0-.586-1.414", "0 0 0.623.622"), so this cannot split on
// whitespace/commas alone - it has to actually recognise number boundaries.
function tokeniseNumbers(s: string): number[] {
  const m = s.match(/-?\d*\.?\d+(?:[eE][-+]?\d+)?/g);
  return m ? m.map(Number) : [];
}

/**
 * Flattens one <path d="..."> into one or more subpaths (a new one starts
 * at each M/m). Supports M/m, L/l, H/h, V/v, A/a, Z/z - see this file's
 * header comment for why that is the whole set the vendored icons need.
 */
export function flattenPathD(d: string): Pt[][] {
  const cmdRe = /([MLHVAZmlhvaz])([^MLHVAZmlhvaz]*)/g;
  const subpaths: Pt[][] = [];
  let current: Pt[] = [];
  let cx = 0, cy = 0;       // current point
  let startX = 0, startY = 0; // subpath start, for Z

  let match: RegExpExecArray | null;
  while ((match = cmdRe.exec(d))) {
    const cmd = match[1];
    const nums = tokeniseNumbers(match[2]);
    const rel = cmd === cmd.toLowerCase();
    const upper = cmd.toUpperCase();

    const argCount = upper === "M" || upper === "L" ? 2 : upper === "H" || upper === "V" ? 1 : upper === "A" ? 7 : 0;

    if (upper === "Z") {
      if (current.length) current.push({ x: startX, y: startY });
      cx = startX;
      cy = startY;
      continue;
    }

    if (argCount === 0) continue;

    for (let i = 0; i + argCount <= nums.length; i += argCount) {
      // The first coordinate pair after M/m is a moveto; every subsequent
      // pair in the SAME command run is an implicit lineto (SVG spec) -
      // `isFirst` tracks that for M only.
      const isFirstOfRun = i === 0;

      if (upper === "M") {
        const x = rel ? cx + nums[i] : nums[i];
        const y = rel ? cy + nums[i + 1] : nums[i + 1];
        if (isFirstOfRun) {
          if (current.length) subpaths.push(current);
          current = [{ x, y }];
          startX = x;
          startY = y;
        } else {
          current.push({ x, y }); // implicit lineto
        }
        cx = x;
        cy = y;
      } else if (upper === "L") {
        const x = rel ? cx + nums[i] : nums[i];
        const y = rel ? cy + nums[i + 1] : nums[i + 1];
        current.push({ x, y });
        cx = x;
        cy = y;
      } else if (upper === "H") {
        const x = rel ? cx + nums[i] : nums[i];
        current.push({ x, y: cy });
        cx = x;
      } else if (upper === "V") {
        const y = rel ? cy + nums[i] : nums[i];
        current.push({ x: cx, y });
        cy = y;
      } else if (upper === "A") {
        const rx = nums[i], ry = nums[i + 1];
        // nums[i+2] is x-axis-rotation, unused - see flattenArc's own comment.
        const largeArc = nums[i + 3] !== 0;
        const sweep = nums[i + 4] !== 0;
        const x = rel ? cx + nums[i + 5] : nums[i + 5];
        const y = rel ? cy + nums[i + 6] : nums[i + 6];
        const pts = flattenArc(cx, cy, rx, ry, largeArc, sweep, x, y);
        current.push(...pts);
        cx = x;
        cy = y;
      }
    }
  }
  if (current.length) subpaths.push(current);
  return subpaths;
}

/** Parses the handful of SVG elements Lucide's own icons are built from. */
export function flattenLucideIcon(svg: string): FlattenedIcon {
  const circles: Circle[] = [];
  const subpaths: Pt[][] = [];

  for (const m of svg.matchAll(/<circle\b([^>]*)\/?>/g)) {
    const attrs = m[1];
    const num = (name: string) => {
      const am = attrs.match(new RegExp(`${name}="(-?[\\d.]+)"`));
      return am ? parseFloat(am[1]) : 0;
    };
    circles.push({ cx: num("cx"), cy: num("cy"), r: num("r") });
  }

  for (const m of svg.matchAll(/<line\b([^>]*)\/?>/g)) {
    const attrs = m[1];
    const num = (name: string) => {
      const am = attrs.match(new RegExp(`${name}="(-?[\\d.]+)"`));
      return am ? parseFloat(am[1]) : 0;
    };
    subpaths.push([{ x: num("x1"), y: num("y1") }, { x: num("x2"), y: num("y2") }]);
  }

  for (const m of svg.matchAll(/<path\b[^>]*\bd="([^"]+)"/g)) {
    subpaths.push(...flattenPathD(m[1]));
  }

  return { circles, subpaths };
}
