// feature-sketch-palette: headless verification of the sketchpad's
// EXPERIMENTAL colour palette (firmware/apps/sketch.c, the section headed
// "EXPERIMENTAL: the colour palette").
//
// REWORKED alongside the feature itself, twice in one day: first from a
// single row of 4 flat squares to a 3x3 grid of 9 that may cover the whole
// panel (the owner's "peut-etre que long press montre 9 rectangles de
// couleur et relacher en selectionne un", then "c'est ok si les cases
// recouvrent tout l'ecran"), then from flat squares to animated, rounded
// "little balloons" (decision 0009, and the owner's own "i'd love for them
// to pop out like little balloons. they should be rounded rectangles").
// Covering the whole panel also forced the underlying mechanism to change:
// there is no room in the 64KB arena for a saved copy of a 322KB panel, so
// the palette's undo/close now replays a bounded stroke history instead of
// restoring saved pixels - see sketch.c's own "Stroke history" comment
// section for the full design.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/feature-sketch-palette.ts
//
// Loads the REAL firmware compiled to wasm, same shape as every other file
// in this directory (decision 0003) - nothing here reimplements sketch.c's
// logic; every assertion reads the framebuffer, the firmware's own log
// lines, or the push-window list the firmware itself produced.
//
// WHAT THIS FILE PROVES, IN ORDER:
//   1. a long press opens the palette, its animation pops all nine cells
//      in over a bounded number of ticks, and EVERY one of those ticks -
//      not just the final settled frame - obeys decision 0001's 8px rule
//      and the "no pixel changes outside the pushed rectangle" invariant;
//   2. whichever cell the finger is already over is visibly the candidate
//      (grown past its neighbours' size, at the identical relative
//      offset), and every cell is a rounded rectangle, not a square - its
//      own bounding-box corner stays white while its centre is ink;
//   3. dragging onto a cell and releasing there picks that colour without
//      ever lifting, and the next stroke draws in it;
//   4. holding still in the gap between cells and releasing without ever
//      moving - "what happens if she never moves at all" landing exactly
//      on "over nothing" - cancels: the canvas, repainted from stroke
//      history, comes back BYTE-IDENTICAL to what it was before that
//      second palette opened, and the previously picked colour is still
//      in effect (a cancel does not reset the tool).
//
// Screenshots are written to preview/palette-*.png as real RGB PNGs (the
// framebuffer's own rgb565be format, per emu_abi.h's emu_device() panel
// section, unpacked to 24-bit) - a greyscale encoder cannot show the point
// of this feature, so this file carries its own small RGB PNG writer
// rather than reaching for tools/dev.ts's greyscale one.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const ROOT = join(import.meta.dir, "..", "..", "..");
const PREVIEW_DIR = join(ROOT, "preview");

const PANEL_W = 368;
const PANEL_H = 448;
const APP_DRAW = 1; // g_apps[] = { chrono, sketch("draw"), timer }
const APP_ARENA_BYTES = 65536; // app.h APP_ARENA_BYTES

// ---- mirrors of sketch.c's own constants - lifted, not re-derived, same
// convention every other repro test in this directory uses for the app it
// drives. If sketch.c's own numbers change, this file's own comments point
// back at the #define this mirrors so the two stay easy to keep in sync by
// hand.
const CONFIRM_MS = 40; // sketch.c CONFIRM_MS_DEFAULT
const LONG_PRESS_MS = 550; // sketch.c LONG_PRESS_MS
const LIFT_DEBOUNCE_MS = 220; // sketch.c LIFT_DEBOUNCE_MS_DEFAULT
const PALETTE_LIFT_GRACE_MS = 80; // sketch.c PALETTE_LIFT_GRACE_MS
const PALETTE_COLS = 3; // sketch.c PALETTE_COLS
const PALETTE_ROWS = 3; // sketch.c PALETTE_ROWS
const PALETTE_COUNT = PALETTE_COLS * PALETTE_ROWS; // 9
const PALETTE_CELL_GAP_PX = 8; // sketch.c PALETTE_CELL_GAP_PX
const PALETTE_CORNER_PX = 26; // sketch.c PALETTE_CORNER_PX
const PALETTE_CANDIDATE_GROW_PX = 3; // sketch.c PALETTE_CANDIDATE_GROW_PX
const PALETTE_POP_MS = 240; // sketch.c PALETTE_POP_MS
const PALETTE_STAGGER_MS = 20; // sketch.c PALETTE_STAGGER_MS
// gfx.h's own device-wide bezel-hidden margin (see its own comment for the
// number and how it was found - a photograph, not a datasheet) - the
// palette grid insets by this plus the candidate's own grow, see sketch.c
// PALETTE_GRID_INSET_PX's own comment for why.
const PANEL_BEZEL_MARGIN_PX = 10; // gfx.h PANEL_BEZEL_MARGIN_PX
const PALETTE_GRID_INSET_PX = PANEL_BEZEL_MARGIN_PX + PALETTE_CANDIDATE_GROW_PX; // sketch.c PALETTE_GRID_INSET_PX
const PALETTE_GRID_W = PANEL_W - 2 * PALETTE_GRID_INSET_PX; // sketch.c PALETTE_GRID_W
const PALETTE_GRID_H = PANEL_H - 2 * PALETTE_GRID_INSET_PX; // sketch.c PALETTE_GRID_H
// The animation's own worst case: the corner cells' own stagger delay
// (Chebyshev rank 2) plus their own pop duration - mirrors palette_render_
// frame/palette_drain_sample's own "animating" threshold.
const PALETTE_ANIM_TOTAL_MS = PALETTE_POP_MS + 2 * PALETTE_STAGGER_MS;

// Mirror of sketch.c's palette_cell_bounds(): the raw grid cell (index =
// row*PALETTE_COLS + col) tiles the VISIBLE area - PALETTE_GRID_W x
// PALETTE_GRID_H, inset from the panel's own edges by PALETTE_GRID_INSET_
// PX, not the panel itself - exactly, by the standard "i*N/D" integer
// partition; the balloon itself is inset by half the gap on every side.
function paletteCellBounds(index: number): { col: number; row: number; cx: number; cy: number; halfW: number; halfH: number } {
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

// Mirror of sketch.c's palette_color(): the nine swapped-RGB565 byte pairs
// AMOLED_1IN8_DisplayWindows actually receives, in the framebuffer's own
// big-endian storage order (emu_abi.h: panel.format "rgb565be") - see this
// file's decodeRgb565Be() for the read side of the same convention.
const PALETTE_COLORS_BE: [number, number][] = [
    [0xf8, 0x00], // red
    [0xfc, 0x60], // orange
    [0xff, 0xe0], // yellow
    [0x07, 0xe0], // green
    [0x00, 0x00], // black (centre, index 4)
    [0x00, 0x1f], // blue
    [0x98, 0x1f], // purple
    [0xfb, 0x56], // pink
    [0x8a, 0x22], // brown
];
const COLOR_NAMES = ["red", "orange", "yellow", "green", "black", "blue", "purple", "pink", "brown"];
const INDEX_RED = 0;
const INDEX_CENTER_BLACK = 4;

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

// ---------------------------------------------------------------------
// Invariant tracking, same shape as repro-timer-coil.ts's own (see that
// file's header for why this lives inline in every test rather than a
// shared module: each test file in this directory is self-contained).
// ---------------------------------------------------------------------
type Violation = { kind: string; detail: string };
const violations: Violation[] = [];
let ticksChecked = 0;

async function loadDevice() {
    const bytes = readFileSync(WASM_PATH);
    const mod = await WebAssembly.compile(bytes);

    let memory!: WebAssembly.Memory;
    const decoder = new TextDecoder();
    const fwLogLines: string[] = [];

    const imports = {
        env: {
            js_log(ptr: number, len: number) {
                const b = new Uint8Array(memory.buffer, ptr, len);
                fwLogLines.push(decoder.decode(b));
            },
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

    if (exp.emu_init() !== 1) {
        throw new Error("emu_init() failed - see [fw] log lines above");
    }

    function fbSnapshot(): Uint8Array {
        const ptr = exp.emu_fb();
        return new Uint8Array(memory.buffer, ptr, PANEL_W * PANEL_H * 2).slice();
    }

    function pushRects(): { x: number; y: number; w: number; h: number }[] {
        const n = exp.emu_push_count();
        const out: { x: number; y: number; w: number; h: number }[] = [];
        for (let i = 0; i < n; i++) {
            out.push({ x: exp.emu_push_x(i), y: exp.emu_push_y(i), w: exp.emu_push_w(i), h: exp.emu_push_h(i) });
        }
        return out;
    }

    return {
        appSwitch(index: number) { exp.emu_app_switch(index); },
        appCurrent(): number { return exp.emu_app_current(); },

        // Checked tick: snapshots before, ticks, then diffs the framebuffer
        // against the union of this tick's own pushed rectangles and checks
        // every rectangle's row length - identical technique to
        // repro-timer-coil.ts's tickChecked(). Called on EVERY tick this
        // file drives, including every single one of the palette's own
        // animation frames (see holdStill()'s own comment) - the whole
        // point being that the invariant is proven across the animation,
        // not only on its final, settled frame.
        tickChecked(nowMs: number) {
            ticksChecked++;
            const before = fbSnapshot();
            exp.emu_tick(nowMs);
            const after = fbSnapshot();
            const rects = pushRects();

            for (const r of rects) {
                if (r.w % 8 !== 0) {
                    violations.push({ kind: "8px-rule", detail: `t=${nowMs} pushed rect (${r.x},${r.y},${r.w},${r.h}) has width ${r.w}, not a multiple of 8` });
                }
            }

            let anyDiff = false;
            for (let i = 0; i < before.length; i++) { if (before[i] !== after[i]) { anyDiff = true; break; } }
            if (!anyDiff) return;

            for (let py = 0; py < PANEL_H; py++) {
                for (let px = 0; px < PANEL_W; px++) {
                    const idx = (py * PANEL_W + px) * 2;
                    if (before[idx] === after[idx] && before[idx + 1] === after[idx + 1]) continue;
                    const inside = rects.some((r) => px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h);
                    if (!inside) {
                        violations.push({ kind: "outside-push", detail: `t=${nowMs} pixel (${px},${py}) changed but is outside every pushed rect (${JSON.stringify(rects)})` });
                    }
                }
            }
        },

        touchChecked(down: boolean, x: number, y: number, nowMs: number) {
            exp.emu_touch(down ? 1 : 0, x, y);
            this.tickChecked(nowMs);
        },

        fbSnapshot,
        fbHash(): number | bigint { return Bun.hash(fbSnapshot()); },
        fwLogLines(): readonly string[] { return fwLogLines; },
        drainLog(): string[] {
            const n = fwLogLines.length;
            const out = fwLogLines.slice();
            fwLogLines.length = 0;
            return out.slice(0, n);
        },
    };
}

type Device = Awaited<ReturnType<typeof loadDevice>>;

// Holds a touch still at (x,y), pushing a fresh sample every ~15ms so
// sketch.c's wall-clock checks (CONFIRM_MS's persisted branch, the
// hold-candidacy long-press check, AND the palette's own pop-in animation
// once it opens) actually get re-evaluated tick by tick - a single
// touch() call followed by one big tick() would NOT do this, and would
// also only ever exercise the animation's FINAL frame rather than every
// frame in between, which is exactly what this file has to prove pushes
// cleanly throughout (see tickChecked's own comment).
function holdStill(dev: Device, x: number, y: number, t0: number, durationMs: number, stepMs = 15): number {
    let t = t0;
    for (let held = 0; held < durationMs; held += stepMs) {
        t += stepMs;
        dev.touchChecked(true, x, y, t);
    }
    return t;
}

// The mirror image: no contact, pushed every step, for the same reason -
// used both to let an ordinary stroke's LIFT_DEBOUNCE_MS lift resolve, and
// to let the palette's own PALETTE_LIFT_GRACE_MS close resolve.
function releaseAndWait(dev: Device, t0: number, durationMs: number, stepMs = 15): number {
    let t = t0;
    for (let waited = 0; waited < durationMs; waited += stepMs) {
        t += stepMs;
        dev.touchChecked(false, 0, 0, t);
    }
    return t;
}

// A clean drag from (x0,y0) to (x1,y1) over `steps` samples, then a lift
// held past LIFT_DEBOUNCE_MS so the stroke actually ends (not just pauses
// mid-dropout-grace) - ordinary stroke drawing, the same shape every other
// repro test's own drag helper uses.
function drawStroke(dev: Device, x0: number, y0: number, x1: number, y1: number, t0: number, steps = 12, stepMs = 15): number {
    let t = t0;
    for (let i = 0; i <= steps; i++) {
        const x = Math.round(x0 + (x1 - x0) * (i / steps));
        const y = Math.round(y0 + (y1 - y0) * (i / steps));
        t += stepMs;
        dev.touchChecked(true, x, y, t);
    }
    t = releaseAndWait(dev, t, LIFT_DEBOUNCE_MS + 100, stepMs);
    return t;
}

function decodeRgb565Be(hi: number, lo: number): [number, number, number] {
    const v = (hi << 8) | lo;
    const r5 = (v >> 11) & 0x1f, g6 = (v >> 5) & 0x3f, b5 = v & 0x1f;
    const r = Math.round((r5 * 255) / 31), g = Math.round((g6 * 255) / 63), b = Math.round((b5 * 255) / 31);
    return [r, g, b];
}

function pixelIsWhite(fb: Uint8Array, x: number, y: number): boolean {
    const idx = (y * PANEL_W + x) * 2;
    return fb[idx] === 0xff && fb[idx + 1] === 0xff;
}

// Mirror of sketch.c's rounded_rect_sdf(): negative inside the shape,
// positive outside. Used here (unlike the firmware, which only needs it
// for drawing and hit-testing) to independently verify EVERY pixel of a
// settled frame is accounted for - either white, or inside some cell's own
// shape - which is what a residue pixel (something painted and never
// cleared, sitting in the gutter) fails.
function roundedRectSdf(px: number, py: number, cx: number, cy: number, halfW: number, halfH: number, cornerR: number): number {
    const qx = Math.abs(px - cx) - halfW + cornerR;
    const qy = Math.abs(py - cy) - halfH + cornerR;
    const ax = Math.max(qx, 0), ay = Math.max(qy, 0);
    const outside = Math.sqrt(ax * ax + ay * ay);
    const inside = Math.min(Math.max(qx, qy), 0);
    return inside + outside - cornerR;
}

// ---------------------------------------------------------------------
// Minimal RGB (colour type 2) PNG encoder - same chunk/CRC/zlib-wrap
// machinery as tools/dev.ts's encodeGreyPNG (that file's own header
// explains the zlib-wrap-around-Bun.deflateSync trick), reimplemented here
// standalone rather than imported: this test's colours are the entire
// point of the feature it verifies, and tools/dev.ts's encoder is
// deliberately greyscale-only.
// ---------------------------------------------------------------------
function crc32Table(): Uint32Array {
    const table = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
        let c = n;
        for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
        table[n] = c >>> 0;
    }
    return table;
}
const CRC_TABLE = crc32Table();
function crc32(buf: Uint8Array): number {
    let crc = 0xffffffff;
    for (let i = 0; i < buf.length; i++) crc = CRC_TABLE[(crc ^ buf[i]) & 0xff]! ^ (crc >>> 8);
    return (crc ^ 0xffffffff) >>> 0;
}
function adler32(data: Uint8Array): number {
    let a = 1, b = 0;
    const MOD = 65521;
    for (let i = 0; i < data.length; i++) { a = (a + data[i]!) % MOD; b = (b + a) % MOD; }
    return ((b << 16) | a) >>> 0;
}
function zlibWrap(rawDeflate: Uint8Array, source: Uint8Array): Uint8Array {
    const out = new Uint8Array(2 + rawDeflate.length + 4);
    out[0] = 0x78; out[1] = 0x9c;
    out.set(rawDeflate, 2);
    new DataView(out.buffer).setUint32(2 + rawDeflate.length, adler32(source), false);
    return out;
}
function pngChunk(type: string, data: Uint8Array): Uint8Array {
    const typeBytes = new TextEncoder().encode(type);
    const body = new Uint8Array(typeBytes.length + data.length);
    body.set(typeBytes, 0); body.set(data, typeBytes.length);
    const out = new Uint8Array(4 + body.length + 4);
    const dv = new DataView(out.buffer);
    dv.setUint32(0, data.length, false);
    out.set(body, 4);
    dv.setUint32(4 + body.length, crc32(body), false);
    return out;
}
function concatBytes(chunks: Uint8Array[]): Uint8Array {
    const total = chunks.reduce((s, c) => s + c.length, 0);
    const out = new Uint8Array(total);
    let off = 0;
    for (const c of chunks) { out.set(c, off); off += c.length; }
    return out;
}
function encodeRgbPNG(fb: Uint8Array, w: number, h: number): Uint8Array {
    const raw = new Uint8Array((w * 3 + 1) * h);
    for (let y = 0; y < h; y++) {
        const rowOff = y * (w * 3 + 1);
        raw[rowOff] = 0; // filter type: None
        for (let x = 0; x < w; x++) {
            const idx = (y * w + x) * 2;
            const [r, g, b] = decodeRgb565Be(fb[idx]!, fb[idx + 1]!);
            const o = rowOff + 1 + x * 3;
            raw[o] = r; raw[o + 1] = g; raw[o + 2] = b;
        }
    }
    const idatData = zlibWrap(Bun.deflateSync(raw), raw);
    const sig = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    const ihdr = new Uint8Array(13);
    const dv = new DataView(ihdr.buffer);
    dv.setUint32(0, w, false); dv.setUint32(4, h, false);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0; // 8-bit RGB truecolor
    return concatBytes([sig, pngChunk("IHDR", ihdr), pngChunk("IDAT", idatData), pngChunk("IEND", new Uint8Array(0))]);
}
async function writeScreenshot(name: string, fb: Uint8Array) {
    const png = encodeRgbPNG(fb, PANEL_W, PANEL_H);
    const path = join(PREVIEW_DIR, name);
    await Bun.write(path, png);
    console.log(`    wrote ${path} (${png.length} bytes)`);
    return path;
}

async function main() {
    console.log("=== feature: sketchpad colour palette (9-cell, full-panel, pop-in) ===\n");
    const dev = await loadDevice();
    dev.tickChecked(0);
    dev.appSwitch(APP_DRAW);
    dev.tickChecked(1000);
    check("switched into the sketchpad", dev.appCurrent() === APP_DRAW, `app_current()=${dev.appCurrent()}`);

    // ---- arena arithmetic: measured, not assumed - see sketch_enter()'s
    // own diagnostic print and sketch.c's "Stroke history" comment section
    // for the reasoning behind STROKE_POOL_POINTS/STROKES. -----------------
    const sizeofLine = dev.fwLogLines().find((l) => l.includes("sketch: state="));
    const sizeofMatch = sizeofLine?.match(/state=(\d+) bytes \(arena (\d+)\)/);
    const measuredBytes = sizeofMatch ? Number(sizeofMatch[1]) : -1;
    check(
        "sketch_state_t's measured size fits the arena, with the fit reported (not assumed)",
        sizeofMatch !== undefined && measuredBytes > 0 && measuredBytes <= APP_ARENA_BYTES,
        sizeofLine ?? "(no sizeof line seen in the firmware log)",
    );
    if (measuredBytes > 0) {
        const pct = ((measuredBytes / APP_ARENA_BYTES) * 100).toFixed(1);
        console.log(`    sketch_state_t = ${measuredBytes} bytes of ${APP_ARENA_BYTES} (${pct}%)`);
    }

    // ---- an existing drawing, in the device's own default black ink,
    // BEFORE touching the palette at all. -----------------------------------
    console.log("\n-- drawing an existing stroke in black, before touching the palette at all --");
    let t = drawStroke(dev, 40, 40, 280, 120, 1000);
    check("the existing stroke actually drew something", (() => {
        const fb = dev.fbSnapshot();
        for (let py = 40; py < 120; py++) for (let px = 40; px < 280; px++) {
            const idx = (py * PANEL_W + px) * 2;
            if (fb[idx] !== 0xff || fb[idx + 1] !== 0xff) return true;
        }
        return false;
    })());

    // ---- scenario 1: hold still on the centre (black) cell, past
    // CONFIRM_MS+LONG_PRESS_MS. The palette opens, wipes the whole panel
    // (it is a full-screen menu now - the owner's own "c'est pas tres
    // grave"), and pops all nine cells in; the hold is kept for well past
    // the animation's own total duration so every one of its frames gets
    // driven through tickChecked(). ------------------------------------
    console.log("\n-- long press on the centre cell, held through the whole pop-in animation --");
    const centre = paletteCellBounds(INDEX_CENTER_BLACK);
    const ticksBefore = ticksChecked;
    t = holdStill(dev, centre.cx, centre.cy, t + 300, CONFIRM_MS + LONG_PRESS_MS + PALETTE_ANIM_TOTAL_MS + 150);
    console.log(`    ${ticksChecked - ticksBefore} ticks driven and invariant-checked across the long press + full pop-in animation`);

    const openLine = dev.fwLogLines().findLast((l) => l.includes("palette: open at"));
    check("the firmware logged the palette opening with the centre cell as the initial candidate",
        !!openLine && openLine.includes(`candidate=${INDEX_CENTER_BLACK}`), openLine ?? "(no palette open log line seen)");

    const fbSettled = dev.fbSnapshot();

    // All nine cells show their own flat colour at their own centre.
    let allCentersMatch = true;
    const mismatches: string[] = [];
    for (let i = 0; i < PALETTE_COUNT; i++) {
        const { cx, cy } = paletteCellBounds(i);
        const idx = (cy * PANEL_W + cx) * 2;
        const [wantHi, wantLo] = PALETTE_COLORS_BE[i]!;
        if (fbSettled[idx] !== wantHi || fbSettled[idx + 1] !== wantLo) {
            allCentersMatch = false;
            mismatches.push(`${COLOR_NAMES[i]} (index ${i}) centre (${cx},${cy}): got [${fbSettled[idx]!.toString(16)},${fbSettled[idx + 1]!.toString(16)}]`);
        }
    }
    check("the palette popped in with all nine expected flat colours at each cell's centre", allCentersMatch, mismatches.join("; "));

    // Roundedness: each cell's own bounding-box corner is white (a plain
    // axis-aligned square fill would have coloured it) - decision 0009's
    // "no palette squares" proven per cell, not asserted once.
    let allCornersWhite = true;
    const cornerMismatches: string[] = [];
    for (let i = 0; i < PALETTE_COUNT; i++) {
        const { cx, cy, halfW, halfH } = paletteCellBounds(i);
        const cornerX = cx - halfW, cornerY = cy - halfH;
        if (!pixelIsWhite(fbSettled, cornerX, cornerY)) {
            allCornersWhite = false;
            cornerMismatches.push(`${COLOR_NAMES[i]} (index ${i}) bounding-box corner (${cornerX},${cornerY}) is not white`);
        }
    }
    check("every cell's own bounding-box corner is white - rounded rectangles, not squares (decision 0009)",
        allCornersWhite, cornerMismatches.join("; "));

    // Direct residue detector, over the WHOLE settled panel, not just nine
    // sample points: every pixel must be either white or inside SOME
    // cell's own rounded-rect shape (the candidate's own grown one
    // included). This is exactly the check that would have caught the
    // owner's "y a des espèces de traits qui traînent" report - thin
    // painted streaks sitting in the paper-coloured gutters between rows
    // and columns, left behind by an early animation frame's overshoot
    // that a later frame's too-small whiten box never reached back out to
    // reclaim (see sketch.c's palette_render_frame() header comment for
    // the full account) - which the per-cell corner/candidate probes above
    // are far too sparse to notice on their own.
    const cellShapes = Array.from({ length: PALETTE_COUNT }, (_, i) => {
        const b = paletteCellBounds(i);
        const grown = i === INDEX_CENTER_BLACK; // the settled candidate in this scenario
        return { ...b, halfW: b.halfW + (grown ? PALETTE_CANDIDATE_GROW_PX : 0), halfH: b.halfH + (grown ? PALETTE_CANDIDATE_GROW_PX : 0) };
    });
    let residuePixels = 0;
    const residueSamples: string[] = [];
    const AA_SLOP_PX = 1.5; // draw_rounded_rect's own +-1px AA feather margin
    for (let py = 0; py < PANEL_H; py++) {
        for (let px = 0; px < PANEL_W; px++) {
            if (pixelIsWhite(fbSettled, px, py)) continue;
            const insideSome = cellShapes.some((c) => roundedRectSdf(px + 0.5, py + 0.5, c.cx, c.cy, c.halfW, c.halfH, PALETTE_CORNER_PX) <= AA_SLOP_PX);
            if (!insideSome) {
                residuePixels++;
                if (residueSamples.length < 6) residueSamples.push(`(${px},${py})`);
            }
        }
    }
    check("no residue: every non-white pixel of the settled panel sits inside some cell's own shape (none left in the gutters)",
        residuePixels === 0, residuePixels > 0 ? `${residuePixels} stray pixel(s), e.g. ${residueSamples.join(", ")}` : "0 stray pixels");

    // The candidate (centre, still under the finger) is visibly grown past
    // a non-candidate cell at the identical relative offset - see this
    // file's own header comment for the SDF arithmetic behind these exact
    // offsets (base halfW + PALETTE_CANDIDATE_GROW_PX/2 rounds to "just
    // past the base edge, just short of the grown one").
    const growOffset = Math.floor(centre.halfW + PALETTE_CANDIDATE_GROW_PX / 2);
    const candidateProbeX = centre.cx + growOffset, candidateProbeY = centre.cy;
    const nonCandidate = paletteCellBounds(INDEX_RED);
    const otherProbeX = nonCandidate.cx + Math.floor(nonCandidate.halfW + PALETTE_CANDIDATE_GROW_PX / 2), otherProbeY = nonCandidate.cy;
    const candidateGrown = !pixelIsWhite(fbSettled, candidateProbeX, candidateProbeY);
    const otherStillBase = pixelIsWhite(fbSettled, otherProbeX, otherProbeY);
    check("the candidate cell (still under the finger) is visibly larger than a non-candidate cell at the same relative offset",
        candidateGrown && otherStillBase,
        `candidate probe (${candidateProbeX},${candidateProbeY}) ink=${candidateGrown}, other probe (${otherProbeX},${otherProbeY}) white=${otherStillBase}`);

    await writeScreenshot("palette-open.png", fbSettled);
    console.log("    screenshot: the palette open, popped in, centre cell visibly the candidate");

    // ---- scenario 2: drag the still-down touch onto the red cell and
    // release there - picks red without ever lifting. ---------------------
    console.log("\n-- dragging onto the red cell and releasing over it --");
    const red = paletteCellBounds(INDEX_RED);
    dev.touchChecked(true, red.cx, red.cy, (t += 15));
    dev.touchChecked(true, red.cx, red.cy, (t += 15));
    t = releaseAndWait(dev, t, PALETTE_LIFT_GRACE_MS + 100);

    const pickedLine = dev.fwLogLines().findLast((l) => l.includes("palette: picked"));
    check("the firmware logged a colour pick, not a cancel", !!pickedLine, pickedLine ?? "(no palette log line seen)");

    const fbAfterPick = dev.fbSnapshot();
    // The original black stroke, drawn before the palette ever opened,
    // reappears once the canvas is repainted from stroke history - proof
    // the close-time repaint is not just "wipe and move on".
    let originalStrokeRestored = false;
    for (let py = 40; py < 120 && !originalStrokeRestored; py++) {
        for (let px = 40; px < 280; px++) {
            const idx = (py * PANEL_W + px) * 2;
            if (fbAfterPick[idx] !== 0xff || fbAfterPick[idx + 1] !== 0xff) { originalStrokeRestored = true; break; }
        }
    }
    check("the drawing from before the palette opened reappears, repainted from stroke history", originalStrokeRestored);

    t = releaseAndWait(dev, t, 100);

    console.log("-- drawing a new stroke, away from everything already on screen --");
    t = drawStroke(dev, 200, 360, 340, 420, t);

    const fbColourStroke = dev.fbSnapshot();
    let sawRedInk = false;
    for (let py = 360; py < 425 && !sawRedInk; py++) {
        for (let px = 195; px < 345; px++) {
            const idx = (py * PANEL_W + px) * 2;
            const hi = fbColourStroke[idx]!, lo = fbColourStroke[idx + 1]!;
            // Red ink: high R byte, near-zero G/B - not exactly 0xF8/0x00
            // everywhere, since anti-aliased edges blend toward white (see
            // tint_to_px's own comment), but the stroke's CORE pixels land
            // on or very near the pure red bytes.
            if (hi >= 0xe0 && lo <= 0x20) { sawRedInk = true; break; }
        }
    }
    check("the new stroke actually drew in the picked colour (red), not black", sawRedInk);

    await writeScreenshot("palette-stroke-colour.png", fbColourStroke);
    console.log("    screenshot: a stroke drawn in the chosen colour, over the drawing restored from history");

    // ---- scenario 3: open the palette again, hold in the GAP between two
    // cells (never over any colour), then lift WITHOUT ever moving - "what
    // happens if she never moves at all" landing exactly on "over nothing".
    // Must restore the framebuffer to BYTE-IDENTICAL what it was
    // immediately before this second palette opened. ------------------------
    console.log("\n-- long press in the gap between two cells, released without ever moving (cancel) --");
    const preSecondOpen = dev.fbSnapshot();

    // The raw column-0/column-1 boundary (the same "i*N/D" partition line
    // palette_cell_bounds() itself divides on) sits in the horizontal gap
    // between them by construction, whatever the grid's own inset or gap
    // width are tuned to - computed here rather than hardcoded so this
    // stays correct if either ever changes. y is the row's own vertical
    // centre, well clear of any row gap - see this file's header comment
    // on the SDF check proving both neighbouring cells read this point as
    // outside their own shape.
    const gapX = PALETTE_GRID_INSET_PX + Math.floor((PALETTE_GRID_W * 1) / PALETTE_COLS);
    const gapY = paletteCellBounds(INDEX_CENTER_BLACK).cy;
    t = holdStill(dev, gapX, gapY, t + 300, CONFIRM_MS + LONG_PRESS_MS + 250);

    const openLine2 = dev.fwLogLines().findLast((l) => l.includes("palette: open at"));
    check("the palette opened a second time, with no initial candidate (held in the gap)",
        !!openLine2 && openLine2.includes("candidate=-1"), openLine2 ?? "(no palette open log line seen)");

    // Never moved: one more sample at the exact same spot, then lift.
    dev.touchChecked(true, gapX, gapY, (t += 15));
    t = releaseAndWait(dev, t, PALETTE_LIFT_GRACE_MS + 100);

    const cancelLine = dev.fwLogLines().findLast((l) => l.includes("palette: cancelled"));
    check("the firmware logged a cancel, not a pick", !!cancelLine, cancelLine ?? "(no palette log line seen)");

    const afterCancel = dev.fbSnapshot();
    let identical = preSecondOpen.length === afterCancel.length;
    let firstDiffAt = -1;
    if (identical) {
        for (let i = 0; i < preSecondOpen.length; i++) {
            if (preSecondOpen[i] !== afterCancel[i]) { identical = false; firstDiffAt = i; break; }
        }
    }
    check(
        "dismissing the palette restores EXACTLY the framebuffer from before it opened (byte-identical, replayed from stroke history)",
        identical,
        identical ? "0 byte(s) differ" : `first differing byte offset=${firstDiffAt} (pixel (${Math.floor(firstDiffAt / 2 / PANEL_W)},${Math.floor((firstDiffAt / 2) % PANEL_W)}))`,
    );

    await writeScreenshot("palette-dismissed.png", afterCancel);
    console.log("    screenshot: the drawing after the palette has been dismissed - nothing left behind");

    // Sanity: the inking colour is still red after the cancel (a cancel
    // must not silently reset the tool the way a fresh app-enter would).
    console.log("\n-- confirming a cancel leaves the previously picked colour in effect --");
    t = drawStroke(dev, 20, 200, 120, 250, t);
    const fbAfterCancelStroke = dev.fbSnapshot();
    let sawRedAfterCancel = false;
    for (let py = 195; py < 255 && !sawRedAfterCancel; py++) {
        for (let px = 15; px < 125; px++) {
            const idx = (py * PANEL_W + px) * 2;
            if (fbAfterCancelStroke[idx]! >= 0xe0 && fbAfterCancelStroke[idx + 1]! <= 0x20) { sawRedAfterCancel = true; break; }
        }
    }
    check("the pen is still inking red after a cancelled palette (cancel does not reset the tool)", sawRedAfterCancel);

    // ---- scenario 4: dismiss sampled at several points ACROSS the pop-in
    // animation, not only once it has fully settled. Scenario 3 above only
    // ever cancelled after holding well past the whole animation
    // (CONFIRM_MS + LONG_PRESS_MS + 250, comfortably past PALETTE_ANIM_
    // TOTAL_MS's own 280) - every earlier byte-identical check in this file
    // shared that same blind spot, which is exactly why the residue this
    // scenario is named for (an animation frame painting into the gutter,
    // a later frame's too-small clear box never reaching back out to it)
    // went unnoticed by "the dismiss is byte-identical" alone: the
    // fully-settled frame this file always cancelled from happened to be
    // the LAST frame rendered, which is not where an animation-only defect
    // shows. Each hold length below lands the cancel at a different point
    // in the pop-in (freshly opened, early in the overshoot, past the
    // overshoot's own peak, and fully settled) - every one of them still
    // has to restore the canvas byte for byte. ------------------------------
    console.log("\n-- dismissing at several points across the pop-in animation, each must still be byte-identical --");
    const midAnimHoldsMs = [0, 60, 140, 220, PALETTE_ANIM_TOTAL_MS + 60];
    for (const extraHoldMs of midAnimHoldsMs) {
        const before = dev.fbSnapshot();
        t = holdStill(dev, gapX, gapY, t + 300, CONFIRM_MS + LONG_PRESS_MS + extraHoldMs);
        dev.touchChecked(true, gapX, gapY, (t += 15)); // never moved
        t = releaseAndWait(dev, t, PALETTE_LIFT_GRACE_MS + 100);

        const after = dev.fbSnapshot();
        let sameHere = before.length === after.length;
        let firstDiffHere = -1;
        if (sameHere) {
            for (let i = 0; i < before.length; i++) {
                if (before[i] !== after[i]) { sameHere = false; firstDiffHere = i; break; }
            }
        }
        check(
            `cancel after +${extraHoldMs}ms into the pop-in restores byte-identical`,
            sameHere,
            sameHere ? "0 byte(s) differ" : `first differing byte offset=${firstDiffHere} (pixel (${Math.floor((firstDiffHere / 2) % PANEL_W)},${Math.floor(firstDiffHere / 2 / PANEL_W)}))`,
        );
    }

    console.log("\n=== invariants across this whole run (EVERY tick, including every animation frame): 8px rule + no pixels outside pushed rects ===");
    console.log(`    ${ticksChecked} ticks checked in total`);
    const byKind = new Map<string, number>();
    for (const v of violations) byKind.set(v.kind, (byKind.get(v.kind) ?? 0) + 1);
    check(
        "every pushed window's row length was a multiple of 8 (decision 0001)",
        (byKind.get("8px-rule") ?? 0) === 0,
        `${byKind.get("8px-rule") ?? 0} violation(s)`,
    );
    check(
        "no framebuffer pixel ever changed outside that tick's own pushed rectangles (the palette's animation included)",
        (byKind.get("outside-push") ?? 0) === 0,
        `${byKind.get("outside-push") ?? 0} violation(s)`,
    );
    if (violations.length > 0) {
        console.log("first few violations:");
        for (const v of violations.slice(0, 10)) console.log(`    [${v.kind}] ${v.detail}`);
    }

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
