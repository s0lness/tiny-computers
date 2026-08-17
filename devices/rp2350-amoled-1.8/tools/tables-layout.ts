/**
 * tables-layout: firmware/apps/tables.c's own layout constants, in ONE
 * place on the TypeScript side, so every test and sweep tool that has to
 * hit-test the numpad or read the question band's geometry imports them
 * from here instead of copying the numbers by hand into each file.
 *
 * WHY THIS EXISTS. Before this file, four separate TS files each carried
 * their own hand-copied `CELL_W = 99, CELL_H = 64, NUMPAD_Y0 = OY + 92`
 * (feature-tables.ts, preview-tables.ts, repro-touch-dropout-tables.ts,
 * sweep-tables-grace.ts). When tables.c's layout changed under the owner's
 * redrawn mockup (2026-08-17), two of those four were updated and two were
 * not - repro-touch-dropout-tables.ts and sweep-tables-grace.ts kept
 * hit-testing the OLD numpad rectangle against the NEW one, which does not
 * fail loudly: numpad_hit()-equivalent code just returns -1 (or the wrong
 * cell) for coordinates that no longer land where the real firmware puts
 * them, so the tool keeps printing PASS while silently exercising nothing
 * real. A silently-hollow test is worse than no test, because it still
 * looks like coverage. One shared file cannot fully close that gap on its
 * own - see the honest limit below - but it turns "four copies to keep in
 * sync by hand" into "one copy, checked once", which is the part that is
 * actually fixable here.
 *
 * THE HONEST LIMIT: this is still NOT read from tables.c itself. There is
 * no build step that extracts a C #define into TypeScript on this project,
 * and adding one would mean touching emu_abi.h/emu_shim.c/build.ts, which
 * three other agents are actively editing in this same tree as this file
 * is written - not a safe place to add new shared surface today. So this
 * remains a byte-for-byte mirror of tables.c's own LAYOUT/NUMPAD/QUESTION
 * BAND/COUNTERS PANEL #defines, kept in sync BY HAND, the same discipline
 * every one of the four files already relied on individually. What changes
 * is that there is now exactly one place to update, and CI-shaped drift
 * (four copies silently diverging) can no longer happen unnoticed.
 *
 * If a future change makes emu_device() (the JSON blob every one of these
 * tools already reads for the app index, never a hardcoded number) carry
 * layout too, this file is where that would land instead - the four
 * consumers below would not need to change their own imports.
 */

// ---- THE PANEL, and the bezel every element is inset from -----------------
export const PANEL_W = 368, PANEL_H = 448;
export const LAND_W = PANEL_H, LAND_H = PANEL_W; // 448 x 368
export const OX = 10, OY = 10;
export const USABLE_W = LAND_W - 2 * OX; // 428
export const USABLE_H = LAND_H - 2 * OY; // 348

// ---- THE QUESTION BAND (tables.c's QROW_*) ---------------------------------
export const QROW_Y0 = OY;
export const QROW_H = 80;
export const QROW_CY = OY + Math.floor(QROW_H / 2); // 50

// ---- THE LOUPE ZONE, between the question band and the pad ----------------
export const LOUPE_ZONE_H = 80; // = LOUPE_BOX_H exactly, tables.c's THE LOUPE

// ---- THE NUMPAD (tables.c's THE NUMPAD) ------------------------------------
export const NUMPAD_COLS = 3, NUMPAD_ROWS = 4;
export const CELL_W = 95, CELL_H = 47;
export const NUMPAD_W = CELL_W * NUMPAD_COLS; // 285
export const NUMPAD_H = CELL_H * NUMPAD_ROWS; // 188
export const NUMPAD_X0 = OX;
export const NUMPAD_Y0 = OY + QROW_H + LOUPE_ZONE_H; // 170

export const CELL_BACK = 9, CELL_ZERO = 10, CELL_CHECK = 11;
export const cellCx = (c: number) => NUMPAD_X0 + (c % NUMPAD_COLS) * CELL_W + CELL_W / 2;
export const cellCy = (c: number) => NUMPAD_Y0 + Math.floor(c / NUMPAD_COLS) * CELL_H + CELL_H / 2;
export const digitCell = (d: number) => (d === 0 ? CELL_ZERO : d - 1);
export const cellValueName = (c: number) =>
  c === CELL_BACK ? "back" : c === CELL_CHECK ? "check" : c === CELL_ZERO ? "0" : String(c + 1);

// ---- THE QUESTION BAND'S OWN LAYOUT (tables.c's Q_*) -----------------------
// The blank's own x0 depends on whether the CURRENT question's factor is
// one digit or two (tables.c's question_slot_x0(), the fix for the
// "6 x    1 =" spacing hole) - a test that does not know the first
// question's factor in advance has to check both candidate positions.
const Q_FACTOR_X0 = 142, Q_GAP = 20, QDIGIT_W = 36, QICON_BOX = 32, Q2W = 76;
export const Q_SLOT_X0_NARROW = Q_FACTOR_X0 + QDIGIT_W + Q_GAP + QICON_BOX + Q_GAP; // 250, 1-digit factor
export const Q_SLOT_X0_WIDE = Q_FACTOR_X0 + Q2W + Q_GAP + QICON_BOX + Q_GAP;        // 290, 2-digit factor ("10")
export const CURSOR_X_CANDIDATES = [Q_SLOT_X0_NARROW + 4, Q_SLOT_X0_WIDE + 4]; // tables.c's cursor_x_for_len(0)

// ---- THE COUNTERS PANEL (tables.c's THE COUNTERS PANEL / the box) --------
const INFO_X0 = NUMPAD_X0 + NUMPAD_W;       // 295
const INFO_W = USABLE_W - NUMPAD_W;         // 143
export const BOX_X0 = INFO_X0 + 8;                       // 303
export const BOX_Y0 = NUMPAD_Y0 + CELL_H;                // 217 - skip the pad's own top row
export const BOX_H = NUMPAD_H - CELL_H - 6;              // 135 - clear of the bezel, see tables.c's own comment
const BOX_INSET = 5;
export const COUNTER_ROW_X0 = BOX_X0 + BOX_INSET;        // 308
export const COUNTER_ROW_W = INFO_W - 16 - 2 * BOX_INSET; // 117
export const COUNTERS_Y0 = BOX_Y0 + BOX_INSET;            // 222
export const COUNTER_ROW_H = Math.floor((BOX_H - 2 * BOX_INSET) / 2); // 62 (2 rows)

// ---- landscape/panel conversion, the same isometry gfx.h uses -------------
export function landToPanel(lx: number, ly: number): [number, number] {
  return [PANEL_W - 1 - Math.round(ly), Math.round(lx)];
}
