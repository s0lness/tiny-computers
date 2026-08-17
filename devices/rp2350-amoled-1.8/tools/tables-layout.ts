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
 * UPDATED AGAIN, TWICE, for the owner's THIRD drawing (2026-08-17, held
 * portrait). First relayed as "stay in landscape, just shuffle the bands" -
 * a version of this file briefly widened the pad and moved the counters to
 * a bottom band while keeping LAND_W/LAND_H and landToPanel(). That reading
 * was corrected once the owner specified the physical grip precisely (BOOT
 * upper, PWR lower on the panel's own right edge - checked against
 * emu_shim.c's own button descriptor before trusting it, see tables.c's own
 * LAYOUT comment for the full check): this is genuinely portrait, PANEL_W
 * wide by PANEL_H tall, no rotation, so LAND_W/LAND_H/landToPanel() are
 * gone from this file - touch coordinates a test drives ARE panel
 * coordinates now, no conversion needed. Along with the orientation
 * correction, the counters' pills moved from PINK to ORANGE
 * (pill_color_wrong() in tables.c, sketch.c's own palette orange) and the
 * backspace glyph from a bare chevron to a proper arrow.
 *
 * THE HONEST LIMIT: this is still NOT read from tables.c itself. There is
 * no build step that extracts a C #define into TypeScript on this project,
 * and adding one would mean touching emu_abi.h/emu_shim.c/build.ts, which
 * other agents have been actively editing in this same tree while this
 * file was written - not a safe place to add new shared surface today. So
 * this remains a byte-for-byte mirror of tables.c's own LAYOUT/NUMPAD/
 * QUESTION BAND/COUNTER PILLS #defines, kept in sync BY HAND, the same
 * discipline every one of the four consumer files already relied on
 * individually. What changes is that there is now exactly one place to
 * update, and CI-shaped drift (four copies silently diverging) can no
 * longer happen unnoticed.
 *
 * If a future change makes emu_device() (the JSON blob every one of these
 * tools already reads for the app index, never a hardcoded number) carry
 * layout too, this file is where that would land instead - the four
 * consumers below would not need to change their own imports.
 */

// ---- THE PANEL, and the bezel every element is inset from -----------------
// Portrait, native, unrotated - PANEL_W is the SHORT axis (368), PANEL_H
// the LONG one (448), same orientation the framebuffer itself uses.
export const PANEL_W = 368, PANEL_H = 448;
export const OX = 10, OY = 10;
export const USABLE_W = PANEL_W - 2 * OX; // 348
export const USABLE_H = PANEL_H - 2 * OY; // 428

// ---- THE QUESTION BAND (tables.c's QROW_*) ---------------------------------
export const QROW_Y0 = OY;
export const QROW_H = 64;
export const QROW_CY = OY + Math.floor(QROW_H / 2); // 42

// ---- THE LOUPE ZONE, between the question band and the pad ----------------
export const LOUPE_ZONE_H = 80; // = LOUPE_BOX_H exactly, tables.c's THE LOUPE - unchanged since the app's first version
export const LOUPE_R = 34, LOUPE_PAD = 6;
export const LOUPE_BOX_W = LOUPE_R * 2 + 2 * LOUPE_PAD, LOUPE_BOX_H = LOUPE_BOX_W; // 80
export const LOUPE_CY = OY + QROW_H + Math.floor(LOUPE_ZONE_H / 2); // 114

// ---- THE NUMPAD (tables.c's THE NUMPAD) ------------------------------------
// CELL_W is 100, not "however much width is left over" (an earlier pass
// stretched this to 140 and a rendered preview showed why that is wrong:
// at 140x38 a cell is a 3.7:1 sliver, three isolated digit columns rather
// than a grid - see tables.c's own comment on this constant). CELL_H (57)
// is taller here than any earlier version of this pad managed, landscape
// or portrait-shuffled, because portrait's own 428px of usable height is
// markedly more generous than the 348px this app fit the same four
// stacked bands into during its landscape detour.
export const NUMPAD_COLS = 3, NUMPAD_ROWS = 4;
export const CELL_W = 100, CELL_H = 57;
export const NUMPAD_W = CELL_W * NUMPAD_COLS; // 300
export const NUMPAD_H = CELL_H * NUMPAD_ROWS; // 228
export const NUMPAD_X0 = OX + (USABLE_W - NUMPAD_W) / 2; // 34 - centred
export const NUMPAD_Y0 = OY + QROW_H + LOUPE_ZONE_H; // 154

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
// Smaller than the app's landscape-detour numbers on purpose: portrait's
// own USABLE_W (348) is narrower than landscape's (428), a genuine width
// constraint independent of portrait's extra vertical room - see tables.c's
// own comment on Q_LPAD/Q_GAP/QDIGIT_W/QICON_BOX for the worst-case sum
// that had to fit inside it.
const Q_FACTOR_X0 = 112, Q_GAP = 14, QDIGIT_W = 34, QICON_BOX = 26, Q2W = 72;
export const Q_SLOT_X0_NARROW = Q_FACTOR_X0 + QDIGIT_W + Q_GAP + QICON_BOX + Q_GAP; // 200, 1-digit factor
export const Q_SLOT_X0_WIDE = Q_FACTOR_X0 + Q2W + Q_GAP + QICON_BOX + Q_GAP;        // 238, 2-digit factor ("10")
export const CURSOR_X_CANDIDATES = [Q_SLOT_X0_NARROW + 4, Q_SLOT_X0_WIDE + 4]; // tables.c's cursor_x_for_len(0)

// ---- THE COUNTER PILLS (tables.c's THE COUNTER PILLS) ---------------------
// Two coloured pills side by side along the very bottom, spanning the FULL
// usable width (NOT tied to NUMPAD_W - a pill is a readout, not a key, so
// it is free to be wider than the pad's own key-driven width) - replacing
// the earlier boxed, stacked counters (see git history for BOX_X0/BOX_Y0/
// COUNTER_ROW_* if that layout is ever needed again). Wrong is ORANGE
// (tables.c's pill_color_wrong(), sketch.c's own palette_color(1)), right
// is green (pill_color_right(), palette_color(3)) - the owner's own
// instruction to reuse a colour this device already has rather than a new
// one.
export const COUNTERS_Y0 = NUMPAD_Y0 + NUMPAD_H; // 382 - the pad's own bottom edge
export const COUNTERS_H = USABLE_H - QROW_H - LOUPE_ZONE_H - NUMPAD_H; // 56
export const PILL_GAP = 20, PILL_H = 44;
export const PILL_W = (USABLE_W - PILL_GAP) / 2; // 164
export const PILL_Y0 = COUNTERS_Y0 + (COUNTERS_H - PILL_H) / 2; // 388
export const PILL_WRONG_X0 = OX; // 10 - flush with the panel's own left margin
export const PILL_RIGHT_X0 = OX + PILL_W + PILL_GAP; // 194 - flush with the panel's own right margin
export const PILL_ICON_CX_OFF = 42, PILL_NUM_CX_OFF = 100;

// ---- touch coordinates -----------------------------------------------------
// Portrait, native, unrotated: a cell centre IS the panel point to feed
// emu_touch(), no rotation to undo. Kept as a named export (rather than
// having every consumer round cellCx()/cellCy() itself) because that is
// what the four consumer files already call this shape of thing, and a
// same-named drop-in keeps their own diffs to an import line - see git
// history for this file's own landToPanel(), which did the landscape
// rotation the app no longer needs.
export function panelPoint(x: number, y: number): [number, number] {
  return [Math.round(x), Math.round(y)];
}
