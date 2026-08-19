/**
 * The menu grid's geometry, in ONE place for everything host-side.
 *
 * WHY THIS FILE EXISTS. menu.c owns the real layout, and until now every
 * host-side reader kept its own hand-copied mirror of it: tools/preview-menu-
 * grid.ts, emulator/wasm/tests/feature-menu-hover.ts and
 * emulator/wasm/tests/repro-menu-icon-resample-ghost.ts each re-derived the
 * same four formulas. The moment the grid started centring itself vertically
 * (menu.c's menu_grid_y0(), added when a five-app roster left a quarter of
 * the panel bare), all three went stale at once and reported failures that
 * were entirely their own: a halo "outside its own cell", corners that no
 * longer lit, ring holes that were not where they were looked for. Nothing
 * was wrong with the firmware in any of those runs.
 *
 * tools/tables-layout.ts already exists for exactly this reason on the other
 * app that grew a second and third reader. Same shape, same argument.
 *
 * HONEST LIMIT, stated the same way tables-layout.ts states it: this is a
 * mirror kept in sync BY HAND against menu.c, not generated from it. What it
 * buys is one place to fix instead of three, not a guarantee. If menu.c's
 * formulas change, change them here too, and the previews and tests follow.
 */

export const PANEL_W = 368;
export const PANEL_H = 448;
export const LAND_W = PANEL_H; // 448
export const LAND_H = PANEL_W; // 368

export const MENU_CELL_FLOOR = 112;
export const MENU_COLS_MAX = 4;
export const MENU_TOP_INSET = 10; // gfx.h PANEL_BEZEL_MARGIN_PX
export const MENU_CANCEL_FLOOR = 22;
export const MENU_AVAIL_H = LAND_H - MENU_TOP_INSET - MENU_CANCEL_FLOOR; // 336
export const MENU_ROWS_MAX = Math.floor(MENU_AVAIL_H / MENU_CELL_FLOOR); // 3

export interface Rect { bx: number; by: number; bw: number; bh: number }

export function menuRows(n: number): number {
  return Math.min(Math.max(Math.ceil(n / MENU_COLS_MAX), 1), MENU_ROWS_MAX);
}

export function rowSpan(n: number, r: number): { first: number; cols: number } {
  const rows = menuRows(n);
  const base = Math.floor(n / rows);
  const extra = n % rows;
  return { first: r * base + Math.min(r, extra), cols: base + (r < extra ? 1 : 0) };
}

// menu_cell_h(): the smaller of (available height / rows) and the narrowest
// row's own width, rounded DOWN to a multiple of 8 (decision 0001's push
// rule), never below the floor.
export function menuCellH(n: number): number {
  const rows = menuRows(n);
  const byHeight = Math.floor(MENU_AVAIL_H / rows);
  let narrowest = LAND_W;
  for (let r = 0; r < rows; r++) {
    narrowest = Math.min(narrowest, Math.floor(LAND_W / rowSpan(n, r).cols));
  }
  const byWidth = Math.floor((narrowest * 6) / 5); // menu.c: up to 1.2x width
  // menu.c's third bound: never grow so tall that menu_grid_y0()'s clamp is
  // forced to bind, which would silently turn "centred" into "packed against
  // the top inset". See menu_cell_h()'s own comment for the six-app case that
  // exposed it.
  const byCentred = Math.floor((LAND_H - 2 * MENU_CANCEL_FLOOR) / rows);
  return Math.max(Math.floor(Math.min(byHeight, byWidth, byCentred) / 8) * 8, MENU_CELL_FLOOR);
}

// menu_grid_y0(): the block of rows is centred in the full landscape height,
// because a release above the grid cancels exactly as a release below it
// does - so the leftover is one cancel zone in two halves, not a band at the
// bottom plus an accident at the top. Clamped so centring never spends the
// cancel floor: at eleven and twelve apps the slack is 32px and half of it
// would leave 16 below against a floor of 22, so those rosters stay at
// MENU_TOP_INSET and do not move at all.
export function menuGridY0(n: number): number {
  const grid = menuRows(n) * menuCellH(n);
  let y0 = Math.floor((LAND_H - grid) / 2);
  const maxY0 = LAND_H - MENU_CANCEL_FLOOR - grid;
  if (y0 > maxY0) y0 = maxY0;
  return y0 < MENU_TOP_INSET ? MENU_TOP_INSET : y0;
}

// cell_rect_land(): contiguous and full-width within its row, the LAST cell
// absorbing whatever LAND_W does not divide evenly, so every x belongs to
// exactly one cell and none of it is dead space a touch can fall into.
export function cellRect(n: number, i: number): Rect {
  const rows = menuRows(n);
  const cellH = menuCellH(n);
  for (let r = 0; r < rows; r++) {
    const { first, cols } = rowSpan(n, r);
    if (i < first + cols) {
      const c = i - first;
      const w = Math.floor(LAND_W / cols);
      const bx = c * w;
      return { bx, by: menuGridY0(n) + r * cellH, bw: c === cols - 1 ? LAND_W - bx : w, bh: cellH };
    }
  }
  throw new Error(`no cell for index ${i} at ${n} apps`);
}
