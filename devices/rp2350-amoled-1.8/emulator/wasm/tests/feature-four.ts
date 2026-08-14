// feature-four: headless verification of the Connect Four app
// (firmware/apps/four.c), against the REAL firmware compiled to wasm
// (decision 0003) - nothing here reimplements the app's logic. Every
// assertion reads the framebuffer, the firmware's own log lines, or the
// push-window list the firmware itself produced.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/feature-four.ts
//
// WHAT THIS FILE PROVES, IN ORDER:
//   1. the board renders as a grey slab with white holes and a red waiting
//      piece, and the app's arena footprint is REPORTED rather than assumed;
//   2. a thumb on the glass highlights its column unmistakably - the chute
//      washes pale red the whole height of the screen, and the lowest empty
//      hole of that column wears a landing ring - and moving the thumb moves
//      both, leaving the column it left back at its resting grey;
//   3. releasing drops the piece, it falls THROUGH the column (caught
//      mid-air, not just at rest) and lands in the column the thumb was over;
//   4. the device answers on its own, in blue, having visibly aimed first;
//   5. a game is won, the four in a row breathe, a touch moves it along, the
//      board drains and a fresh game begins - with no text anywhere in any of
//      it;
//   6. EVERY tick of all of the above - the fall, the celebration's pulse and
//      the drain included, not only the frames at rest - obeys decision
//      0001's 8px rule and the "no pixel changes outside the pushed
//      rectangle" invariant.
//
// The gesture's behaviour under a REALISTIC touch stream (contact dropping
// out ~34 times a second) is a separate file, deliberately:
// repro-touch-dropout-four-drop.ts. This one drives clean input, which is
// what makes it a readable statement of what the app is supposed to do.
//
// Screenshots go to preview/four-*.png, written LANDSCAPE (448x368, the way
// the device is held for this app) rather than in the panel's own portrait
// orientation: the owner judges these by eye, and a board rotated 90 degrees
// is not the thing being judged. The rotation is applied at encode time
// only; every assertion above reads the framebuffer in its real layout.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const ROOT = join(import.meta.dir, "..", "..", "..");
const PREVIEW_DIR = join(ROOT, "preview");

const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const LAND_H = PANEL_W; // 368
const APP_FOUR = 3; // g_apps[] = { chrono, sketch("draw"), timer, four }
const APP_ARENA_BYTES = 65536; // app.h APP_ARENA_BYTES

// ---- mirrors of four.c's own constants. Lifted, not re-derived; the same
// convention every other test in this directory uses for the app it drives.
const COLS = 7;
const ROWS = 6;
const CELL = 50;
const BOARD_X0 = 49;
const BOARD_Y0 = 56;
const HOLE_R = 21;
const RELEASE_GRACE_MS = 260;
const THINK_MS = 620;
const CELEBRATE_SKIP_MS = 1200;

const colX = (c: number) => BOARD_X0 + CELL / 2 + c * CELL;
const rowY = (r: number) => BOARD_Y0 + CELL / 2 + r * CELL;

// The colours four.c paints, as the LOGICAL rgb565 byte pair the
// framebuffer holds (the fb stores px_swap(v) as a uint16, which on this
// little-endian target puts the logical high byte first - see
// feature-sketch-palette.ts's PALETTE_COLORS_BE for the same convention).
const C_WHITE: [number, number] = [0xff, 0xff];
const C_SLAB: [number, number] = [0xde, 0xfb];
const C_RED: [number, number] = [0xf8, 0x00];
const C_BLUE: [number, number] = [0x00, 0x1f];
const C_WASH_RED: [number, number] = [0xfd, 0x55];
const C_WASH_BLUE: [number, number] = [0xad, 0xff];

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

// ---------------------------------------------------------------------
// Invariant tracking, same shape as feature-sketch-palette.ts's (see that
// file's header for why this lives inline in every test in this directory
// rather than in a shared module: each one is self-contained).
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
    if (exp.emu_init() !== 1) throw new Error("emu_init() failed - see [fw] log lines above");

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

        // Snapshots before, ticks, then diffs the framebuffer against the
        // union of this tick's own pushed rectangles and checks every
        // rectangle's row length. Every tick this file drives goes through
        // here, animation frames included: the invariant has to hold ACROSS
        // the fall and the celebration, not only once they are over.
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

            // Diffed 32 bits (two pixels) at a time, and only the words that
            // actually differ are then resolved to coordinates. The plain
            // per-pixel double loop this replaces walked all 165k pixels on
            // every tick that changed anything, which is most of them here:
            // this app animates a falling piece, a breathing win and a
            // draining board, so "nothing changed" is the exception rather
            // than the rule, unlike the apps the earlier tests in this
            // directory drive. Same assertions, same coverage, about fifteen
            // times faster - which is the difference between this file
            // finishing and this file timing out.
            const b32 = new Uint32Array(before.buffer, before.byteOffset, before.byteLength >> 2);
            const a32 = new Uint32Array(after.buffer, after.byteOffset, after.byteLength >> 2);
            for (let w = 0; w < b32.length; w++) {
                if (b32[w] === a32[w]) continue;
                for (let half = 0; half < 2; half++) {
                    const idx = w * 4 + half * 2;
                    if (before[idx] === after[idx] && before[idx + 1] === after[idx + 1]) continue;
                    const pixel = idx >> 1;
                    const py = (pixel / PANEL_W) | 0;
                    const px = pixel - py * PANEL_W;
                    const inside = rects.some((r) => px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h);
                    if (!inside) {
                        violations.push({ kind: "outside-push", detail: `t=${nowMs} pixel (${px},${py}) changed but is outside every pushed rect (${JSON.stringify(rects)})` });
                    }
                }
            }
        },

        // Touch, given in LANDSCAPE coordinates and converted here, because
        // that is the space this app thinks in. gfx.h's mapping is
        // (lx, ly) -> panel (PANEL_W-1-ly, lx), and emu_touch takes panel
        // coordinates (emu_abi.h: "always in the panel's own, unrotated
        // space... mapping the pointer back is the emulator's job, because
        // the firmware's own coordinate handling is under test").
        touchLand(down: boolean, lx: number, ly: number, nowMs: number) {
            exp.emu_touch(down ? 1 : 0, PANEL_W - 1 - Math.round(ly), Math.round(lx));
            this.tickChecked(nowMs);
        },

        fbSnapshot,
        fwLogLines(): readonly string[] { return fwLogLines; },
        drainLog(): string[] { const out = fwLogLines.slice(); fwLogLines.length = 0; return out; },
    };
}

type Device = Awaited<ReturnType<typeof loadDevice>>;

// The same module, without the per-tick framebuffer diff. For the long
// endurance run only - see that section for why paying for the invariant
// check twice buys nothing.
async function loadRawDevice() {
    const mod = await WebAssembly.compile(readFileSync(WASM_PATH));
    let memory!: WebAssembly.Memory;
    const decoder = new TextDecoder();
    const fwLog: string[] = [];
    const instance = await WebAssembly.instantiate(mod, {
        env: {
            js_log(ptr: number, len: number) { fwLog.push(decoder.decode(new Uint8Array(memory.buffer, ptr, len))); },
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
    });
    memory = instance.exports.memory as WebAssembly.Memory;
    const exp = instance.exports as any;
    if (exp.emu_init() !== 1) throw new Error("emu_init() failed");
    exp.emu_tick(0);
    exp.emu_app_switch(APP_FOUR);
    exp.emu_tick(10);
    fwLog.length = 0;
    return {
        touchLand(down: boolean, lx: number, ly: number, nowMs: number) {
            exp.emu_touch(down ? 1 : 0, PANEL_W - 1 - Math.round(ly), Math.round(lx));
            exp.emu_tick(nowMs);
        },
        drainLog(): string[] { const out = fwLog.slice(); fwLog.length = 0; return out; },
    };
}

// ---- reading the framebuffer in landscape ---------------------------
function landPx(fb: Uint8Array, lx: number, ly: number): [number, number] {
    const px = PANEL_W - 1 - Math.round(ly);
    const py = Math.round(lx);
    const idx = (py * PANEL_W + px) * 2;
    return [fb[idx]!, fb[idx + 1]!];
}

function near(got: [number, number], want: [number, number], tol = 6): boolean {
    return Math.abs(got[0] - want[0]) <= tol && Math.abs(got[1] - want[1]) <= tol;
}

function describe(p: [number, number]): string {
    return `[0x${p[0].toString(16).padStart(2, "0")},0x${p[1].toString(16).padStart(2, "0")}]`;
}

// Is this pixel dominated by red (high R, low G/B) or by blue? Used where an
// exact match is wrong because the pixel may sit on an anti-aliased edge.
function isRedish(p: [number, number]): boolean { return p[0] >= 0xe0 && p[1] <= 0x20; }
function isBluish(p: [number, number]): boolean { return p[0] <= 0x20 && p[1] >= 0x10 && p[1] <= 0x3f; }

// ---- driving ---------------------------------------------------------
function settle(dev: Device, t0: number, durationMs: number, stepMs = 15): number {
    let t = t0;
    for (let waited = 0; waited < durationMs; waited += stepMs) {
        t += stepMs;
        dev.touchLand(false, 0, 0, t);
    }
    return t;
}

function holdLand(dev: Device, lx: number, ly: number, t0: number, durationMs: number, stepMs = 15): number {
    let t = t0;
    for (let held = 0; held < durationMs; held += stepMs) {
        t += stepMs;
        dev.touchLand(true, lx, ly, t);
    }
    return t;
}

// ---------------------------------------------------------------------
// A mirror of the board, maintained purely from the firmware's own "four:
// drop col=.. row=.. player=.." log lines - never from any state this test
// keeps in parallel with the app. It exists so the test can play WELL
// enough to reach a win in a handful of moves (the app's opponent blocks
// only about a third of the time, by design, so a competent partner beats
// it quickly) rather than dropping pieces at random and hoping.
// ---------------------------------------------------------------------
type Mirror = { cells: number[] };
function newMirror(): Mirror { return { cells: new Array(ROWS * COLS).fill(0) }; }
function mirrorApply(m: Mirror, col: number, row: number, player: number) { m.cells[row * COLS + col] = player; }
function mirrorLanding(m: Mirror, col: number): number {
    for (let r = ROWS - 1; r >= 0; r--) if (m.cells[r * COLS + col] === 0) return r;
    return -1;
}
function mirrorLine(m: Mirror, col: number, row: number, p: number): number {
    const DC = [1, 0, 1, 1], DR = [0, 1, 1, -1];
    let best = 1;
    for (let i = 0; i < 4; i++) {
        let n = 1;
        for (const sign of [1, -1]) {
            let c = col + DC[i]! * sign, r = row + DR[i]! * sign;
            while (c >= 0 && c < COLS && r >= 0 && r < ROWS && m.cells[r * COLS + c] === p) {
                n++; c += DC[i]! * sign; r += DR[i]! * sign;
            }
        }
        if (n > best) best = n;
    }
    return best;
}
// The stalemate-seeking partner: block the device's fours, build threes so it
// has to keep answering, and NEVER complete a four. See the endurance section
// for why a draw needs a player like this rather than a good one.
function chooseStalemateMove(m: Mirror): number {
    for (let c = 0; c < COLS; c++) { const r = mirrorLanding(m, c); if (r >= 0 && mirrorLine(m, c, r, 2) >= 4) return c; }
    let best = -1, bestScore = -99;
    for (let c = 0; c < COLS; c++) {
        const r = mirrorLanding(m, c);
        if (r < 0) continue;
        const own = mirrorLine(m, c, r, 1);
        if (own >= 4) continue;
        const score = own * 10 - Math.abs(c - 3);
        if (score > bestScore) { bestScore = score; best = c; }
    }
    // Every legal column would complete a four: it still has to move.
    if (best < 0) for (let c = 0; c < COLS; c++) if (mirrorLanding(m, c) >= 0) best = c;
    return best;
}

// Take a win, else block, else build the longest line available, preferring
// the middle. Ordinary competent play, nothing clever.
function chooseChildMove(m: Mirror): number {
    for (let c = 0; c < COLS; c++) { const r = mirrorLanding(m, c); if (r >= 0 && mirrorLine(m, c, r, 1) >= 4) return c; }
    for (let c = 0; c < COLS; c++) { const r = mirrorLanding(m, c); if (r >= 0 && mirrorLine(m, c, r, 2) >= 4) return c; }
    let best = -1, bestScore = -1;
    for (let c = 0; c < COLS; c++) {
        const r = mirrorLanding(m, c);
        if (r < 0) continue;
        const score = mirrorLine(m, c, r, 1) * 10 + (3 - Math.abs(c - 3));
        if (score > bestScore) { bestScore = score; best = c; }
    }
    return best;
}

// ---------------------------------------------------------------------
// PNG. Same chunk/CRC/zlib-wrap machinery as feature-sketch-palette.ts's
// encoder, with one difference: the rows written are the LANDSCAPE image,
// so the file opens the way the device is held for this app. See this
// file's header comment.
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
    for (let i = 0; i < buf.length; i++) crc = CRC_TABLE[(crc ^ buf[i]!) & 0xff]! ^ (crc >>> 8);
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
function decodeRgb565Be(hi: number, lo: number): [number, number, number] {
    const v = (hi << 8) | lo;
    const r5 = (v >> 11) & 0x1f, g6 = (v >> 5) & 0x3f, b5 = v & 0x1f;
    return [Math.round((r5 * 255) / 31), Math.round((g6 * 255) / 63), Math.round((b5 * 255) / 31)];
}
function encodeLandscapePNG(fb: Uint8Array): Uint8Array {
    const w = LAND_W, h = LAND_H;
    const raw = new Uint8Array((w * 3 + 1) * h);
    for (let ly = 0; ly < h; ly++) {
        const rowOff = ly * (w * 3 + 1);
        raw[rowOff] = 0; // filter type: None
        for (let lx = 0; lx < w; lx++) {
            const [hi, lo] = landPx(fb, lx, ly);
            const [r, g, b] = decodeRgb565Be(hi, lo);
            const o = rowOff + 1 + lx * 3;
            raw[o] = r; raw[o + 1] = g; raw[o + 2] = b;
        }
    }
    const idatData = zlibWrap(Bun.deflateSync(raw), raw);
    const sig = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    const ihdr = new Uint8Array(13);
    const dv = new DataView(ihdr.buffer);
    dv.setUint32(0, w, false); dv.setUint32(4, h, false);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    return concatBytes([sig, pngChunk("IHDR", ihdr), pngChunk("IDAT", idatData), pngChunk("IEND", new Uint8Array(0))]);
}
const shots: string[] = [];
async function writeScreenshot(name: string, fb: Uint8Array, what: string) {
    const png = encodeLandscapePNG(fb);
    const path = join(PREVIEW_DIR, name);
    await Bun.write(path, png);
    shots.push(path);
    console.log(`    wrote ${path} (${png.length} bytes) - ${what}`);
}

// ---------------------------------------------------------------------
async function main() {
    console.log("=== feature: Connect Four (firmware/apps/four.c) ===\n");
    const dev = await loadDevice();
    dev.tickChecked(0);
    dev.appSwitch(APP_FOUR);
    dev.tickChecked(1000);
    check("switched into the Connect Four app", dev.appCurrent() === APP_FOUR, `app_current()=${dev.appCurrent()}`);

    // ---- 1. the board, and the arena cost, reported not assumed ---------
    const sizeLine = dev.fwLogLines().find((l) => l.includes("four: state="));
    const sizeMatch = sizeLine?.match(/state=(\d+) bytes \(arena (\d+)\)/);
    const measuredBytes = sizeMatch ? Number(sizeMatch[1]) : -1;
    check("four_state_t's measured size fits the arena, with the fit reported (not assumed)",
        !!sizeMatch && measuredBytes > 0 && measuredBytes <= APP_ARENA_BYTES,
        sizeLine ?? "(no sizeof line seen in the firmware log)");
    if (measuredBytes > 0) {
        console.log(`    four_state_t = ${measuredBytes} bytes of ${APP_ARENA_BYTES} (${((measuredBytes / APP_ARENA_BYTES) * 100).toFixed(2)}%)`);
    }

    let t = 1000;
    t = settle(dev, t, 200);
    let fb = dev.fbSnapshot();

    // The slab is grey between two holes; every hole is paper white; there is
    // no line, no border and no right angle anywhere, so the ONLY thing that
    // makes a grid is the arrangement of the white circles.
    const gutter = landPx(fb, colX(3), (rowY(0) + rowY(1)) / 2);
    check("the board's face is a flat grey slab between its holes", near(gutter, C_SLAB), describe(gutter));
    let allHolesWhite = true;
    const holeMiss: string[] = [];
    for (let c = 0; c < COLS; c++) for (let r = 0; r < ROWS; r++) {
        const p = landPx(fb, colX(c), rowY(r));
        if (!near(p, C_WHITE)) { allHolesWhite = false; if (holeMiss.length < 4) holeMiss.push(`(${c},${r})=${describe(p)}`); }
    }
    check("all 42 holes are punched out in paper white", allHolesWhite, holeMiss.join(", "));

    // The slab's own corner: a lozenge with a 34px radius, so the rectangle's
    // corner pixel is still paper. Decision 0009, asserted rather than
    // asserted-in-prose.
    const slabCorner = landPx(fb, BOARD_X0 - 6 + 1, BOARD_Y0 - 6 + 1);
    check("the slab's bounding-box corner is still paper - a lozenge, not a rectangle (decision 0009)",
        near(slabCorner, C_WHITE), describe(slabCorner));

    // Whose turn it is, said only by colour: the waiting piece at the top is
    // red, which is hers.
    const waiting = landPx(fb, colX(3), 26);
    check("a red waiting piece sits at the top - the only thing that says whose turn it is",
        isRedish(waiting), describe(waiting));

    // ---- 2. the highlight ----------------------------------------------
    console.log("\n-- a thumb goes down on column 2 --");
    const THUMB_LY = 200; // mid-board, roughly where a thumb actually lands
    t = holdLand(dev, colX(2), THUMB_LY, t, 150);
    const hoverLine = dev.fwLogLines().findLast((l) => l.includes("four: hover"));
    check("the app reports the thumb's column", !!hoverLine && hoverLine.includes("c=2"), hoverLine ?? "(no hover line)");

    fb = dev.fbSnapshot();
    // The chute: pale red, the WHOLE height of the screen. Sampled at four
    // heights - above the board entirely, and in three different gutters
    // between holes - because the point of a full-height highlight is that
    // most of it is outside the ~75px her thumb is covering.
    const chuteProbes: { ly: number; where: string }[] = [
        { ly: 48, where: "above the board" },
        { ly: (rowY(0) + rowY(1)) / 2, where: "between rows 0 and 1" },
        { ly: (rowY(2) + rowY(3)) / 2, where: "between rows 2 and 3" },
        { ly: (rowY(4) + rowY(5)) / 2, where: "between rows 4 and 5" },
    ];
    let chuteOk = true;
    const chuteDetail: string[] = [];
    for (const probe of chuteProbes) {
        const p = landPx(fb, colX(2), probe.ly);
        if (!near(p, C_WASH_RED)) { chuteOk = false; }
        chuteDetail.push(`${probe.where}: ${describe(p)}`);
    }
    check("the highlighted column washes pale red over the WHOLE height of the screen", chuteOk, chuteDetail.join("; "));

    // ...and its neighbours do not.
    const neigh1 = landPx(fb, colX(1), (rowY(2) + rowY(3)) / 2);
    const neigh3 = landPx(fb, colX(3), (rowY(2) + rowY(3)) / 2);
    check("the columns either side of it are untouched grey slab",
        near(neigh1, C_SLAB) && near(neigh3, C_SLAB), `left ${describe(neigh1)}, right ${describe(neigh3)}`);

    // The landing ring: the outline of the piece that is about to be there,
    // in the lowest empty hole. Its ink is on the ring, its middle is paper.
    const ringInk = landPx(fb, colX(2) + HOLE_R - 3, rowY(5));
    const ringHole = landPx(fb, colX(2), rowY(5));
    check("the lowest empty hole of that column wears a red landing ring, hollow in the middle",
        isRedish(ringInk) && near(ringHole, C_WHITE), `ring ${describe(ringInk)}, middle ${describe(ringHole)}`);

    // The waiting piece has moved onto the column under the thumb.
    const waitOnCol2 = landPx(fb, colX(2), 26);
    const waitOffCol3 = landPx(fb, colX(3), 26);
    check("the waiting piece rides the highlighted column",
        isRedish(waitOnCol2) && near(waitOffCol3, C_WHITE), `on ${describe(waitOnCol2)}, off ${describe(waitOffCol3)}`);

    // (the screenshot of this is taken later, once the board has pieces on
    // it: an empty board cannot show the interesting half of the promise,
    // which is a landing ring sitting ON TOP of a stack rather than at the
    // bottom of an empty column.)

    // ---- the thumb slides ------------------------------------------------
    console.log("\n-- the thumb slides across to column 5 --");
    for (let c = 3; c <= 5; c++) t = holdLand(dev, colX(c), THUMB_LY, t, 45);
    fb = dev.fbSnapshot();
    const oldCol = landPx(fb, colX(2), (rowY(2) + rowY(3)) / 2);
    const newCol = landPx(fb, colX(5), (rowY(2) + rowY(3)) / 2);
    check("the highlight followed the thumb, and the column it left is back to plain slab",
        near(newCol, C_WASH_RED) && near(oldCol, C_SLAB), `new ${describe(newCol)}, old ${describe(oldCol)}`);

    // ---- 3. release drops the piece -------------------------------------
    console.log("\n-- the thumb lifts --");
    dev.drainLog();
    let dropLine: string | undefined;
    for (let waited = 0; waited < RELEASE_GRACE_MS + 200 && !dropLine; waited += 15) {
        t += 15;
        dev.touchLand(false, 0, 0, t);
        dropLine = dev.fwLogLines().findLast((l) => l.includes("four: drop"));
    }
    check("releasing drops a piece, in the column the thumb was over",
        !!dropLine && dropLine.includes("col=5") && dropLine.includes("player=1"), dropLine ?? "(no drop line)");

    // Catch it in the air. The piece has to be genuinely mid-column at some
    // point - a piece that teleports into place would pass every at-rest
    // assertion in this file and fail this one.
    let caught = false;
    for (let i = 0; i < 40 && !caught; i++) {
        t += 15;
        dev.touchLand(false, 0, 0, t);
        const now = dev.fbSnapshot();
        const mid = landPx(now, colX(5), rowY(2));
        const bottom = landPx(now, colX(5), rowY(5));
        if (isRedish(mid) && near(bottom, C_WHITE)) {
            caught = true;
            await writeScreenshot("four-falling.png", now,
                "a piece caught mid-fall: it is passing row 2 and the hole it is heading for is still empty");
        }
    }
    check("the piece is genuinely in flight partway down the column at some point", caught);

    // ---- it lands, and 4. the device answers ----------------------------
    t = settle(dev, t, 600);
    fb = dev.fbSnapshot();
    const landed = landPx(fb, colX(5), rowY(5));
    check("it comes to rest in the bottom hole of that column", isRedish(landed), describe(landed));

    const aiLine = dev.fwLogLines().findLast((l) => l.includes("four: ai col="));
    check("the device takes the other side and chooses a column of its own", !!aiLine, aiLine ?? "(no ai line)");
    const aiCol = aiLine ? Number(aiLine.match(/col=(\d+)/)?.[1] ?? -1) : -1;

    // It aims first, visibly, in its own colour: the same gesture she just
    // performed, played back at her. This is the only "whose turn" signal
    // there is and it has to be legible before its piece moves.
    fb = dev.fbSnapshot();
    const aiWash = landPx(fb, colX(aiCol), (rowY(0) + rowY(1)) / 2);
    const aiWaiting = landPx(fb, colX(aiCol), 26);
    check("while the device aims, ITS column washes pale blue and a blue piece waits above it",
        near(aiWash, C_WASH_BLUE) && isBluish(aiWaiting), `wash ${describe(aiWash)}, waiting ${describe(aiWaiting)}`);

    t = settle(dev, t, THINK_MS + 900);
    fb = dev.fbSnapshot();
    let sawBluePiece = false;
    for (let c = 0; c < COLS && !sawBluePiece; c++) for (let r = 0; r < ROWS; r++) {
        if (isBluish(landPx(fb, colX(c), rowY(r)))) { sawBluePiece = true; break; }
    }
    check("the device's piece is on the board, in blue", sawBluePiece);

    // ---- a real mid-game board, and the highlight ON it -----------------
    console.log("\n-- four more moves, then the same gesture over a column that already has a stack in it --");
    const mirror = newMirror();
    for (const line of dev.fwLogLines()) {
        const m = line.match(/four: drop col=(\d+) row=(\d+) player=(\d+)/);
        if (m) mirrorApply(mirror, Number(m[1]), Number(m[2]), Number(m[3]));
    }
    dev.drainLog();

    // Deliberately NOT chooseChildMove() here: that plays to win, and a win
    // mid-way would deal a fresh empty board out from under the screenshot
    // this block exists to take. Two pieces each into two separate columns
    // cannot make her four in a row, and it builds the stack the landing
    // ring needs to be resting on.
    const STACK_COL = 2;
    function playOneMove(col: number) {
        t = holdLand(dev, colX(col), THUMB_LY, t, 120);
        t = settle(dev, t, RELEASE_GRACE_MS + 2200);
        for (const line of dev.drainLog()) {
            const m = line.match(/four: drop col=(\d+) row=(\d+) player=(\d+)/);
            if (m) mirrorApply(mirror, Number(m[1]), Number(m[2]), Number(m[3]));
            if (line.includes("four: new game")) for (let k = 0; k < ROWS * COLS; k++) mirror.cells[k] = 0;
        }
    }
    for (const col of [STACK_COL, 4, STACK_COL, 4]) playOneMove(col);
    // If a game happened to end in there (the device does sometimes win),
    // the board was dealt again: rebuild enough of a stack to be worth
    // photographing before taking the picture.
    for (let guard = 0; guard < 6 && mirrorLanding(mirror, STACK_COL) > ROWS - 3; guard++) playOneMove(STACK_COL);

    const stackCol = STACK_COL;
    const stackRow = mirrorLanding(mirror, stackCol);
    console.log(`    column ${stackCol} now holds ${ROWS - 1 - stackRow} piece(s); the landing ring should be at row ${stackRow}`);
    t = holdLand(dev, colX(stackCol), THUMB_LY, t, 150);
    fb = dev.fbSnapshot();
    const stackRingInk = landPx(fb, colX(stackCol) + HOLE_R - 3, rowY(stackRow));
    const belowRing = stackRow + 1 < ROWS ? landPx(fb, colX(stackCol), rowY(stackRow + 1)) : C_RED;
    check("mid-game, the landing ring sits on TOP of the stack already in that column",
        isRedish(stackRingInk) && !near(belowRing, C_WHITE),
        `column ${stackCol}, ring at row ${stackRow} ${describe(stackRingInk)}, hole below it ${describe(belowRing)}`);
    await writeScreenshot("four-highlight.png", fb,
        `mid-game highlight: column ${stackCol} washed head to foot, landing ring resting on the stack in it`);

    // Let go of that one without dropping anything: sliding off the board is
    // not a thing this app offers, so the piece goes in - which is fine, it
    // is her move either way, and the win loop below just carries on.
    t = settle(dev, t, RELEASE_GRACE_MS + 2200);
    for (const line of dev.drainLog()) {
        const m = line.match(/four: drop col=(\d+) row=(\d+) player=(\d+)/);
        if (m) mirrorApply(mirror, Number(m[1]), Number(m[2]), Number(m[3]));
        if (line.includes("four: new game")) for (let k = 0; k < ROWS * COLS; k++) mirror.cells[k] = 0;
    }

    // ---- 5. play the game out to a win ----------------------------------
    console.log("\n-- playing on until somebody wins --");
    let winLine: string | undefined;
    let moves = 0;
    while (!winLine && moves < 30) {
        const col = chooseChildMove(mirror);
        if (col < 0) break;
        moves++;
        t = holdLand(dev, colX(col), THUMB_LY, t, 120);
        t = settle(dev, t, RELEASE_GRACE_MS + 2200); // her fall, the device's think, its fall
        for (const line of dev.drainLog()) {
            const m = line.match(/four: drop col=(\d+) row=(\d+) player=(\d+)/);
            if (m) mirrorApply(mirror, Number(m[1]), Number(m[2]), Number(m[3]));
            if (line.includes("four: win")) winLine = line;
            if (line.includes("four: new game")) { for (let i = 0; i < ROWS * COLS; i++) mirror.cells[i] = 0; }
        }
        if (winLine) break;
    }
    check("a game reaches a win", !!winLine, winLine ?? `(no win after ${moves} of her moves)`);

    // The four in a row breathe: the winning pieces change size frame to
    // frame while the rest of the board holds perfectly still. That is the
    // whole "who won" announcement, and there is no text in it.
    // Sampled across one whole breath (PULSE_MS): how far ink reaches out of
    // the widest hole on the board, frame by frame. A still board gives the
    // same number every time and a piece that never exceeds HOLE_R is not
    // swelling at all; both are what this has to rule out. The frame where
    // that reach is greatest is also the one worth photographing, so the
    // screenshot is chosen by measurement rather than by whenever the test
    // happened to stop ticking.
    // The metric is the radius of SOLID PIECE COLOUR out of a hole's centre,
    // walked outward until the colour stops - the disc itself, not "any ink
    // out here somewhere". Two earlier versions of this measured the latter
    // and were useless in different ways: probing to HOLE_R+13 kept hitting
    // the NEIGHBOURING piece and reported a constant 34, and probing to the
    // neighbour's edge kept hitting the halo and reported a near-constant 28
    // whose maximum frame was the one where the piece was at its SMALLEST.
    function discRadius(snap: Uint8Array, c: number, r: number): number {
        const centre = landPx(snap, colX(c), rowY(r));
        const isColour = isRedish(centre) ? isRedish : isBluish(centre) ? isBluish : null;
        if (!isColour) return 0;
        let d = 0;
        while (d < CELL - HOLE_R - 1 && isColour(landPx(snap, colX(c) + d, rowY(r)))) d++;
        return d;
    }

    const winSizes: number[] = [];
    let bestShot = dev.fbSnapshot();
    let bestReach = -1;
    for (let phase = 0; phase < 14; phase++) {
        t = settle(dev, t, 70);
        const snap = dev.fbSnapshot();
        let widest = 0;
        for (let c = 0; c < COLS; c++) for (let r = 0; r < ROWS; r++) {
            const d = discRadius(snap, c, r);
            if (d > widest) widest = d;
        }
        winSizes.push(widest);
        if (widest > bestReach) { bestReach = widest; bestShot = snap; }
    }
    check("the winning line BREATHES - some piece grows past its own hole, and by a different amount frame to frame",
        Math.max(...winSizes) > HOLE_R && new Set(winSizes).size > 1, `piece radius per sample: ${winSizes.join(", ")}`);

    await writeScreenshot("four-won.png", bestShot, "a won game: the four in a row swollen and blooming, no text anywhere");
    fb = dev.fbSnapshot();

    // No waiting piece during the celebration: the absence of "something to
    // drop" is what says a touch now means something else (decision 0002's
    // "no modal state" - the screen says so).
    let anyWaiting = false;
    for (let c = 0; c < COLS; c++) if (!near(landPx(fb, colX(c), 26), C_WHITE)) anyWaiting = true;
    check("no waiting piece is shown while the game is over - the screen says a touch means something else now", !anyWaiting);

    // ---- a touch moves it along, the board drains, a new game begins ----
    console.log("\n-- a touch during the celebration, then the board drains --");
    t = settle(dev, t, CELEBRATE_SKIP_MS + 100);
    dev.drainLog();
    t = holdLand(dev, colX(3), THUMB_LY, t, 60);
    t = settle(dev, t, 1400);
    const newGame = dev.fwLogLines().findLast((l) => l.includes("four: new game"));
    check("the board empties itself and a fresh game starts, with no menu and no text", !!newGame, newGame ?? "(no new game line)");

    fb = dev.fbSnapshot();
    let cleared = true;
    for (let c = 0; c < COLS; c++) for (let r = 0; r < ROWS; r++) {
        if (!near(landPx(fb, colX(c), rowY(r)), C_WHITE)) cleared = false;
    }
    check("the fresh board really is empty - all 42 holes back to paper", cleared);

    // ---- endurance, and the one ending the scripted game above cannot
    // reach: A FULL BOARD WITH NO WINNER.
    //
    // Run on a SECOND device, with the framebuffer invariant check switched
    // off. That check diffs 165k pixels per tick and this section drives
    // hundreds of thousands of them; the invariant it proves is a property
    // of render_span()/flush(), which the scripted game above already
    // exercises across every kind of frame this app has (highlight, fall,
    // pulse, drain). Paying for it again here would buy nothing and cost
    // minutes.
    //
    // HOW THE DRAW IS REACHED, AND WHY THIS IS NOT FLAKY. Everything in this
    // module is deterministic: four.c seeds its generator from a fixed
    // constant and stirs it only with the timings of her drops, which this
    // test scripts exactly, so the same moves produce the same games every
    // run. A draw needs neither side to complete a four, which does not
    // happen against a partner playing to win (the loop above wins in a
    // handful of moves) - so this player is deliberately different: it
    // blocks the device's fours, builds threes to keep it busy answering,
    // and never completes a four of its own. Against that, the boards run
    // long and one of them fills.
    //
    // If this ever fails, read it as "no draw was reached inside the game
    // budget" rather than as "the draw path is broken": a change to
    // ai_choose() or to this app's timings moves which game draws, and the
    // budget below (which is comfortably past where the draw lands today)
    // may simply need raising.
    console.log("\n-- endurance: games back to back, hunting the full-board-no-winner ending --");
    const raw = await loadRawDevice();
    const mirror2 = newMirror();
    let rawT = 1000;
    let games = 0, wins = 0, draws = 0, resetAfterDraw = false;
    let sawDraw = false;
    const GAME_BUDGET = 20;
    for (let step = 0; step < 2000 && games < GAME_BUDGET && !resetAfterDraw; step++) {
        const col = chooseStalemateMove(mirror2);
        // col < 0 means the mirror says the board is full, i.e. the game just
        // ended in the draw this loop is hunting for: there is nothing left to
        // play, so this step is spent waiting out DRAW_PAUSE_MS plus the drain
        // rather than ticking 15ms at a time and running out of budget before
        // the fresh board arrives. (It did exactly that on the first run.)
        if (col < 0) {
            for (let i = 0; i < 170; i++) { rawT += 15; raw.touchLand(false, 0, 0, rawT); }
        } else {
            for (let i = 0; i < 9; i++) { rawT += 15; raw.touchLand(true, colX(col), THUMB_LY, rawT); }
            for (let i = 0; i < 170; i++) { rawT += 15; raw.touchLand(false, 0, 0, rawT); }
        }
        for (const line of raw.drainLog()) {
            const m = line.match(/four: drop col=(\d+) row=(\d+) player=(\d+)/);
            if (m) mirrorApply(mirror2, Number(m[1]), Number(m[2]), Number(m[3]));
            if (line.includes("four: win")) wins++;
            if (line.includes("four: draw")) { draws++; sawDraw = true; }
            if (line.includes("four: new game")) {
                games++;
                if (sawDraw) resetAfterDraw = true;
                for (let i = 0; i < ROWS * COLS; i++) mirror2.cells[i] = 0;
            }
        }
    }
    check("games run back to back, each ending and dealing a fresh board on its own",
        games >= 3, `${games} games completed (${wins} won, ${draws} drawn)`);
    check("a board that fills up with nobody winning ends the game too - no pulse, and it deals again",
        draws >= 1 && resetAfterDraw,
        draws >= 1 ? `drawn game followed by a fresh board: ${resetAfterDraw}` : `no draw inside ${GAME_BUDGET} games`);

    // ---- 6. the invariants, across every tick above ---------------------
    console.log("\n=== invariants across this whole run (EVERY tick, animation frames included) ===");
    console.log(`    ${ticksChecked} ticks checked in total`);
    const byKind = new Map<string, number>();
    for (const v of violations) byKind.set(v.kind, (byKind.get(v.kind) ?? 0) + 1);
    check("every pushed window's row length was a multiple of 8 (decision 0001)",
        (byKind.get("8px-rule") ?? 0) === 0, `${byKind.get("8px-rule") ?? 0} violation(s)`);
    check("no framebuffer pixel ever changed outside that tick's own pushed rectangles (the fall, the pulse and the drain included)",
        (byKind.get("outside-push") ?? 0) === 0, `${byKind.get("outside-push") ?? 0} violation(s)`);
    if (violations.length > 0) {
        console.log("first few violations:");
        for (const v of violations.slice(0, 10)) console.log(`    [${v.kind}] ${v.detail}`);
    }

    console.log("\nscreenshots:");
    for (const p of shots) console.log(`    ${p}`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
