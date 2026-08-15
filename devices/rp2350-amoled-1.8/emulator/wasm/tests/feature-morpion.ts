// feature-morpion: headless verification of the noughts and crosses app
// (firmware/apps/morpion.c), against the REAL firmware compiled to wasm
// (decision 0003) - nothing here reimplements the app's logic. Every
// assertion reads the framebuffer, the firmware's own log lines, or the
// push-window list the firmware itself produced.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/feature-morpion.ts
//
// WHAT THIS FILE PROVES, IN ORDER:
//   1. the board renders as one rounded slab with NINE white discs punched
//      out of it and no line anywhere, inside the VISIBLE canvas (gfx.h's
//      PANEL_BEZEL_MARGIN_PX - the case hides a band the emulator cannot
//      show), with X's mark waiting in the tray; and the arena footprint is
//      REPORTED, not assumed;
//   2. THE CANDIDATE CELL IS LEGIBLE WITH A THUMB ON IT. This is the whole
//      design problem of a 3x3 grid on this panel and it is asserted as
//      arithmetic rather than as prose: for each of the nine cells, a 76px
//      occlusion disc (AGENTS.md's child fingertip) is placed on the cell
//      centre and the band pixels still visible outside it are counted,
//      along the column centre line and along the row centre line
//      separately. Both have to survive, because a column and a row name a
//      cell, and the centre lines are the harshest place to count since the
//      thumb sits exactly on them;
//   3. the two bands cross on the candidate, the candidate's own disc wears
//      a deeper wash than the bands, and a GHOST of the mark that would be
//      made sits in it - faint, so it can never be confused with a played
//      one;
//   4. releasing draws the mark ON, stroke by stroke (caught partway
//      through, not only once finished), in the cell the thumb was over;
//   5. WHOSE TURN IT IS, at the instant it changes, which is the only
//      moment the cue matters and the only thing the screen has to say with
//      no words available: the whole slab changes temperature, the new
//      player's CROSS washes in over the centre cell, and their mark pops
//      into the tray in a different SHAPE as well as a different colour;
//   6. THE BOARD NEVER MOVES ON ITS OWN. Two people play this, one puck
//      between them: over five seconds of nobody touching the glass, not
//      one pixel outside the tray changes;
//   7. a taken cell says no in grey BEFORE the release, and refuses it;
//   8. a game is won: the three in a line are STRUCK THROUGH by an ink
//      stroke drawn along them, they breathe, and the board wears the
//      winner's colour. Then the board is WIPED and a fresh game is there,
//      with no text and no button anywhere in any of it;
//   9. a board that fills with nobody winning ends too, and the difference
//      is exactly the absence of the celebration: no ink anywhere on the
//      slab, and a board wearing nobody's colour;
//  10. EVERY tick of all of the above - the ink stroke, the hand-off, the
//      strike, the pulse and the wipe included, not only the frames at rest
//      - obeys decision 0001's 8px rule and the "no pixel changes outside
//      the pushed rectangle" invariant.
//
// The gesture's behaviour under a REALISTIC touch stream (contact dropping
// out ~34 times a second) is a separate file, deliberately, and AGENTS.md's
// regression-test section requires both: repro-touch-dropout-morpion.ts.
// This one drives clean input, which is what makes it a readable statement
// of what the app is supposed to do.
//
// Screenshots go to preview/morpion-*.png, written LANDSCAPE (448x368, the
// way the device is held for this app) rather than in the panel's own
// portrait orientation: the owner judges these by eye. The rotation is
// applied at encode time only; every assertion reads the framebuffer in its
// real layout.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const ROOT = join(import.meta.dir, "..", "..", "..");
const PREVIEW_DIR = join(ROOT, "preview");

const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const LAND_H = PANEL_W; // 368
// g_apps[] = { chrono, sketch("draw"), timer, four, morpion }
const APP_MORPION = 4;
const APP_ARENA_BYTES = 65536; // app.h APP_ARENA_BYTES

// ---- mirrors of morpion.c's own constants, DERIVED THE SAME WAY THE
// FIRMWARE DERIVES THEM (integer division and all) rather than written down
// as the numbers they currently come to. The board is laid out against the
// VISIBLE canvas, so one edit to gfx.h's PANEL_BEZEL_MARGIN_PX moves every
// coordinate in this file too; a test that hardcoded "cellCx(c) = 155+110c"
// would then be asserting about a board that no longer exists, and would do
// it by failing in nine places at once.
const GRID = 3;
const BEZEL = 10;                       // gfx.h PANEL_BEZEL_MARGIN_PX
const SAFE_X0 = BEZEL, SAFE_X1 = LAND_W - BEZEL;
const SAFE_Y0 = BEZEL, SAFE_Y1 = LAND_H - BEZEL;
const SAFE_H = SAFE_Y1 - SAFE_Y0;
const SLAB_PAD = 8;
const CELL = Math.floor((SAFE_H - 2 * SLAB_PAD) / GRID);   // 110
const BOARD_W = GRID * CELL;
const SLAB_W = BOARD_W + 2 * SLAB_PAD;
const SLAB_H = SLAB_W;
const SLAB_X1 = SAFE_X1, SLAB_X0 = SLAB_X1 - SLAB_W;
const SLAB_Y0 = SAFE_Y0 + Math.floor((SAFE_H - SLAB_H) / 2);
const SLAB_Y1 = SLAB_Y0 + SLAB_H;
const BOARD_X0 = SLAB_X0 + SLAB_PAD;
const BOARD_Y0 = SLAB_Y0 + SLAB_PAD;
const DISC_R = 39;
const TRAY_CX = (SAFE_X0 + SLAB_X0) / 2;
const TRAY_CY = (SLAB_Y0 + SLAB_Y1) / 2;

const RELEASE_GRACE_MS = 300;
const INK_MS = 340;
const HANDOFF_MS = 420;
const WIN_SKIP_MS = 1200;
const WIPE_MS = 780;
const DRAW_PAUSE_MS = 900;

// morpion.c uses C integer division for the half-cell; mirrored exactly.
const cellCx = (c: number) => BOARD_X0 + Math.floor(CELL / 2) + c * CELL;
const cellCy = (r: number) => BOARD_Y0 + Math.floor(CELL / 2) + r * CELL;
const cxOf = (cell: number) => cellCx(cell % GRID);
const cyOf = (cell: number) => cellCy(Math.floor(cell / GRID));

const P_X = 1; // red
const P_O = 2; // blue

// The colours morpion.c paints, as the LOGICAL rgb565 byte pair the
// framebuffer holds (the fb stores px_swap(v) as a uint16, which on this
// little-endian target puts the logical high byte first - the same
// convention feature-four.ts and feature-sketch-palette.ts use).
const C_WHITE: [number, number] = [0xff, 0xff];
const C_BAND_X: [number, number] = [0xfd, 0x55];   // #FFAAAD
const C_CELL_X: [number, number] = [0xfb, 0x6e];   // #FF6E73, the candidate's own disc
const C_BAND_TAKEN: [number, number] = [0xb5, 0x96];
const C_CELL_TAKEN: [number, number] = [0xd6, 0xba];
// The slab wears whose turn it is: warm for X, cool for O, neutral when no
// side owns the moment (the wipe, the nobody-won beat). Three separate
// constants and not one, because "the whole board changed temperature" is a
// turn cue in its own right and a test that treated them as interchangeable
// would be blind to it.
const C_SLAB_X: [number, number] = [0xe6, 0xda];
const C_SLAB_O: [number, number] = [0xd6, 0xdc];
const C_SLAB_NEUTRAL: [number, number] = [0xde, 0xfb];
const slabFor = (p: number) => (p === P_X ? C_SLAB_X : p === P_O ? C_SLAB_O : C_SLAB_NEUTRAL);

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

// ---------------------------------------------------------------------
// Invariant tracking, same shape as feature-four.ts's (see that file's
// header for why this lives inline in every test in this directory rather
// than in a shared module: each one is self-contained).
// ---------------------------------------------------------------------
type Violation = { kind: string; detail: string };
const violations: Violation[] = [];
let ticksChecked = 0;

const WASM_MODULE = await WebAssembly.compile(readFileSync(WASM_PATH));

function hostImports(pushLog: (s: string) => void, getMemory: () => WebAssembly.Memory) {
    const decoder = new TextDecoder();
    return {
        env: {
            js_log(ptr: number, len: number) {
                pushLog(decoder.decode(new Uint8Array(getMemory().buffer, ptr, len)));
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
}

async function loadDevice(checkInvariants: boolean) {
    let memory!: WebAssembly.Memory;
    const fwLogLines: string[] = [];
    const instance = await WebAssembly.instantiate(
        WASM_MODULE, hostImports((s) => fwLogLines.push(s), () => memory));
    memory = instance.exports.memory as WebAssembly.Memory;
    const exp = instance.exports as any;
    if (exp.emu_init() !== 1) throw new Error("emu_init() failed - see [fw] log lines above");

    function fbSnapshot(): Uint8Array {
        return new Uint8Array(memory.buffer, exp.emu_fb(), PANEL_W * PANEL_H * 2).slice();
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
        // rectangle's row length. EVERY tick this file drives on the
        // instrumented device goes through here, animation frames included:
        // the invariant has to hold ACROSS the ink stroke, the hand-off,
        // the strike and the wipe, not only once they are over. This
        // project has been bitten by residue left mid-animation three
        // times.
        tickChecked(nowMs: number) {
            if (!checkInvariants) { exp.emu_tick(nowMs); return; }
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
            // actually differ are resolved to coordinates - the same
            // optimisation feature-four.ts explains: this app animates
            // almost continuously, so "nothing changed" is the exception.
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
        // coordinates (emu_abi.h: the firmware's own coordinate handling is
        // under test, so mapping the pointer back is the emulator's job).
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

// A device already sitting in morpion with the opening hand-off behind it.
async function freshMorpion(checkInvariants: boolean): Promise<{ dev: Device; t: number }> {
    const dev = await loadDevice(checkInvariants);
    dev.tickChecked(0);
    dev.appSwitch(APP_MORPION);
    dev.tickChecked(1000);
    // The log is deliberately NOT drained: enter()'s own "state=N bytes"
    // line is the arena measurement section 1 reports, and it is printed
    // exactly once, here.
    return { dev, t: 1000 };
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

// Decoded to real channels, never compared as raw byte pairs: rgb565 packs
// green ACROSS the two bytes, so "high byte big, low byte small" matches a
// warm light grey as readily as it matches red. feature-four.ts grew six
// phantom red pixels per hole that way, invisible until a probe box
// happened to sit over a rim.
function channels(p: [number, number]): [number, number, number] {
    const v = (p[0] << 8) | p[1];
    return [
        Math.round((((v >> 11) & 0x1f) * 255) / 31),
        Math.round((((v >> 5) & 0x3f) * 255) / 63),
        Math.round(((v & 0x1f) * 255) / 31),
    ];
}

// TWO STRENGTHS OF PREDICATE, and the distinction is load-bearing here in a
// way it was not for Connect Four's flat discs.
//
// A PLAYED mark is opaque ink: #FF0000 or #0000FF in its interior. The
// GHOST of a mark that has not been made yet is the same ink at alpha 118
// over the candidate cell's own deeper wash, which comes out at #FF3D42 -
// unmistakably red ink to a child, and it has to be, but NOT a move. So
// "is there ink here" and "has a mark been played here" are different
// questions and get different functions. Without the split, every assertion
// about what is on the board would have counted the proposal as a move.
//
// The thresholds are solved rather than guessed: the wash under the ghost
// is (255,109,115) and the ghost's own core is (255,61,66), so g<85
// separates them with 24 counts of margin on each side, and solid ink's
// (255,0,0) clears g<40 by 40.
function isGhostX(p: [number, number]): boolean {
    const [r, g, b] = channels(p);
    return r > 200 && g < 85 && b < 90;
}
function isGhostO(p: [number, number]): boolean {
    const [r, g, b] = channels(p);
    return b > 200 && r < 90 && g < 90;
}
function isSolidX(p: [number, number]): boolean {
    const [r, g, b] = channels(p);
    return r > 200 && g < 40 && b < 40;
}
function isSolidO(p: [number, number]): boolean {
    const [r, g, b] = channels(p);
    return b > 200 && r < 40 && g < 40;
}
const solidFor = (p: number) => (p === P_X ? isSolidX : isSolidO);
const ghostFor = (p: number) => (p === P_X ? isGhostX : isGhostO);

// ---- driving ---------------------------------------------------------
function settle(dev: Device, t0: number, durationMs: number, stepMs = 15): number {
    let t = t0;
    for (let waited = 0; waited < durationMs; waited += stepMs) {
        t += stepMs;
        dev.touchLand(false, 0, 0, t);
    }
    return t;
}

function holdCell(dev: Device, cell: number, t0: number, durationMs: number, stepMs = 15): number {
    let t = t0;
    for (let held = 0; held < durationMs; held += stepMs) {
        t += stepMs;
        dev.touchLand(true, cxOf(cell), cyOf(cell), t);
    }
    return t;
}

// A whole move: press, hold long enough to arm, release, then wait out the
// release grace, the ink stroke and the hand-off so the app is back at rest
// and the next player may play.
function playMove(dev: Device, cell: number, t0: number): number {
    const t = holdCell(dev, cell, t0, 160);
    return settle(dev, t, RELEASE_GRACE_MS + INK_MS + HANDOFF_MS + 260);
}

// How many pixels of solid `player` ink sit in a cell's disc: is a mark
// there, and whose. Counted over a box rather than probed at one point,
// because a mark is a STROKE - its exact centre is inside the O's hole and
// between the X's arms, so a single centre sample answers neither question.
function markInk(fb: Uint8Array, cell: number, player: number): number {
    const solid = solidFor(player);
    const cx = cxOf(cell), cy = cyOf(cell);
    let n = 0;
    for (let ly = cy - DISC_R; ly <= cy + DISC_R; ly++) {
        for (let lx = cx - DISC_R; lx <= cx + DISC_R; lx++) {
            if (lx < 0 || lx >= LAND_W || ly < 0 || ly >= LAND_H) continue;
            if (solid(landPx(fb, lx, ly))) n++;
        }
    }
    return n;
}

// The same, for the tray: whose mark is waiting, outside the board.
function trayInk(fb: Uint8Array, player: number): number {
    const solid = solidFor(player);
    let n = 0;
    for (let ly = TRAY_CY - 34; ly <= TRAY_CY + 34; ly++) {
        for (let lx = TRAY_CX - 34; lx <= TRAY_CX + 34; lx++) {
            if (lx < 0 || lx >= LAND_W || ly < 0 || ly >= LAND_H) continue;
            if (solid(landPx(fb, lx, ly))) n++;
        }
    }
    return n;
}

// Solid ink ON THE SLAB, i.e. anywhere inside the board that is NOT inside
// one of the nine discs. A mark reaches 36px out of a 39px disc and never
// leaves it, and a ghost lives in a disc too, so the ONLY things this app
// can put here are the win strike and the bloom that goes with it. That
// makes this one number the difference between "somebody won" and "nobody
// did" - which, with no words available, is exactly the distinction the
// screen has to carry (see the draw section).
function inkOnSlab(fb: Uint8Array, player: number): number {
    const solid = solidFor(player);
    let n = 0;
    for (let ly = SLAB_Y0; ly <= SLAB_Y1; ly++) {
        for (let lx = SLAB_X0; lx <= SLAB_X1; lx++) {
            let inDisc = false;
            for (let cell = 0; cell < GRID * GRID && !inDisc; cell++) {
                const dx = lx - cxOf(cell), dy = ly - cyOf(cell);
                if (dx * dx + dy * dy <= (DISC_R + 2) * (DISC_R + 2)) inDisc = true;
            }
            if (inDisc) continue;
            if (solid(landPx(fb, lx, ly))) n++;
        }
    }
    return n;
}

// The whole board as a string, so two frames can be compared for "are the
// same marks in the same places" independently of anything animating over
// them. A cell counts as played only on SOLID ink, so a ghost never reads
// as a move.
function census(fb: Uint8Array): string {
    let s = "";
    for (let cell = 0; cell < GRID * GRID; cell++) {
        const x = markInk(fb, cell, P_X), o = markInk(fb, cell, P_O);
        s += x > 200 ? "X" : o > 200 ? "O" : x + o === 0 ? "." : "?";
    }
    return s;
}

// Do the two framebuffers agree everywhere OUTSIDE the tray strip? The tray
// is allowed to change: the mark bobs there forever, and that is the point
// of it - it is the one moving thing on a still screen and one of the cues
// saying whose turn it is. Everything from the slab's left edge rightward
// is the board, and the board must not move by itself.
function unchangedOnBoard(a: Uint8Array, b: Uint8Array): { lx: number; ly: number } | null {
    for (let ly = 0; ly < LAND_H; ly++) {
        for (let lx = SLAB_X0; lx < LAND_W; lx++) {
            const p = landPx(a, lx, ly), q = landPx(b, lx, ly);
            if (p[0] !== q[0] || p[1] !== q[1]) return { lx, ly };
        }
    }
    return null;
}

// ---------------------------------------------------------------------
// PNG. Same chunk/CRC/zlib-wrap machinery as feature-four.ts's encoder,
// writing the LANDSCAPE image so the file opens the way the device is held.
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
    return [
        Math.round((((v >> 11) & 0x1f) * 255) / 31),
        Math.round((((v >> 5) & 0x3f) * 255) / 63),
        Math.round(((v & 0x1f) * 255) / 31),
    ];
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
// 2. THE CANDIDATE CELL UNDER A THUMB, measured on nine fresh devices.
//
// One device per cell, deliberately: a gesture that is not released cannot
// be cancelled without touching the board somewhere else (every point on
// the canvas belongs to some cell - morpion.c's cell_from_land() clamps
// rather than rejecting, so there is no dead space to let go over), and a
// released gesture plays a mark, which changes the next cell's board. A
// fresh module per cell makes each measurement independent and the whole
// scan reproducible. The invariant checker is off for these: it is a
// property of render_span()/flush() that the instrumented run below
// exercises across every kind of frame this app has, and paying for a
// 165k-pixel diff twice buys nothing.
// ---------------------------------------------------------------------
async function measureThumbOcclusion(): Promise<{ worstCol: number; worstRow: number; per: string[] }> {
    const THUMB_R = 38;   // AGENTS.md: a child's fingertip contact is ~75px
    let worstCol = 99999, worstRow = 99999;
    const per: string[] = [];
    for (let cell = 0; cell < GRID * GRID; cell++) {
        const { dev, t: t0 } = await freshMorpion(false);
        let t = settle(dev, t0, HANDOFF_MS + 100);
        t = holdCell(dev, cell, t, 170);
        const snap = dev.fbSnapshot();
        const cx = cxOf(cell), cy = cyOf(cell);
        let colVisible = 0, rowVisible = 0;
        for (let ly = SLAB_Y0; ly <= SLAB_Y1; ly++) {
            if (Math.abs(ly - cy) <= THUMB_R) continue;      // hidden by the thumb
            if (near(landPx(snap, cx, ly), C_BAND_X)) colVisible++;
        }
        for (let lx = SLAB_X0; lx <= SLAB_X1; lx++) {
            if (Math.abs(lx - cx) <= THUMB_R) continue;
            if (near(landPx(snap, lx, cy), C_BAND_X)) rowVisible++;
        }
        if (colVisible < worstCol) worstCol = colVisible;
        if (rowVisible < worstRow) worstRow = rowVisible;
        per.push(`${cell}:${colVisible}/${rowVisible}`);
    }
    return { worstCol, worstRow, per };
}

// ---------------------------------------------------------------------
async function main() {
    console.log("=== feature: morpion, two players (firmware/apps/morpion.c) ===\n");
    const { dev, t: t0 } = await freshMorpion(true);
    let t = t0;
    check("switched into the morpion app", dev.appCurrent() === APP_MORPION, `app_current()=${dev.appCurrent()}`);

    // ---- 1. the board, and the arena cost, reported not assumed ---------
    const sizeLine = dev.fwLogLines().find((l) => l.includes("morpion: state="));
    const sizeMatch = sizeLine?.match(/state=(\d+) bytes \(arena (\d+)\)/);
    const measuredBytes = sizeMatch ? Number(sizeMatch[1]) : -1;
    check("morpion_state_t's measured size fits the arena, with the fit reported (not assumed)",
        !!sizeMatch && measuredBytes > 0 && measuredBytes <= APP_ARENA_BYTES,
        sizeLine ?? "(no sizeof line seen in the firmware log)");
    if (measuredBytes > 0) {
        console.log(`    morpion_state_t = ${measuredBytes} bytes of ${APP_ARENA_BYTES} (${((measuredBytes / APP_ARENA_BYTES) * 100).toFixed(2)}%)`);
    }

    t = settle(dev, t, HANDOFF_MS + 200);
    let fb = dev.fbSnapshot();

    // The slab is flat between the discs, every disc is paper white, and
    // there is no line, no border and no right angle anywhere: the ONLY
    // thing that makes a grid is the arrangement of the nine circles.
    const gutter = landPx(fb, cellCx(0) + Math.floor(CELL / 2), cellCy(1));
    check("the board's face is one flat slab between its discs", near(gutter, C_SLAB_X), describe(gutter));
    let allDiscsWhite = true;
    const discMiss: string[] = [];
    for (let cell = 0; cell < GRID * GRID; cell++) {
        const d = DISC_R * 0.8;
        const pts: [number, number][] = [
            [cxOf(cell) - d, cyOf(cell)], [cxOf(cell) + d, cyOf(cell)],
            [cxOf(cell), cyOf(cell) - d], [cxOf(cell), cyOf(cell) + d],
        ];
        for (const [x, y] of pts) {
            if (!near(landPx(fb, x, y), C_WHITE)) {
                allDiscsWhite = false;
                if (discMiss.length < 4) discMiss.push(`cell ${cell} at (${Math.round(x)},${Math.round(y)})=${describe(landPx(fb, x, y))}`);
            }
        }
    }
    check("all nine cells are discs punched out in paper white, and nothing is drawn between them",
        allDiscsWhite, discMiss.join(", "));

    // The slab's own corner: a lozenge with a 46px radius, so the bounding
    // box's corner pixel is still paper. Decision 0009, asserted rather than
    // asserted-in-prose.
    const slabCorner = landPx(fb, SLAB_X0 + 3, SLAB_Y0 + 3);
    check("the slab's bounding-box corner is still paper - a lozenge, not a square (decision 0009)",
        near(slabCorner, C_WHITE), describe(slabCorner));

    // X starts, and the screen says so twice over: the mark waiting in the
    // TRAY is X's, and the whole board wears X's warm grey.
    const xWaiting = trayInk(fb, P_X);
    const oWaiting = trayInk(fb, P_O);
    check("X starts, and its mark waits in the tray - outside the board, where a thumb on the board never goes",
        xWaiting > 250 && oWaiting === 0, `${xWaiting} red / ${oWaiting} blue pixels in the tray`);

    // ---- 2. the candidate cell, with a thumb on it ----------------------
    console.log("\n-- a 76px thumb on each of the nine cells, and what survives around it --");
    const occl = await measureThumbOcclusion();
    console.log(`    column/row band pixels still visible, per cell: ${occl.per.join("  ")}`);
    check("with a 76px thumb on the cell, the COLUMN band is still visible above and below it, for every one of the nine",
        occl.worstCol >= 60, `the worst cell kept ${occl.worstCol} px of column band on its own centre line`);
    check("...and the ROW band either side of it, for every one of the nine - a column and a row name a cell",
        occl.worstRow >= 60, `the worst cell kept ${occl.worstRow} px of row band on its own centre line`);

    // ---- 3. the cross, the hotter candidate cell, and the ghost ---------
    console.log("\n-- a thumb goes down on the middle-left cell (3) --");
    dev.drainLog();
    const CAND = 3;
    t = holdCell(dev, CAND, t, 170);
    const hoverLine = dev.fwLogLines().findLast((l) => l.includes("morpion: hover"));
    check("the app reports the thumb's cell", !!hoverLine && hoverLine.includes("cell=3"), hoverLine ?? "(no hover line)");

    fb = dev.fbSnapshot();
    const colProbe = landPx(fb, cxOf(CAND), SLAB_Y0 + 8);
    const rowProbe = landPx(fb, SLAB_X1 - 8, cyOf(CAND));
    check("the candidate's column and row are both washed, head to foot and side to side",
        near(colProbe, C_BAND_X) && near(rowProbe, C_BAND_X),
        `column top ${describe(colProbe)}, row right ${describe(rowProbe)}`);

    const offCross = landPx(fb, cellCx(2), cellCy(2) - DISC_R - 12);
    check("a cell on neither the candidate's row nor its column is untouched slab",
        near(offCross, C_SLAB_X), describe(offCross));

    const candDisc = landPx(fb, cxOf(CAND), cyOf(CAND) + DISC_R - 6);
    check("where the two bands cross, the cell's own disc is a DEEPER wash than the bands themselves",
        near(candDisc, C_CELL_X), `${describe(candDisc)} (the bands are ${describe(C_BAND_X)})`);

    let ghostPx = 0;
    for (let ly = cyOf(CAND) - DISC_R; ly <= cyOf(CAND) + DISC_R; ly++) {
        for (let lx = cxOf(CAND) - DISC_R; lx <= cxOf(CAND) + DISC_R; lx++) {
            if (ghostFor(P_X)(landPx(fb, lx, ly))) ghostPx++;
        }
    }
    let elsewhere = 0;
    for (let cell = 0; cell < GRID * GRID; cell++) if (cell !== CAND) elsewhere += markInk(fb, cell, P_X) + markInk(fb, cell, P_O);
    check("a GHOST of the mark that would be made sits in the candidate cell, faint rather than solid, and nowhere else",
        ghostPx > 300 && markInk(fb, CAND, P_X) === 0 && elsewhere === 0,
        `${ghostPx} px of faint ink, ${markInk(fb, CAND, P_X)} px of solid ink in it, ${elsewhere} px anywhere else`);

    // ---- the thumb slides ------------------------------------------------
    console.log("\n-- the thumb slides to cell 7 (a different row AND a different column) --");
    t = holdCell(dev, 4, t, 45);
    // Long enough to clear morpion.c's SETTLE_MS: a cell the thumb has only
    // brushed past is deliberately not a cell a release plays (see that
    // constant - it is what stops the controller's own position wander from
    // choosing the move), so a test that let go after 60ms here would be
    // asserting about the previous cell.
    t = holdCell(dev, 7, t, 150);
    fb = dev.fbSnapshot();
    const oldCol = landPx(fb, cxOf(CAND), SLAB_Y0 + 8);
    const newCol = landPx(fb, cxOf(7), SLAB_Y0 + 8);
    const newRow = landPx(fb, SLAB_X1 - 8, cyOf(7));
    check("the cross followed the thumb on both axes, and where it was is back to plain slab",
        near(newCol, C_BAND_X) && near(newRow, C_BAND_X) && near(oldCol, C_SLAB_X),
        `new column ${describe(newCol)}, new row ${describe(newRow)}, old column ${describe(oldCol)}`);

    // ---- 4. release draws the mark on -----------------------------------
    console.log("\n-- the thumb lifts --");
    dev.drainLog();
    let markLine: string | undefined;
    for (let waited = 0; waited < RELEASE_GRACE_MS + 200 && !markLine; waited += 15) {
        t += 15;
        dev.touchLand(false, 0, 0, t);
        markLine = dev.fwLogLines().findLast((l) => l.includes("morpion: mark"));
    }
    check("releasing plays a mark, in the cell the thumb was over, for the player whose turn it is",
        !!markLine && markLine.includes("cell=7") && markLine.includes("player=1"), markLine ?? "(no mark line)");

    // Catch it being DRAWN. A mark that appeared whole would pass every
    // at-rest assertion in this file and fail this one.
    //
    // The cross is sampled on the same frames, because the mark is COMMITTED
    // the instant it starts being drawn: the cell is occupied from that
    // moment on, and a naive "is this cell taken" test would wash the cross
    // grey exactly while the promise it made is being kept. It shipped that
    // way for one build.
    const inkTrace: number[] = [];
    let crossStayedLit = true;
    for (let i = 0; i < Math.ceil(INK_MS / 15) + 6; i++) {
        t += 15;
        dev.touchLand(false, 0, 0, t);
        const snap = dev.fbSnapshot();
        inkTrace.push(markInk(snap, 7, P_X));
        if (inkTrace[inkTrace.length - 1]! > 0 && inkTrace[inkTrace.length - 1]! < 1000) {
            if (!near(landPx(snap, cxOf(7), SLAB_Y0 + 8), C_BAND_X)) crossStayedLit = false;
        }
    }
    check("the cross stays in the player's colour while their mark is being drawn - it does not go grey the moment the cell is taken",
        crossStayedLit, `column band at the top of cell 7 while inking`);
    const inkFinal = inkTrace[inkTrace.length - 1]!;
    const inkEarly = inkTrace[3]!;
    check("the mark is DRAWN ON - partway through it has a fraction of the ink it ends with",
        inkFinal > 400 && inkEarly < inkFinal * 0.5,
        `ink over the stroke: ${inkTrace.filter((_, i) => i % 4 === 0).join(", ")} -> ${inkFinal}`);

    // ---- 5. THE TURN CHANGES -------------------------------------------
    console.log("\n-- the puck changes hands --");
    let sawCrossAtCentre = false;
    let bestTrayO = 0;
    for (let i = 0; i < 40; i++) {
        t += 15;
        dev.touchLand(false, 0, 0, t);
        const now = dev.fbSnapshot();
        // The announcement: the CENTRE cell's cross, in the NEW player's
        // colour, on both axes. Asked as "is this bluer than the board
        // around it" rather than "does it match the wash byte for byte",
        // because it fades the whole time it is up.
        const cp = channels(landPx(now, cellCx(1), SLAB_Y0 + 8));
        const rp = channels(landPx(now, SLAB_X1 - 8, cellCy(1)));
        if (cp[2] > cp[0] + 12 && rp[2] > rp[0] + 12) sawCrossAtCentre = true;
        const ink = trayInk(now, P_O);
        if (ink > bestTrayO) bestTrayO = ink;
    }
    check("at the turn change the CENTRE cell's cross washes in in the new player's colour, on both axes",
        sawCrossAtCentre);
    check("and the new player's mark arrives in the tray, in their colour AND their shape",
        bestTrayO > 250, `the blue tray mark reached ${bestTrayO} px of ink`);

    fb = dev.fbSnapshot();
    const slabNow = landPx(fb, SLAB_X0 + 3, cellCy(1));
    check("THE WHOLE BOARD has changed temperature - it wears O's grey now, not X's",
        near(slabNow, C_SLAB_O) && !near(slabNow, C_SLAB_X),
        `${describe(slabNow)} (X's would be ${describe(C_SLAB_X)})`);
    check("nothing is placed in reply - the only mark on the board is the one a hand made",
        !dev.fwLogLines().some((l) => l.includes("morpion: mark") && l.includes("player=2")),
        "no blue mark appeared by itself");

    // ---- 6. THE BOARD NEVER MOVES ON ITS OWN ----------------------------
    console.log("\n-- five seconds of nobody touching it --");
    dev.drainLog();
    const beforeIdle = dev.fbSnapshot();
    const censusBefore = census(beforeIdle);
    t = settle(dev, t, 5000);
    const afterIdle = dev.fbSnapshot();
    const idleLog = dev.drainLog();
    check("nothing is placed, nothing is won, no new game is dealt, while nobody is playing",
        !idleLog.some((l) => l.includes("morpion: mark") || l.includes("morpion: win") || l.includes("morpion: new game")),
        idleLog.length ? `firmware said: ${idleLog.map((l) => l.trim()).join(" | ")}` : "the firmware said nothing at all");
    check("every cell holds exactly what it held five seconds ago",
        census(afterIdle) === censusBefore, `${censusBefore} -> ${census(afterIdle)}`);
    const moved = unchangedOnBoard(beforeIdle, afterIdle);
    check("and not one pixel of the BOARD changes either - only the tray, where the mark breathes",
        moved === null, moved ? `landscape pixel (${moved.lx},${moved.ly}) changed` : "0 pixels changed on the board");

    // ---- 7. a taken cell says no, before the release --------------------
    console.log("\n-- a thumb over the cell that is already taken --");
    dev.drainLog();
    t = holdCell(dev, 7, t, 170);
    fb = dev.fbSnapshot();
    const takenBand = landPx(fb, cxOf(7), SLAB_Y0 + 8);
    const takenDisc = landPx(fb, cxOf(7), cyOf(7) + DISC_R - 6);
    check("a taken cell washes GREY, on both the bands and the cell - colour means it will happen, grey means it will not",
        near(takenBand, C_BAND_TAKEN) && near(takenDisc, C_CELL_TAKEN),
        `band ${describe(takenBand)}, cell ${describe(takenDisc)}`);
    t = settle(dev, t, RELEASE_GRACE_MS + 250);
    const ignored = dev.drainLog();
    check("and releasing on it does nothing at all - no mark, and the turn does not change",
        ignored.some((l) => l.includes("ignored")) && !ignored.some((l) => l.includes("morpion: mark")),
        ignored.map((l) => l.trim()).join(" | ") || "(the firmware said nothing)");

    // ---- 8. a game played out to a win ---------------------------------
    // Board so far: X at 7. O to play. Scripted from here rather than
    // searched, so the win is exactly reproducible and so that the
    // screenshot below catches the winning move being CHOSEN rather than
    // some arbitrary mid-game frame: O takes 0 and 4, X takes 1 and 2, and
    // O's next release completes 0-4-8.
    console.log("\n-- played out to a win --");
    dev.drainLog();
    for (const cell of [0, 1, 4, 2]) t = playMove(dev, cell, t);
    check("four more moves went in, alternating, with nothing won yet",
        !dev.fwLogLines().some((l) => l.includes("morpion: win")) &&
        dev.fwLogLines().filter((l) => l.includes("morpion: mark")).length === 4,
        census(dev.fbSnapshot()));

    // The screenshot the owner judges: a game in progress with a cell shown
    // as the candidate. It is also the winning move about to be made.
    t = holdCell(dev, 8, t, 190);
    fb = dev.fbSnapshot();
    await writeScreenshot("morpion-candidate.png", fb,
        "a game in progress with the bottom-right cell shown as the candidate: both bands lit head to foot and side to side, the cell's own disc a deeper wash, a faint O waiting in it, and O's mark in the tray");
    const midCol = landPx(fb, cellCx(2), SLAB_Y0 + 8);
    const midRow = landPx(fb, SLAB_X0 + 8, cellCy(2));
    check("mid-game, the cross still reads over a board that already has marks on it",
        near(midCol, [0xad, 0xff]) && near(midRow, [0xad, 0xff]), `${describe(midCol)} / ${describe(midRow)}`);

    t = settle(dev, t, RELEASE_GRACE_MS + INK_MS + 120);
    const winLine = dev.fwLogLines().findLast((l) => l.includes("morpion: win"));
    check("a game reaches a win", !!winLine, winLine ?? "(no win line)");
    const winner = winLine ? Number(winLine.match(/player=(\d+)/)?.[1] ?? 0) : 0;

    // The strike draws itself through the three, ACROSS THE SLAB between
    // them, which is the one place nothing else this app draws can reach.
    let bestShot = dev.fbSnapshot();
    let bestStrike = -1;
    for (let i = 0; i < 60; i++) {
        t = settle(dev, t, 20);
        const snap = dev.fbSnapshot();
        const onSlab = inkOnSlab(snap, winner);
        if (onSlab > bestStrike) { bestStrike = onSlab; bestShot = snap; }
    }
    check("the three in a line are STRUCK THROUGH - the winner's ink crosses the slab between them",
        bestStrike > 400, `${bestStrike} px of the winner's solid ink on the slab, where only a strike can put it`);
    const winSlab = landPx(bestShot, SLAB_X0 + 3, cellCy(0));
    check("and the whole board wears the WINNER's colour while it does",
        near(winSlab, slabFor(winner), 12), `${describe(winSlab)}, the winner was player ${winner}`);
    check("no mark waits in the tray while the game is over - the screen says a touch means something else now",
        trayInk(bestShot, P_X) + trayInk(bestShot, P_O) === 0,
        `${trayInk(bestShot, P_X) + trayInk(bestShot, P_O)} px of ink in the tray`);
    await writeScreenshot("morpion-won.png", bestShot,
        "a won game: the three in a line struck through with one bowed ink stroke and breathing, on a board wearing the winner's colour - no text anywhere");

    // ---- the board is wiped, and a fresh game is there ------------------
    console.log("\n-- a touch during the celebration, then the wipe --");
    t = settle(dev, t, WIN_SKIP_MS + 100);
    dev.drainLog();
    t = holdCell(dev, 4, t, 80);
    t = settle(dev, t, WIPE_MS + 400);
    const newGame = dev.fwLogLines().findLast((l) => l.includes("morpion: new game"));
    check("the board wipes itself clean and a fresh game starts, with no menu, no button and no text",
        !!newGame, newGame ?? "(no new game line)");
    t = settle(dev, t, HANDOFF_MS + 200);
    fb = dev.fbSnapshot();
    check("the fresh board really is empty - all nine cells back to paper", census(fb) === ".........", census(fb));
    const freshSlab = landPx(fb, SLAB_X0 + 3, cellCy(1));
    check("and X starts again, said by the board's own colour", near(freshSlab, C_SLAB_X), describe(freshSlab));

    // ---- 9. a board that fills up with nobody winning -------------------
    // Scripted, so it is deterministic and reaches a draw in the FIRST
    // game: X at 0,1,5,6,7 and O at 2,3,4,8 is a full board with no line,
    // and no prefix of the order below completes one either.
    console.log("\n-- a board that fills with nobody winning --");
    dev.drainLog();
    const drawOrder = [0, 2, 1, 3, 5, 4, 6, 8];
    for (const cell of drawOrder) t = playMove(dev, cell, t);
    t = holdCell(dev, 7, t, 160);
    t = settle(dev, t, RELEASE_GRACE_MS + INK_MS + 120);
    const drawLog = dev.fwLogLines();
    check("the game ends with nobody having won", drawLog.some((l) => l.includes("morpion: draw")),
        drawLog.filter((l) => l.includes("morpion:")).slice(-3).map((l) => l.trim()).join(" | "));
    check("and nothing was won on the way to it", !drawLog.some((l) => l.includes("morpion: win")));

    // The difference between a draw and a win, with no words available, is
    // the ABSENCE of the celebration. Asserted as that absence, using the
    // same measurement that found the strike: no ink anywhere on the slab,
    // and a board wearing nobody's colour.
    fb = dev.fbSnapshot();
    const drawStrike = inkOnSlab(fb, P_X) + inkOnSlab(fb, P_O);
    const drawSlab = landPx(fb, SLAB_X0 + 3, cellCy(0));
    check("a draw is the celebration NOT happening: no strike anywhere on the board, and it wears nobody's colour",
        drawStrike === 0 && near(drawSlab, C_SLAB_NEUTRAL),
        `${drawStrike} px of ink on the slab, slab ${describe(drawSlab)}`);
    check("all nine cells are full", census(fb).indexOf(".") < 0, census(fb));
    await writeScreenshot("morpion-draw.png", fb,
        "a draw: nine marks, no strike, no breathing, and a board gone neutral grey - the absence of the celebration is the whole difference");

    // Counted since the drain above, so this is the deal that followed THIS
    // draw and not the one that followed the win before it.
    t = settle(dev, t, DRAW_PAUSE_MS + WIPE_MS + 400);
    check("and a drawn game deals again by itself, the same way a won one does",
        dev.fwLogLines().filter((l) => l.includes("morpion: new game")).length >= 1,
        `${dev.fwLogLines().filter((l) => l.includes("morpion: new game")).length} new game(s) dealt since the draw`);

    // ---- 10. the invariants, across every tick above --------------------
    console.log("\n=== invariants across this whole run (EVERY tick, animation frames included) ===");
    console.log(`    ${ticksChecked} ticks checked in total`);
    const byKind = new Map<string, number>();
    for (const v of violations) byKind.set(v.kind, (byKind.get(v.kind) ?? 0) + 1);
    check("every pushed window's row length was a multiple of 8 (decision 0001)",
        (byKind.get("8px-rule") ?? 0) === 0, `${byKind.get("8px-rule") ?? 0} violation(s)`);
    check("no framebuffer pixel ever changed outside that tick's own pushed rectangles (the ink, the hand-off, the strike and the wipe included)",
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
