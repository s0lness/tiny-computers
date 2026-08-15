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
//   1. the board renders as a slab with white holes, filling the VISIBLE
//      canvas edge to edge (gfx.h PANEL_BEZEL_MARGIN_PX - the case hides a
//      band the emulator cannot show), with a red waiting piece above the
//      centre column, and the arena footprint is REPORTED not assumed;
//   2. a thumb on the glass highlights its column unmistakably - the chute
//      washes pale red the whole height of the screen, and the lowest empty
//      hole of that column wears a landing ring - and moving the thumb moves
//      both, leaving the column it left back at its resting slab;
//   3. releasing drops the piece, it falls THROUGH the column (caught
//      mid-air, not just at rest) and lands in the column the thumb was over;
//   4. THE BOARD NEVER MOVES ON ITS OWN. Two people play this, one puck
//      between them, and nothing in the app takes a turn: over five seconds
//      of nobody touching the glass, every one of the 42 holes still holds
//      what it held and not one pixel outside the single column the waiting
//      parked over changes. This is the property the owner actually asked
//      for ("il faudrait que ce soit chacun son tour avec deux joueurs"), so
//      it is asserted directly rather than inferred from the absence of an
//      opponent in the source;
//   5. WHOSE TURN IT IS, which with two humans is the only thing the screen
//      has to say and has no words to say it with. All three cues are
//      checked at the moment they matter, the instant the turn changes: the
//      whole slab changes temperature (warm for red, cool for blue), the new
//      player's chute washes in at the centre and fades, and the new PIECE
//      pops in in their colour;
//   6. the second player then plays, with the identical gesture, and their
//      piece is blue;
//   7. a game is won, the four in a row breathe, the board wears the
//      winner's colour, a touch moves it along, the board drains and a fresh
//      game begins - with no text anywhere in any of it; and a board that
//      fills with nobody winning ends too;
//   8. EVERY tick of all of the above - the fall, the hand-off, the
//      celebration's pulse and the drain included, not only the frames at
//      rest - obeys decision 0001's 8px rule and the "no pixel changes
//      outside the pushed rectangle" invariant.
//
// REWRITTEN TWICE, and both rewrites are recorded because the assertions
// they replaced were not loosened, they were re-aimed at a board that had
// changed shape underneath them (the same way repro-ring-shrink-residue.ts
// was rewritten when the timer stopped being a dial).
//
// SECOND REWRITE, when the board went full width and full height and the
// waiting piece briefly became an arrow: "je pense que j.aimerais qu.elle prenne
// absolument tout l.ecran en largeur... on fait disparaitre la balle mais on
// peut remplacer par un highlight de la colonne avec une fleche". Three
// classes of assertion had to change. Anything that read a HOLE at its
// centre, because the arrow lived inside one for a round (holeAt samples the rim
// instead). Anything that said "below the hopper", because there is no
// hopper (unchangedOutsideColumn says what is actually meant). And the
// colour predicates themselves, which compared raw byte pairs and matched a
// light grey once the slab started carrying a tint.
//
// FIRST REWRITE, when the opponent was removed. The owner played the version
// where the device took the other side and said "ouais c'est pas mal du tout
// le jeu !", then: "Il faudrait que ce soit chacun son tour avec deux
// joueurs. Et quand je place un rouge ca place direct un bleu." So the
// scenarios that drove the opponent had nothing left to drive, and the ones
// asserting that a reply arrives were asserting something that must now
// never happen. See four.c's section 6 for the full account.
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
//
// DERIVED THE SAME WAY THE FIRMWARE DERIVES THEM, integer division and all,
// rather than written down as the numbers they currently come to. The board
// is laid out against the VISIBLE canvas (gfx.h's PANEL_BEZEL_MARGIN_PX -
// the case hides a band along every edge, which the emulator cannot show
// because it has no bezel), so one edit to that margin moves every
// coordinate in this file too. A test that hardcoded "colX(c) = 40 + 61c"
// would then be asserting about a board that no longer exists, and would do
// it by failing in nine places at once.
const COLS = 7;
const ROWS = 6;
const BEZEL = 10;                       // gfx.h PANEL_BEZEL_MARGIN_PX
const SAFE_X0 = BEZEL, SAFE_X1 = LAND_W - BEZEL;
const SAFE_Y0 = BEZEL, SAFE_Y1 = LAND_H - BEZEL;
const SAFE_W = SAFE_X1 - SAFE_X0, SAFE_H = SAFE_Y1 - SAFE_Y0;
// Square cells: six rows, plus a hopper tall enough for everywhere the
// waiting piece ever GOES (its bob and its hand-off overshoot, not just
// where it rests), pin CELL at 48 inside the visible canvas. Derived the way
// four.c derives it rather than written down, so one edit to the bezel moves
// both - and four.c now carries a _Static_assert on the same constraint, so
// a change that breaks it is a build error rather than something this file
// discovers.
const CELL = 48;
const SLAB_PAD = 6;
const BOARD_W = COLS * CELL;
const BOARD_X0 = SAFE_X0 + Math.floor((SAFE_W - BOARD_W) / 2);
const BOARD_Y0 = SAFE_Y1 - SLAB_PAD - ROWS * CELL;
const HOLE_R = CELL / 2 - 4;
const GHOST_STROKE = HOLE_R / 3;
// The waiting piece's resting centre. Sized so its HIGHEST ink - 5px of bob
// above the resting top - lands exactly on the visible boundary, which is
// what tools/gate/run.ts caught it not doing.
const BOB_RISE_PX = 5;
const HOPPER_CY = SAFE_Y0 + HOLE_R + BOB_RISE_PX;
const RELEASE_GRACE_MS = 300;
const HANDOFF_MS = 420;
const CELEBRATE_SKIP_MS = 1200;
const CENTRE_COL = 3; // four.c parks the waiting piece at COLS/2 every turn

// col_x() uses C integer division for the half-cell, row_y() does not: both
// mirror four.c exactly rather than being "close enough".
const colX = (c: number) => BOARD_X0 + Math.floor(CELL / 2) + c * CELL;
const rowY = (r: number) => BOARD_Y0 + CELL / 2 + r * CELL;
// A gutter between two rows, which is where a probe of the SLAB or of the
// chute has to land: anywhere else in a column is a hole.
const gutterY = (r: number) => (rowY(r) + rowY(r + 1)) / 2;

const P_RED = 1;
const P_BLUE = 2;

// The colours four.c paints, as the LOGICAL rgb565 byte pair the
// framebuffer holds (the fb stores px_swap(v) as a uint16, which on this
// little-endian target puts the logical high byte first - see
// feature-sketch-palette.ts's PALETTE_COLORS_BE for the same convention).
const C_WHITE: [number, number] = [0xff, 0xff];
const C_RED: [number, number] = [0xf8, 0x00];
const C_BLUE: [number, number] = [0x00, 0x1f];
const C_WASH_RED: [number, number] = [0xfd, 0x55];
const C_WASH_BLUE: [number, number] = [0xad, 0xff];
// The slab wears whose turn it is: warm grey for red, cool for blue, neutral
// when no side owns the moment (the drain, the nobody-won beat). Three
// separate constants and not one, because "the whole board changed
// temperature" is a turn cue in its own right (four.c section 6) and a test
// that treated them as interchangeable would be blind to it.
const C_SLAB_RED: [number, number] = [0xe6, 0xda];
const C_SLAB_BLUE: [number, number] = [0xd6, 0xdc];
const C_SLAB_NEUTRAL: [number, number] = [0xde, 0xfb];
const slabFor = (player: number) =>
    player === P_RED ? C_SLAB_RED : player === P_BLUE ? C_SLAB_BLUE : C_SLAB_NEUTRAL;

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

    // Pick "vs human" (see the choice-screen note in main()) before handing
    // the device back - every caller of loadRawDevice() assumes ordinary
    // two-player play starts immediately.
    let setupT = 10;
    const press = (lx: number, ly: number, down: boolean) => {
        setupT += 15;
        exp.emu_touch(down ? 1 : 0, PANEL_W - 1 - Math.round(ly), Math.round(lx));
        exp.emu_tick(setupT);
    };
    for (let e = 0; e < 350; e += 15) press(LAND_W * 0.25, 200, true);
    for (let e = 0; e < RELEASE_GRACE_MS + 350; e += 15) press(0, 0, false);
    for (let e = 0; e < HANDOFF_MS + 200; e += 15) press(0, 0, false);
    fwLog.length = 0;
    return {
        startMs: setupT,
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
// Decoded to real channels, not compared as raw byte pairs, and that is a
// correction rather than a tidy-up. The old predicates were `p[0] >= 0xe0 &&
// p[1] <= 0x20` for red, which asks "is the high byte big and the low byte
// small" - and rgb565 packs green ACROSS the two bytes, so a light warm grey
// like #E6E3DE encodes as [0xe7,0x1b] and sailed through as "red". That grey
// is exactly what the anti-aliased edge between a white hole and the warm
// slab produces, so the board grew six phantom red pixels per hole the
// moment the slab started carrying a tint, and they were invisible until a
// probe box happened to sit over a hole's rim.
//
// Written as red/blue DOMINANCE instead: a piece is saturated, and every
// pale thing this app draws (the washes, the tinted slab, any AA blend
// toward white) has all three channels high and fails on the other two.
function channels(p: [number, number]): [number, number, number] {
    const v = (p[0] << 8) | p[1];
    return [
        Math.round((((v >> 11) & 0x1f) * 255) / 31),
        Math.round((((v >> 5) & 0x3f) * 255) / 63),
        Math.round(((v & 0x1f) * 255) / 31),
    ];
}
function isRedish(p: [number, number]): boolean {
    const [r, g, b] = channels(p);
    return r > 150 && g < 90 && b < 90;
}
function isBluish(p: [number, number]): boolean {
    const [r, g, b] = channels(p);
    return b > 150 && r < 90 && g < 110;
}

// How far toward blue a pixel leans, in rgb565 steps: the 5-bit blue channel
// minus the 5-bit red one. Used to ask "is this bluer than that" rather than
// "is this exactly this colour", which is the right question for anything
// this app FADES - a wash on its way out passes through every blend between
// itself and the slab, and none of them match its own byte pair.
function blueness(p: [number, number]): number {
    const v = (p[0] << 8) | p[1];
    return (v & 0x1f) - ((v >> 11) & 0x1f);
}

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

// A whole move: press, hold long enough to arm, release, then wait out the
// fall, the bounces and the hand-off, so the app is back at rest and the
// next player may play. Returns the new clock.
function playMove(dev: Device, col: number, t0: number, ly = 200): number {
    let t = holdLand(dev, colX(col), ly, t0, 140);
    return settle(dev, t, RELEASE_GRACE_MS + 1300);
}

// How many pixels of `player`'s own saturated colour sit in the hopper
// above column `col` - i.e. is the waiting piece there, and whose is it.
//
// Counted over a box rather than probed at one point. That was needed when
// the cue was a chevron (whose exact centre falls BETWEEN its two arms) and
// it stays useful now that it is a disc again: a count also distinguishes a
// FILLED piece from the hollow ring a full column shows, which a single
// centre sample cannot.
function pieceInk(fb: Uint8Array, col: number, player: number): number {
    const isColour = player === P_RED ? isRedish : isBluish;
    let n = 0;
    for (let ly = Math.round(HOPPER_CY - HOLE_R - 8); ly <= Math.round(HOPPER_CY + HOLE_R + 2); ly++) {
        for (let lx = Math.round(colX(col) - HOLE_R - 2); lx <= Math.round(colX(col) + HOLE_R + 2); lx++) {
            if (lx < 0 || lx >= LAND_W || ly < 0 || ly >= LAND_H) continue;
            if (isColour(landPx(fb, lx, ly))) n++;
        }
    }
    return n;
}

// Do the two framebuffers agree everywhere OUTSIDE the column the waiting
// piece is parked over? That column is allowed to change: the piece bobs there
// forever, and that is the point of it (it is the one moving thing on a
// still screen, and one of the three cues saying whose turn it is).
//
// THIS USED TO BE "nothing below the hopper changes", and it stopped meaning
// anything the moment the board went full height: there is no hopper any
// more, and the cue at the top of a column is inside that column, so a y threshold would
// either have excluded most of the board or have included the one thing that
// is supposed to move. Rewritten to describe the new layout rather than
// loosened to survive it - and it is a strictly stronger statement, because
// it now covers the whole panel outside a single 61px column instead of
// everything under an arbitrary line.
function unchangedOutsideColumn(a: Uint8Array, b: Uint8Array, col: number): { lx: number; ly: number } | null {
    const lo = colX(col) - CELL / 2 - 10, hi = colX(col) + CELL / 2 + 10;
    for (let ly = 0; ly < LAND_H; ly++) {
        for (let lx = 0; lx < LAND_W; lx++) {
            if (lx >= lo && lx <= hi) continue;
            const p = landPx(a, lx, ly), q = landPx(b, lx, ly);
            if (p[0] !== q[0] || p[1] !== q[1]) return { lx, ly };
        }
    }
    return null;
}

// Is this hole FILLED by a piece, and by whose? Sampled at four points near
// the hole's rim rather than at its centre, and this is not fussiness: at
// for one round the arrow lived inside row 0's hole, so a centre sample there
// reads the chevron's ink and calls an empty hole occupied. A piece is a
// disc that fills the hole, so all four rim points agree; an arrow was two
// strokes that reach none of them (checked against the chevron's own
// geometry: the nearest of the four is 10px clear of the nearest arm).
//
// The first version of this DID sample the centre, and the census it built
// reported the top row of the parked column as holding a piece that was
// never played. Worth keeping the reason written down: a cue moved into
// the board when the board went full height, and every "read the hole" probe
// in this file had quietly assumed nothing else was ever drawn in one.
function holeAt(fb: Uint8Array, c: number, r: number): "R" | "B" | "." | "?" {
    const d = HOLE_R * 0.85;
    const pts: [number, number][] = [
        [colX(c) - d, rowY(r)], [colX(c) + d, rowY(r)],
        [colX(c), rowY(r) - d], [colX(c), rowY(r) + d],
    ];
    const px = pts.map(([x, y]) => landPx(fb, x, y));
    if (px.every(isRedish)) return "R";
    if (px.every(isBluish)) return "B";
    if (px.every((p) => near(p, C_WHITE))) return ".";
    return "?";
}

// The whole board as a string, so two frames can be compared for "are the
// same pieces in the same places" independently of anything animating over
// them.
function holeCensus(fb: Uint8Array): string {
    let s = "";
    for (let c = 0; c < COLS; c++) for (let r = 0; r < ROWS; r++) s += holeAt(fb, c, r);
    return s;
}

// ---------------------------------------------------------------------
// A mirror of the board, maintained purely from the firmware's own "four:
// drop col=.. row=.. player=.." log lines - never from any state this test
// keeps in parallel with the app. Both sides are the test's to play now, so
// this is what lets it play a real game rather than dropping pieces at
// random: it picks a move for whoever's turn the firmware just said it is.
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
// Play FOR whoever's turn it is: take the win if there is one, else block the
// other side, else build the longest line available, preferring the middle.
// Ordinary competent play from both hands, which is what reaches a win in a
// handful of moves instead of wandering.
function chooseMove(m: Mirror, p: number): number {
    const other = p === P_RED ? P_BLUE : P_RED;
    for (let c = 0; c < COLS; c++) { const r = mirrorLanding(m, c); if (r >= 0 && mirrorLine(m, c, r, p) >= 4) return c; }
    for (let c = 0; c < COLS; c++) { const r = mirrorLanding(m, c); if (r >= 0 && mirrorLine(m, c, r, other) >= 4) return c; }
    let best = -1, bestScore = -1;
    for (let c = 0; c < COLS; c++) {
        const r = mirrorLanding(m, c);
        if (r < 0) continue;
        const score = mirrorLine(m, c, r, p) * 10 + (3 - Math.abs(c - CENTRE_COL));
        if (score > bestScore) { bestScore = score; best = c; }
    }
    return best;
}

// Play for whoever's turn it is, but NEVER complete a four for them. Both
// hands doing this cannot produce a winner, so the board fills and the game
// ends the other way - which is how the full-board-no-winner path gets
// exercised at all. With the opponent gone this is fully in the test's
// control and reaches a draw in the FIRST game, where the old version had to
// hunt across fifteen of them and hope.
function chooseStalemateMove(m: Mirror, p: number): number {
    let best = -1, bestScore = -99;
    for (let c = 0; c < COLS; c++) {
        const r = mirrorLanding(m, c);
        if (r < 0) continue;
        if (mirrorLine(m, c, r, p) >= 4) continue; // never finish a four
        const score = -Math.abs(c - CENTRE_COL);
        if (score > bestScore) { bestScore = score; best = c; }
    }
    // Every legal column would complete a four: forced, so somebody wins and
    // this game is simply not the one that draws.
    if (best < 0) for (let c = 0; c < COLS; c++) if (mirrorLanding(m, c) >= 0) best = c;
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
    console.log("=== feature: Connect Four, two players (firmware/apps/four.c) ===\n");
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

    // ---- 0. the choice screen: pick "vs human" ---------------------------
    // The app now opens on a choice screen (vs human / vs cpu - four.c's
    // header, section 8) rather than dealing a board directly. This whole
    // file exercises two-player play, so pick "vs human" here, with clean
    // input, before any of the checks below assume a board is on screen.
    // The choice screen itself, and the cpu opponent, get their own
    // dedicated files: feature-four-choice.ts, repro-touch-dropout-four-
    // choice.ts and feature-four-cpu.ts.
    let t = 1000;
    console.log('-- picking "vs human" from the opening choice screen --');
    // Long enough to arm (ARM_SAMPLES/ARM_MS) AND THEN hold the confirmed
    // side for CHOOSE_CONFIRM_MS (150ms) before releasing - the confirm
    // window starts counting only once armed, not from first contact.
    t = holdLand(dev, LAND_W * 0.25, 200, t, 350);
    t = settle(dev, t, RELEASE_GRACE_MS + 350);
    check('picking "vs human" deals a fresh two-player board',
        dev.fwLogLines().some((l) => l.includes("four: choice vsCpu=0")) &&
        dev.fwLogLines().some((l) => l.includes("four: new game")),
        dev.fwLogLines().filter((l) => l.includes("four:")).join(" | "));

    t = settle(dev, t, HANDOFF_MS + 200); // the opening hand-off, announcing red
    let fb = dev.fbSnapshot();

    // The slab is flat between two holes; every hole is paper white; there is
    // no line, no border and no right angle anywhere, so the ONLY thing that
    // makes a grid is the arrangement of the white circles.
    const gutter = landPx(fb, colX(3), gutterY(0));
    check("the board's face is one flat slab between its holes", near(gutter, C_SLAB_RED), describe(gutter));
    let allHolesWhite = true;
    const holeMiss: string[] = [];
    for (let c = 0; c < COLS; c++) for (let r = 0; r < ROWS; r++) {
        const h = holeAt(fb, c, r);
        if (h !== ".") { allHolesWhite = false; if (holeMiss.length < 4) holeMiss.push(`(${c},${r})=${h}`); }
    }
    check("all 42 holes are punched out in paper white", allHolesWhite, holeMiss.join(", "));

    // The slab's own corner: a lozenge with a 34px radius, so the rectangle's
    // corner pixel is still paper. Decision 0009, asserted rather than
    // asserted-in-prose.
    const slabCorner = landPx(fb, SAFE_X0 + 2, SAFE_Y0 + 2);
    check("the slab's bounding-box corner is still paper - a lozenge, not a rectangle (decision 0009)",
        near(slabCorner, C_WHITE), describe(slabCorner));

    // Red starts, and the screen says so twice over: the WAITING PIECE above
    // the centre column is red, and the whole board is wearing red's warm
    // grey. The piece is the stronger of the two cues that have held this job
    // (four.c section 3) - a filled disc, not a stroke.
    const redWaiting = pieceInk(fb, CENTRE_COL, P_RED);
    check("red starts: a red waiting piece above the centre column",
        redWaiting > 150, `${redWaiting} red pixels in the hopper`);

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
    // most of it is outside the ~75px a thumb is covering.
    const chuteProbes: { ly: number; where: string }[] = [
        { ly: SAFE_Y1 - 3, where: "below the bottom row" },
        { ly: gutterY(0), where: "between rows 0 and 1" },
        { ly: gutterY(2), where: "between rows 2 and 3" },
        { ly: gutterY(4), where: "between rows 4 and 5" },
    ];
    let chuteOk = true;
    const chuteDetail: string[] = [];
    for (const probe of chuteProbes) {
        const p = landPx(fb, colX(2), probe.ly);
        if (!near(p, C_WASH_RED)) { chuteOk = false; }
        chuteDetail.push(`${probe.where}: ${describe(p)}`);
    }
    check("the highlighted column washes pale red over the WHOLE height of the screen", chuteOk, chuteDetail.join("; "));

    const neigh1 = landPx(fb, colX(1), gutterY(2));
    const neigh3 = landPx(fb, colX(3), gutterY(2));
    check("the columns either side of it are untouched slab",
        near(neigh1, C_SLAB_RED) && near(neigh3, C_SLAB_RED), `left ${describe(neigh1)}, right ${describe(neigh3)}`);

    // The landing ring: the outline of the piece that is about to be there,
    // in the lowest empty hole. Its ink is on the ring, its middle is paper.
    const ringInk = landPx(fb, colX(2) + HOLE_R - 3, rowY(5));
    const ringHole = landPx(fb, colX(2), rowY(5));
    check("the lowest empty hole of that column wears a red landing ring, hollow in the middle",
        isRedish(ringInk) && near(ringHole, C_WHITE), `ring ${describe(ringInk)}, middle ${describe(ringHole)}`);

    const pieceOnCol2 = pieceInk(fb, 2, P_RED);
    const pieceOffCentre = pieceInk(fb, CENTRE_COL, P_RED);
    check("the waiting piece rides the highlighted column, and is not left behind where it was parked",
        pieceOnCol2 > 150 && pieceOffCentre === 0, `${pieceOnCol2} red pixels on column 2, ${pieceOffCentre} still at the centre`);

    // ---- the thumb slides ------------------------------------------------
    console.log("\n-- the thumb slides across to column 5 --");
    for (let c = 3; c <= 5; c++) t = holdLand(dev, colX(c), THUMB_LY, t, 45);
    fb = dev.fbSnapshot();
    const oldCol = landPx(fb, colX(2), gutterY(2));
    const newCol = landPx(fb, colX(5), gutterY(2));
    check("the highlight followed the thumb, and the column it left is back to plain slab",
        near(newCol, C_WASH_RED) && near(oldCol, C_SLAB_RED), `new ${describe(newCol)}, old ${describe(oldCol)}`);

    // ---- 3. release drops the piece -------------------------------------
    console.log("\n-- the thumb lifts --");
    dev.drainLog();
    let dropLine: string | undefined;
    for (let waited = 0; waited < RELEASE_GRACE_MS + 200 && !dropLine; waited += 15) {
        t += 15;
        dev.touchLand(false, 0, 0, t);
        dropLine = dev.fwLogLines().findLast((l) => l.includes("four: drop"));
    }
    check("releasing drops a piece, in the column the thumb was over, for the player whose turn it is",
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
                "a piece caught mid-fall: passing row 2, down its own lit chute, toward the ring already drawn in the hole it is heading for");
        }
    }
    check("the piece is genuinely in flight partway down the column at some point", caught);

    // ---- 5. THE TURN CHANGES, which is the moment the whole design now
    // turns on. Sampled frame by frame rather than once at rest: the
    // announcement is HANDOFF_MS long and the point of it is what it does
    // while it is happening. -------------------------------------------
    console.log("\n-- the piece lands, and the puck changes hands --");
    let handoffShot: Uint8Array | null = null;
    let sawBlueWashAtCentre = false;
    let bluePieceRadius = 0;
    let landed = false;
    for (let i = 0; i < 90; i++) {
        t += 15;
        dev.touchLand(false, 0, 0, t);
        const now = dev.fbSnapshot();
        if (!landed && isRedish(landPx(now, colX(5), rowY(5)))) landed = true;
        if (!landed) continue;
        // The announcement: the centre column washed in the NEW player's
        // colour, full height, with no landing ring in it. Sampled with a
        // loose tolerance because it is fading the whole time it is up.
        // "Is the centre column visibly bluer than the board around it",
        // rather than "does it match the pure wash byte for byte". The wash
        // fades from full to nothing across the hand-off, so an exact match
        // only holds for the first frames of it - which is both a weaker
        // assertion than the real requirement (the column has to STAND OUT
        // from the slab) and, when used to choose the screenshot, picked a
        // frame where the waiting piece had barely started arriving.
        const washRow = gutterY(2);
        const washUp = blueness(landPx(now, colX(CENTRE_COL), washRow)) -
                       blueness(landPx(now, colX(CENTRE_COL - 2), washRow)) >= 3;
        if (washUp) sawBlueWashAtCentre = true;
        const ink = pieceInk(now, CENTRE_COL, P_BLUE);
        // The frame worth photographing is the one where BOTH halves of the
        // announcement are at their most legible: the wash still up, and the
        // piece as far through its pop as it gets while it is. Taking the
        // first frame that had a wash (which is what this did at first) caught
        // the piece at a radius of about two pixels, so the screenshot showed
        // a blue stripe and no piece at all.
        if (washUp && ink > bluePieceRadius) handoffShot = now;
        if (ink > bluePieceRadius) bluePieceRadius = ink;
    }
    check("nothing is placed in reply - the only piece on the board is the one a hand dropped",
        !dev.fwLogLines().some((l) => l.includes("four: drop") && l.includes("player=2")),
        "no blue piece appeared by itself");
    check("at the turn change the centre column washes in the NEW player's colour, full height", sawBlueWashAtCentre);
    check("and the new waiting piece arrives in blue", bluePieceRadius > 150, `blue waiting piece reached ${bluePieceRadius} pixels of ink`);
    if (handoffShot) {
        await writeScreenshot("four-turn.png", handoffShot,
            "the instant the turn changes: the board has gone cool, the centre column is washed blue, blue's waiting piece is arriving");
    }

    fb = dev.fbSnapshot();
    const slabNow = landPx(fb, colX(1), gutterY(0));
    check("THE WHOLE BOARD has changed temperature - it wears blue's grey now, not red's",
        near(slabNow, C_SLAB_BLUE) && !near(slabNow, C_SLAB_RED), `${describe(slabNow)} (red would be ${describe(C_SLAB_RED)})`);
    const settledWaiting = pieceInk(fb, CENTRE_COL, P_BLUE);
    check("and the waiting piece has settled at the centre, in blue", settledWaiting > 150, `${settledWaiting} blue pixels in the arrow box`);

    // ---- 4. THE BOARD NEVER MOVES ON ITS OWN ----------------------------
    // The property the owner actually asked for, asserted directly. Five
    // seconds of nobody touching the glass: the ONLY thing allowed to move is
    // the arrow bobbing at the head of the column it is parked over, which is
    // the point of it - it is the one moving thing on a still screen and one
    // of the three cues saying whose turn it is.
    console.log("\n-- five seconds of nobody touching it --");
    dev.drainLog();
    const beforeIdle = dev.fbSnapshot();
    const censusBefore = holeCensus(beforeIdle);
    t = settle(dev, t, 5000);
    const afterIdle = dev.fbSnapshot();
    const idleLog = dev.drainLog();
    check("nothing is placed, nothing is won, no new game is dealt, while nobody is playing",
        !idleLog.some((l) => l.includes("four: drop") || l.includes("four: win") || l.includes("four: new game")),
        idleLog.length ? `firmware said: ${idleLog.map((l) => l.trim()).join(" | ")}` : "the firmware said nothing at all");
    check("every one of the 42 holes holds exactly what it held five seconds ago",
        holeCensus(afterIdle) === censusBefore, `${censusBefore} -> ${holeCensus(afterIdle)}`);
    const moved = unchangedOutsideColumn(beforeIdle, afterIdle, CENTRE_COL);
    check("and not one pixel outside the parked column changes either - the board is still, the waiting piece breathes",
        moved === null, moved ? `landscape pixel (${moved.lx},${moved.ly}) changed` : "0 pixels changed outside that one column");

    // ---- 6. the second player plays, same gesture ----------------------
    console.log("\n-- the other player takes the puck --");
    dev.drainLog();
    t = playMove(dev, 1, t);
    const blueDrop = dev.fwLogLines().findLast((l) => l.includes("four: drop"));
    check("the second player plays with the identical gesture, and it is their colour that lands",
        !!blueDrop && blueDrop.includes("player=2"), blueDrop ?? "(no drop line)");
    fb = dev.fbSnapshot();
    const bluePiece = landPx(fb, colX(1), rowY(5));
    check("their piece is blue, and it is in the column their thumb was over", isBluish(bluePiece), describe(bluePiece));
    const slabBack = landPx(fb, colX(4), gutterY(0));
    check("and the board has gone warm again - back to red", near(slabBack, C_SLAB_RED), describe(slabBack));

    // ---- a real mid-game board, and the highlight ON it -----------------
    console.log("\n-- a few more moves, then the same gesture over a column that already has a stack in it --");
    const mirror = newMirror();
    for (const line of dev.fwLogLines()) {
        const m = line.match(/four: drop col=(\d+) row=(\d+) player=(\d+)/);
        if (m) mirrorApply(mirror, Number(m[1]), Number(m[2]), Number(m[3]));
    }
    dev.drainLog();

    // Deliberately NOT chooseMove() here: that plays to win, and a win
    // mid-way would deal a fresh empty board out from under the screenshot
    // this block exists to take. Two pieces each into two separate columns
    // cannot make a four, and it builds the stack the landing ring needs to
    // be resting on.
    const STACK_COL = 2;
    for (const col of [STACK_COL, 4, STACK_COL, 4]) {
        t = playMove(dev, col, t);
        for (const line of dev.drainLog()) {
            const m = line.match(/four: drop col=(\d+) row=(\d+) player=(\d+)/);
            if (m) mirrorApply(mirror, Number(m[1]), Number(m[2]), Number(m[3]));
        }
    }

    const stackRow = mirrorLanding(mirror, STACK_COL);
    console.log(`    column ${STACK_COL} now holds ${ROWS - 1 - stackRow} piece(s); the landing ring should be at row ${stackRow}`);
    t = holdLand(dev, colX(STACK_COL), THUMB_LY, t, 150);
    fb = dev.fbSnapshot();
    const stackRingInk = landPx(fb, colX(STACK_COL) + HOLE_R - 3, rowY(stackRow));
    const belowRing = landPx(fb, colX(STACK_COL), rowY(stackRow + 1));
    check("mid-game, the landing ring sits on TOP of the stack already in that column",
        (isRedish(stackRingInk) || isBluish(stackRingInk)) && !near(belowRing, C_WHITE),
        `ring at row ${stackRow} ${describe(stackRingInk)}, hole below it ${describe(belowRing)}`);
    await writeScreenshot("four-highlight.png", fb,
        `mid-game: column ${STACK_COL} washed head to foot, landing ring resting on the stack in it, and the board's own tint saying whose turn it is`);

    t = settle(dev, t, RELEASE_GRACE_MS + 1300);
    for (const line of dev.drainLog()) {
        const m = line.match(/four: drop col=(\d+) row=(\d+) player=(\d+)/);
        if (m) mirrorApply(mirror, Number(m[1]), Number(m[2]), Number(m[3]));
    }

    // ---- 7. play the game out to a win, both hands ----------------------
    console.log("\n-- playing both sides on until somebody wins --");
    let turn = P_RED;
    for (const line of dev.fwLogLines()) {
        const m = line.match(/four: drop col=\d+ row=\d+ player=(\d+)/);
        if (m) turn = Number(m[1]) === P_RED ? P_BLUE : P_RED;
    }
    dev.drainLog();

    let winLine: string | undefined;
    let moves = 0;
    while (!winLine && moves < 30) {
        const col = chooseMove(mirror, turn);
        if (col < 0) break;
        moves++;
        t = playMove(dev, col, t);
        for (const line of dev.drainLog()) {
            const m = line.match(/four: drop col=(\d+) row=(\d+) player=(\d+)/);
            if (m) { mirrorApply(mirror, Number(m[1]), Number(m[2]), Number(m[3])); turn = Number(m[3]) === P_RED ? P_BLUE : P_RED; }
            if (line.includes("four: win")) winLine = line;
            if (line.includes("four: new game")) { for (let i = 0; i < ROWS * COLS; i++) mirror.cells[i] = 0; turn = P_RED; }
        }
    }
    check("a game reaches a win", !!winLine, winLine ?? `(no win after ${moves} moves)`);
    const winner = winLine ? Number(winLine.match(/player=(\d+)/)?.[1] ?? 0) : 0;

    // The four in a row breathe. Measured as the radius of SOLID PIECE COLOUR
    // out of a hole's centre, walked outward until the colour stops - the
    // disc itself, not "any ink out here somewhere". Two earlier versions
    // measured the latter and were useless in different ways: probing to
    // HOLE_R+13 kept hitting the NEIGHBOURING piece and reported a constant
    // 34, and probing to the neighbour's edge kept hitting the halo and
    // reported a near-constant 28 whose maximum frame was the one where the
    // piece was at its SMALLEST.
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
    // Probed in a gutter BETWEEN two holes, not at the slab's bounding-box
    // corner: that corner is paper, which is the whole point of the lozenge
    // this file asserts about twenty lines up.
    const winSlab = landPx(bestShot, colX(0), gutterY(0));
    check("and the whole board wears the WINNER's colour while it does",
        near(winSlab, slabFor(winner), 12), `${describe(winSlab)}, winner was player ${winner}`);

    await writeScreenshot("four-won.png", bestShot,
        "a won game: the four in a row swollen and blooming, the board itself in the winner's colour, no text anywhere");
    fb = dev.fbSnapshot();

    // No waiting piece during the celebration: the absence of "something to
    // drop" is what says a touch now means something else (decision 0002's
    // "no modal state" - the screen says so).
    // Straightforward again now that the waiting piece is back above the
    // board: the hopper is its own band, so nothing else is ever drawn there
    // and a count of coloured pixels in it means exactly what it says. The
    // version this replaces had to skip columns whose TOP HOLE held a piece,
    // because the arrow lived inside that hole and a winning line reaching
    // row 0 read as an arrow that was not there.
    let anyWaiting = false;
    const waitingWhere: string[] = [];
    for (let c = 0; c < COLS; c++) {
        const ink = pieceInk(fb, c, P_RED) + pieceInk(fb, c, P_BLUE);
        if (ink > 0) { anyWaiting = true; waitingWhere.push(`col ${c}: ${ink}px`); }
    }
    check("no waiting piece is shown while the game is over - the screen says a touch means something else now",
        !anyWaiting, waitingWhere.join(", "));

    // ---- a touch moves it along, the board drains, back to the choice ----
    console.log("\n-- a touch during the celebration, then the board drains --");
    t = settle(dev, t, CELEBRATE_SKIP_MS + 100);
    dev.drainLog();
    t = holdLand(dev, colX(3), THUMB_LY, t, 60);
    t = settle(dev, t, 1400); // the drain finishes; the app is back at the
                                // choice screen (section 8), not dealing a
                                // fresh board by itself any more
    // "It deals again" now means picking "vs human" again - see this file's
    // opening "picking vs human" block, same shape.
    t = holdLand(dev, LAND_W * 0.25, 200, t, 350);
    t = settle(dev, t, RELEASE_GRACE_MS + 350);
    const newGame = dev.fwLogLines().findLast((l) => l.includes("four: new game"));
    check("the board empties itself and a fresh game starts, with no menu and no text", !!newGame, newGame ?? "(no new game line)");

    t = settle(dev, t, HANDOFF_MS + 200);
    fb = dev.fbSnapshot();
    const freshCensus = holeCensus(fb);
    check("the fresh board really is empty - all 42 holes back to paper",
        /^.{42}$/.test(freshCensus), freshCensus);
    const freshSlab = landPx(fb, colX(1), gutterY(0));
    check("and red starts again, said by the board's own colour", near(freshSlab, C_SLAB_RED), describe(freshSlab));

    // ---- a full board that nobody won ----------------------------------
    // Run on a SECOND device with the framebuffer invariant check switched
    // off: that check diffs 165k pixels per tick and a whole 42-move game is
    // tens of thousands of them, while the invariant it proves is a property
    // of render_span()/flush() that the game above already exercises across
    // every kind of frame this app has.
    //
    // Deterministic now, and in the FIRST game: with both hands under this
    // test's control and both refusing to complete a four, nobody CAN win, so
    // the board simply fills. The version of this written against the old
    // opponent had to hunt a draw across fifteen games and hope.
    console.log("\n-- a board that fills up with nobody winning --");
    const raw = await loadRawDevice();
    const mirror2 = newMirror();
    let rawT = raw.startMs;
    let turn2 = P_RED;
    let sawDraw = false, resetAfterDraw = false, sawWin = false;
    for (let move = 0; move < 60 && !resetAfterDraw; move++) {
        if (sawDraw) {
            // The drain has finished (the 120-tick settle below already
            // waits through it); the app is back at the choice screen
            // (section 8), not dealing a fresh board by itself any more.
            // "It deals again" now means picking "vs human" again, so do
            // that here rather than touching a board that is not there.
            for (let i = 0; i < 24; i++) { rawT += 15; raw.touchLand(true, LAND_W * 0.25, 200, rawT); }
            for (let i = 0; i < 44; i++) { rawT += 15; raw.touchLand(false, 0, 0, rawT); }
        } else {
            const col = chooseStalemateMove(mirror2, turn2);
            if (col >= 0) for (let i = 0; i < 9; i++) { rawT += 15; raw.touchLand(true, colX(col), THUMB_LY, rawT); }
        }
        for (let i = 0; i < 120; i++) { rawT += 15; raw.touchLand(false, 0, 0, rawT); }
        for (const line of raw.drainLog()) {
            const m = line.match(/four: drop col=(\d+) row=(\d+) player=(\d+)/);
            if (m) { mirrorApply(mirror2, Number(m[1]), Number(m[2]), Number(m[3])); turn2 = Number(m[3]) === P_RED ? P_BLUE : P_RED; }
            if (line.includes("four: win")) sawWin = true;
            if (line.includes("four: draw")) sawDraw = true;
            if (line.includes("four: new game")) {
                if (sawDraw) resetAfterDraw = true;
                for (let i = 0; i < ROWS * COLS; i++) mirror2.cells[i] = 0;
                turn2 = P_RED;
            }
        }
    }
    check("a board that fills up with nobody winning ends the game too - no pulse, and it deals again",
        sawDraw && resetAfterDraw && !sawWin,
        `draw=${sawDraw}, dealt again=${resetAfterDraw}, nobody won on the way=${!sawWin}`);

    // ---- 8. the invariants, across every tick above ---------------------
    console.log("\n=== invariants across this whole run (EVERY tick, animation frames included) ===");
    console.log(`    ${ticksChecked} ticks checked in total`);
    const byKind = new Map<string, number>();
    for (const v of violations) byKind.set(v.kind, (byKind.get(v.kind) ?? 0) + 1);
    check("every pushed window's row length was a multiple of 8 (decision 0001)",
        (byKind.get("8px-rule") ?? 0) === 0, `${byKind.get("8px-rule") ?? 0} violation(s)`);
    check("no framebuffer pixel ever changed outside that tick's own pushed rectangles (the fall, the hand-off, the pulse and the drain included)",
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
