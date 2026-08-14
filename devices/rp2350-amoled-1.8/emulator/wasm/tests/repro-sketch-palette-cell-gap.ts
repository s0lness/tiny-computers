// repro-sketch-palette-cell-gap: the assertion this feature never had.
//
// The coordinator captured the palette's real pixels off the board with the
// new `hold` devlink command and measured, on the decoded panel image:
//
//   row 0:  x 12-127   then 128-239
//   row 2:  x 12-127   then 128-239   then 240-354
//
// Cells touching, zero gap, spanning the grid's own full width (354-12=342,
// exactly PALETTE_GRID_W) - a layout bug, and identical at 740ms and 790ms
// into the hold, which rules out the pop's own overshoot caught mid-flight
// (that changes frame to frame; this did not) and is not the settled-path
// erosion or the unpushed-whiten-band bugs fixed in the two rounds before
// this one - both of those were real, and the coordinator confirmed neither
// explains this.
//
// WHAT THIS FILE VERIFIED FIRST, BEFORE ADDING AN ASSERTION FOR IT: whether
// firmware/apps/sketch.c's own palette_cell_bounds(), as committed right
// now, actually drops PALETTE_CELL_GAP_PX. It does not - by hand (halfW =
// (114-8)/2 = 53 for every column, an exact division, no truncation loss:
// PALETTE_GRID_W=342 divides PALETTE_COLS=3 evenly) and by building this
// exact source and reading the rendered framebuffer the same way the
// capture did, which produced real ~9px gaps (cells at 17-122, 131-236,
// 245-350), not the reported 12-127/128-239. The capture almost certainly
// reads a build that predates the source this repository has at HEAD -
// decision 0010's own table already names this exact failure shape twice
// ("tests ran a stale wasm", "harness diffed... against... the build
// actually flashed"). That question needs a real reflash to close, which is
// outside what this file (or any emulator test) can settle - the emulator
// has no board to be stale relative to.
//
// WHAT THIS FILE IS FOR, regardless of how that question resolves: every
// assertion this feature had before it - including the byte-for-byte ones
// (repro-sketch-palette-pop-in-residue.ts) - compares a framebuffer this
// code produced against ANOTHER framebuffer the SAME code produced. A
// constant silently dropped from palette_cell_bounds() would satisfy every
// one of them, because both sides of every comparison would agree, just
// agree on the wrong picture (the same shape of bug decision 0010's own
// table names for the settled-path erosion: "'both paths settle
// identically' | whether the settled picture is right"). This file reads a
// third thing neither existing test does: paletteCellBounds() below is the
// SOURCE's own stated intent (a TypeScript mirror of the formula, not of
// anything rendered), and every assertion measures the ACTUAL pixels
// against THAT, not against each other. A future change that drops or
// misapplies PALETTE_CELL_GAP_PX - subtracted from the wrong term, applied
// to the grid instead of between cells, lost to integer division if the
// inset or column count ever stops dividing evenly - fails here even if
// every other test in this feature still passes, because this is the one
// place the formula and the picture are ever compared to each other instead
// of a render being compared to a render.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/repro-sketch-palette-cell-gap.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const APP_DRAW = 1; // g_apps[] = { chrono, sketch("draw"), timer, four }
const PANEL_W = 368;
const PANEL_H = 448;

// Mirrors of sketch.c's own #defines - see feature-sketch-palette.ts's own
// header comment for the convention this follows.
const CONFIRM_MS = 40;
const LONG_PRESS_MS = 550;
const PALETTE_POP_MS = 160;
const PALETTE_STAGGER_MS = 14;
const PALETTE_COLS = 3;
const PALETTE_ROWS = 3;
const PALETTE_COUNT = PALETTE_COLS * PALETTE_ROWS;
const PALETTE_CELL_GAP_PX = 16; // sketch.c PALETTE_CELL_GAP_PX - sixth round: was 8, this file's own regression
const PALETTE_CANDIDATE_GROW_PX = 3;
const PANEL_BEZEL_MARGIN_PX = 10;
const PALETTE_GRID_INSET_PX = PANEL_BEZEL_MARGIN_PX + PALETTE_CANDIDATE_GROW_PX;
const PALETTE_GRID_W = PANEL_W - 2 * PALETTE_GRID_INSET_PX;
const PALETTE_GRID_H = PANEL_H - 2 * PALETTE_GRID_INSET_PX;
const PALETTE_ANIM_SETTLE_MARGIN_MS = 16;
const PALETTE_ANIM_TOTAL_MS = PALETTE_POP_MS + 2 * PALETTE_STAGGER_MS + PALETTE_ANIM_SETTLE_MARGIN_MS;

// Mirror of sketch.c's palette_cell_bounds() - THE SOURCE'S OWN STATED
// INTENT, not anything rendered. Every assertion below measures pixels
// against this, never against another render.
function paletteCellBounds(index: number) {
    const col = index % PALETTE_COLS;
    const row = Math.floor(index / PALETTE_COLS);
    const x0 = PALETTE_GRID_INSET_PX + Math.floor((PALETTE_GRID_W * col) / PALETTE_COLS);
    const x1 = PALETTE_GRID_INSET_PX + Math.floor((PALETTE_GRID_W * (col + 1)) / PALETTE_COLS);
    const y0 = PALETTE_GRID_INSET_PX + Math.floor((PALETTE_GRID_H * row) / PALETTE_ROWS);
    const y1 = PALETTE_GRID_INSET_PX + Math.floor((PALETTE_GRID_H * (row + 1)) / PALETTE_ROWS);
    const cx = Math.floor((x0 + x1) / 2);
    const cy = Math.floor((y0 + y1) / 2);
    const halfW = Math.floor((x1 - x0 - PALETTE_CELL_GAP_PX) / 2);
    const halfH = Math.floor((y1 - y0 - PALETTE_CELL_GAP_PX) / 2);
    return { col, row, cx, cy, halfW, halfH };
}

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

async function loadDevice() {
    const bytes = readFileSync(WASM_PATH);
    const mod = await WebAssembly.compile(bytes);
    let memory!: WebAssembly.Memory;
    const decoder = new TextDecoder();
    const fwLog: string[] = [];
    const imports = {
        env: {
            js_log(ptr: number, len: number) { const b = new Uint8Array(memory.buffer, ptr, len); fwLog.push(decoder.decode(b)); },
            sinf: (x: number) => Math.sin(x),
            cosf: (x: number) => Math.cos(x),
            atan2f: (y: number, x: number) => Math.atan2(y, x),
            sqrtf: (x: number) => Math.sqrt(x),
            fabsf: (x: number) => Math.abs(x),
            floorf: (x: number) => Math.floor(x),
            fmodf: (x: number, y: number) => x % y,
            powf: (x: number, y: number) => Math.pow(x, y),
            expf: (x: number) => Math.exp(x),
        },
    };
    const instance = await WebAssembly.instantiate(mod, imports);
    memory = instance.exports.memory as WebAssembly.Memory;
    const exp = instance.exports as any;
    if (exp.emu_init() !== 1) throw new Error("emu_init() failed - see fw log lines above");

    function fbSnapshot(): Uint8Array {
        const ptr = exp.emu_fb();
        return new Uint8Array(memory.buffer, ptr, PANEL_W * PANEL_H * 2).slice();
    }

    return {
        tick(nowMs: number) { exp.emu_tick(nowMs); },
        touch(down: boolean, x: number, y: number) { exp.emu_touch(down ? 1 : 0, Math.round(x), Math.round(y)); },
        appSwitch(index: number) { exp.emu_app_switch(index); },
        drainLog(): string[] { const out = fwLog.slice(); fwLog.length = 0; return out; },
        fbSnapshot,
    };
}

function isWhite(fb: Uint8Array, x: number, y: number): boolean {
    const idx = (y * PANEL_W + x) * 2;
    return fb[idx] === 0xff && fb[idx + 1] === 0xff;
}

// The last ink pixel of a run starting near x0 and growing toward x1's own
// direction, and the first ink pixel found scanning the other way - used to
// measure one cell's own actual left/right (or top/bottom) ink edge along a
// scanline through its centre, the widest point of a rounded rect and the
// one furthest from any corner's own carve.
function inkExtentAlongX(fb: Uint8Array, y: number, xNear: number, xFar: number): number | null {
    const step = xFar >= xNear ? 1 : -1;
    for (let x = xNear; x !== xFar + step; x += step) {
        if (!isWhite(fb, x, y)) return x;
    }
    return null;
}
function inkExtentAlongY(fb: Uint8Array, x: number, yNear: number, yFar: number): number | null {
    const step = yFar >= yNear ? 1 : -1;
    for (let y = yNear; y !== yFar + step; y += step) {
        if (!isWhite(fb, x, y)) return y;
    }
    return null;
}

// The OPPOSITE walk from inkExtentAlongX/Y above, for the pair checks below:
// starts INSIDE a cell's own ink (its centre - the widest point, furthest
// from any rounded corner) and walks TOWARD a neighbour, returning the LAST
// pixel that is still ink before the first white one - the cell's own edge
// nearest that neighbour. inkExtentAlongX/Y would return the starting
// point itself here, trivially, since a cell's own centre is never white.
function inkEdgeTowardX(fb: Uint8Array, y: number, xStart: number, xToward: number): number {
    const step = xToward >= xStart ? 1 : -1;
    let last = xStart;
    for (let x = xStart; x !== xToward + step; x += step) {
        if (isWhite(fb, x, y)) break;
        last = x;
    }
    return last;
}
function inkEdgeTowardY(fb: Uint8Array, x: number, yStart: number, yToward: number): number {
    const step = yToward >= yStart ? 1 : -1;
    let last = yStart;
    for (let y = yStart; y !== yToward + step; y += step) {
        if (isWhite(fb, x, y)) break;
        last = y;
    }
    return last;
}

async function main() {
    console.log("=== reproduction + regression: the rendered grid must match palette_cell_bounds()'s own formula, not just itself ===\n");

    const dev = await loadDevice();
    dev.tick(0);
    dev.appSwitch(APP_DRAW);
    dev.tick(1000);
    dev.drainLog();

    // Open on the centre cell (index 4) and hold perfectly still through the
    // whole pop-in and well past it - the exact scenario repro-sketch-
    // palette-pop-in-residue.ts already proved converges to a stable,
    // byte-identical-to-hand-off picture, so what is measured below is the
    // feature's own steady state, not a transient.
    let t = 1000;
    const { cx: holdCx, cy: holdCy } = paletteCellBounds(4);
    let opened = false;
    for (let held = 0; held < CONFIRM_MS + LONG_PRESS_MS + 100 && !opened; held += 15) {
        t += 15;
        dev.touch(true, holdCx, holdCy);
        dev.tick(t);
        if (dev.drainLog().some((l) => l.includes("palette: open"))) opened = true;
    }
    check("the palette opened via a genuine long press", opened);

    for (let elapsed = 0; elapsed < PALETTE_ANIM_TOTAL_MS + 200; elapsed += 5) {
        t += 5;
        dev.touch(true, holdCx, holdCy);
        dev.tick(t);
        dev.drainLog();
    }

    // One more move, to the gap between column 0 and column 1 on the middle
    // row - not over any cell's own nominal box, so the candidate resolves
    // to none and the hand-off path (palette_render_handoff(), already
    // proven correct on its own - see repro-sketch-palette-pop-in-
    // residue.ts) shrinks cell 4 back down from its grown highlight. Every
    // one of the nine cells is then at its plain, ungrown, settled size -
    // exactly what palette_cell_bounds() itself describes, with no
    // PALETTE_CANDIDATE_GROW_PX to account for in the checks below.
    const gapX = paletteCellBounds(0).cx + paletteCellBounds(0).halfW + PALETTE_CELL_GAP_PX / 2;
    const gapY = paletteCellBounds(4).cy;
    t += 15;
    dev.touch(true, gapX, gapY);
    dev.tick(t);
    dev.drainLog();
    t += 15;
    dev.touch(true, gapX, gapY);
    dev.tick(t);
    dev.drainLog();

    const fb = dev.fbSnapshot();

    // ---- horizontal pairs: every (left, right) sharing a row. -----------
    const horizontalPairs: [number, number][] = [[0, 1], [1, 2], [3, 4], [4, 5], [6, 7], [7, 8]];
    let worstHorizGap = Infinity;
    for (const [li, ri] of horizontalPairs) {
        const left = paletteCellBounds(li);
        const right = paletteCellBounds(ri);
        // Sanity first: each cell's own centre (the widest point of a
        // rounded rect, furthest from any corner's own carve) must actually
        // be ink - otherwise the edge walk below trivially "succeeds" from
        // a blank cell and proves nothing.
        const bothInked = !isWhite(fb, left.cx, left.cy) && !isWhite(fb, right.cx, right.cy);
        check(`cell ${li}/${ri} both have ink on their shared row centre (sanity)`, bothInked);
        if (!bothInked) continue;
        // Walk that shared centre line FROM each cell's own centre TOWARD
        // the other, so the LAST ink pixel found before white IS that
        // cell's own edge nearest the gap.
        const leftInkEdge = inkEdgeTowardX(fb, left.cy, left.cx, right.cx);
        const rightInkEdge = inkEdgeTowardX(fb, right.cy, right.cx, left.cx);
        const measuredGap = rightInkEdge - leftInkEdge - 1;
        const expectedLeftEdge = left.cx + left.halfW; // formula's own nominal right edge of the left cell
        const expectedRightEdge = right.cx - right.halfW; // formula's own nominal left edge of the right cell
        const expectedGap = expectedRightEdge - expectedLeftEdge - 1;
        console.log(`    cells ${li}/${ri}: measured ink edges (${leftInkEdge},${rightInkEdge}) gap=${measuredGap}px - formula expects edges (${expectedLeftEdge},${expectedRightEdge}) gap=${expectedGap}px`);
        if (measuredGap < worstHorizGap) worstHorizGap = measuredGap;
        // A real gap, not the whole PALETTE_CELL_GAP_PX to the pixel (the
        // rounded rect's own SDF antialiasing softens the exact threshold
        // by a pixel or two) - but nowhere near zero, which is what a
        // dropped gap term (this round's own regression, on real hardware)
        // looks like: cells touching, measured gap 0 or negative.
        check(`cells ${li}/${ri} have a real gap between them, not touching (measured ${measuredGap}px, formula expects ~${expectedGap}px)`,
            measuredGap >= PALETTE_CELL_GAP_PX - 3, `measured=${measuredGap}px`);
    }

    // ---- vertical pairs: every (top, bottom) sharing a column. -----------
    const verticalPairs: [number, number][] = [[0, 3], [3, 6], [1, 4], [4, 7], [2, 5], [5, 8]];
    let worstVertGap = Infinity;
    for (const [ti, bi] of verticalPairs) {
        const top = paletteCellBounds(ti);
        const bottom = paletteCellBounds(bi);
        const bothInked = !isWhite(fb, top.cx, top.cy) && !isWhite(fb, bottom.cx, bottom.cy);
        check(`cell ${ti}/${bi} both have ink on their shared column centre (sanity)`, bothInked);
        if (!bothInked) continue;
        const topInkEdge = inkEdgeTowardY(fb, top.cx, top.cy, bottom.cy);
        const bottomInkEdge = inkEdgeTowardY(fb, bottom.cx, bottom.cy, top.cy);
        const measuredGap = bottomInkEdge - topInkEdge - 1;
        const expectedTopEdge = top.cy + top.halfH;
        const expectedBottomEdge = bottom.cy - bottom.halfH;
        const expectedGap = expectedBottomEdge - expectedTopEdge - 1;
        console.log(`    cells ${ti}/${bi}: measured ink edges (${topInkEdge},${bottomInkEdge}) gap=${measuredGap}px - formula expects edges (${expectedTopEdge},${expectedBottomEdge}) gap=${expectedGap}px`);
        if (measuredGap < worstVertGap) worstVertGap = measuredGap;
        check(`cells ${ti}/${bi} have a real gap between them, not touching (measured ${measuredGap}px, formula expects ~${expectedGap}px)`,
            measuredGap >= PALETTE_CELL_GAP_PX - 3, `measured=${measuredGap}px`);
    }

    // ---- every cell's own measured width/height against the formula's
    // own halfW/halfH - directly catches "the gap term got dropped and
    // every cell is wider than it should be", the coordinator's own
    // measurement (116px and 112px against a formula-intended 106px). -----
    // A search margin past each cell's own formula-predicted edge, generous
    // enough to catch a real overshoot-shaped discrepancy but never so wide
    // it crosses into a NEIGHBOUR's own ink - which would misreport that
    // neighbour's edge as this cell's own. PALETTE_CELL_GAP_PX/2 - 1 stays
    // inside this cell's own half of the gap, whatever the gap's own
    // current value is (this is the same reason the pair checks above scan
    // centre-to-centre instead of using a fixed pixel count).
    const OWN_WIDTH_MARGIN_PX = Math.max(1, Math.floor(PALETTE_CELL_GAP_PX / 2) - 1);
    for (let i = 0; i < PALETTE_COUNT; i++) {
        const b = paletteCellBounds(i);
        // Clamped to the panel: an outer cell's own search margin can reach
        // past x=0 or x=PANEL_W-1 (cell 0's own left edge sits close to the
        // panel's own left edge) - clamping keeps the scan inside the
        // framebuffer rather than reading a negative index (isWhite() would
        // read undefined there and misreport an edge at the very first,
        // out-of-bounds step).
        const leftEdge = inkExtentAlongX(fb, b.cy, Math.max(0, b.cx - b.halfW - OWN_WIDTH_MARGIN_PX), b.cx);
        const rightEdge = inkExtentAlongX(fb, b.cy, Math.min(PANEL_W - 1, b.cx + b.halfW + OWN_WIDTH_MARGIN_PX), b.cx);
        if (leftEdge !== null && rightEdge !== null) {
            const measuredWidth = rightEdge - leftEdge + 1;
            const expectedWidth = 2 * b.halfW;
            check(`cell ${i}'s own measured width matches palette_cell_bounds()'s own formula (within 6px)`,
                Math.abs(measuredWidth - expectedWidth) <= 6, `measured=${measuredWidth}px, formula=${expectedWidth}px`);
        } // else: covered by the horizontal pair checks above already

        const topEdge = inkExtentAlongY(fb, b.cx, Math.max(0, b.cy - b.halfH - OWN_WIDTH_MARGIN_PX), b.cy);
        const bottomEdge = inkExtentAlongY(fb, b.cx, Math.min(PANEL_H - 1, b.cy + b.halfH + OWN_WIDTH_MARGIN_PX), b.cy);
        if (topEdge !== null && bottomEdge !== null) {
            const measuredHeight = bottomEdge - topEdge + 1;
            const expectedHeight = 2 * b.halfH;
            check(`cell ${i}'s own measured height matches palette_cell_bounds()'s own formula (within 6px)`,
                Math.abs(measuredHeight - expectedHeight) <= 6, `measured=${measuredHeight}px, formula=${expectedHeight}px`);
        } // else: covered by the vertical pair checks above already
    }

    console.log(`\n    worst measured horizontal gap: ${worstHorizGap}px, worst vertical gap: ${worstVertGap}px (PALETTE_CELL_GAP_PX=${PALETTE_CELL_GAP_PX})`);
    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
