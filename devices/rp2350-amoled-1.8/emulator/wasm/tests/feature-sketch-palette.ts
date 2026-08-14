// feature-sketch-palette: headless verification of the sketchpad's
// EXPERIMENTAL colour palette (firmware/apps/sketch.c, the section headed
// "EXPERIMENTAL: a colour palette, opened by holding the pen still").
//
// This is not a bug reproduction like this directory's other repro-*.ts
// files - it is the standing check for a new, judged-in-the-emulator-only
// feature: a long touch held in one place opens four colour squares away
// from the fingertip; touching one picks that colour for the next stroke;
// lifting without choosing leaves the drawing untouched. Run with (after
// `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/feature-sketch-palette.ts
//
// Loads the REAL firmware compiled to wasm, same shape as every other file
// in this directory (decision 0003) - nothing here reimplements sketch.c's
// logic; every assertion reads the framebuffer or the push-window list the
// firmware itself produced.
//
// THREE THINGS THIS FILE PROVES, IN ORDER:
//   1. the palette actually opens on a held-still touch, and leaves the
//      four expected flat colours on screen, clear of an existing drawing;
//   2. picking a colour (a continuing drag onto a square, then a lift over
//      it) actually changes what the next stroke draws in;
//   3. dismissing the palette - by lifting outside every square - restores
//      the framebuffer to BYTE-IDENTICAL what it was before that palette
//      opened, and every pixel this whole run ever changed sat inside that
//      tick's own pushed rectangle (docs/decisions/0001's 8px rule, and the
//      "no pixel outside the push" invariant repro-timer-coil.ts already
//      established for the rest of this firmware).
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

// ---- mirrors of sketch.c's own constants (HOLD_*/LONG_PRESS_MS/PALETTE_*)
// - lifted, not re-derived, same convention every other repro test in this
// directory uses for the app it drives (e.g. repro-ring-shrink-residue.ts's
// RING_CX/CY, repro-timer-coil.ts's RING geometry). If sketch.c's own
// numbers change, this file's own comments point back at the #define this
// mirrors so the two stay easy to keep in sync by hand.
const CONFIRM_MS = 40; // sketch.c CONFIRM_MS_DEFAULT
const LONG_PRESS_MS = 550; // sketch.c LONG_PRESS_MS
const LIFT_DEBOUNCE_MS = 220; // sketch.c LIFT_DEBOUNCE_MS_DEFAULT
const PALETTE_LIFT_GRACE_MS = 80; // sketch.c PALETTE_LIFT_GRACE_MS
const PALETTE_SQUARE_PX = 82; // sketch.c PALETTE_SQUARE_PX
const PALETTE_GAP_PX = 6; // sketch.c PALETTE_GAP_PX
const PALETTE_COUNT = 4; // sketch.c PALETTE_COUNT
const PALETTE_W = PALETTE_COUNT * PALETTE_SQUARE_PX + (PALETTE_COUNT - 1) * PALETTE_GAP_PX; // 346
const PALETTE_H = PALETTE_SQUARE_PX; // 82
const PALETTE_TOUCH_GAP_PX = 50; // sketch.c PALETTE_TOUCH_GAP_PX

// Mirror of sketch.c's open_palette() placement math: horizontally centred
// always, vertically PALETTE_TOUCH_GAP_PX clear of the touch point on
// whichever side (above/below) has more room, a tie going below.
function paletteRectFor(touchY: number): { x0: number; y0: number; w: number; h: number } {
    const x0 = Math.floor((PANEL_W - PALETTE_W) / 2);
    const spaceAbove = touchY;
    const spaceBelow = PANEL_H - 1 - touchY;
    let y0: number;
    if (spaceBelow >= spaceAbove) {
        y0 = touchY + PALETTE_TOUCH_GAP_PX;
        if (y0 + PALETTE_H - 1 > PANEL_H - 1) y0 = PANEL_H - PALETTE_H;
    } else {
        y0 = touchY - PALETTE_TOUCH_GAP_PX - PALETTE_H;
        if (y0 < 0) y0 = 0;
    }
    return { x0, y0, w: PALETTE_W, h: PALETTE_H };
}

// Mirror of sketch.c's palette_color(): the four swapped-RGB565 byte pairs
// AMOLED_1IN8_DisplayWindows actually receives, in the framebuffer's own
// big-endian storage order (emu_abi.h: panel.format "rgb565be") - see this
// file's decodeRgb565Be() for the read side of the same convention.
const PALETTE_COLORS_BE: [number, number][] = [
    [0x00, 0x00], // black
    [0xf8, 0x00], // red
    [0x00, 0x1f], // blue
    [0xff, 0xe0], // yellow
];
const COLOR_NAMES = ["black", "red", "blue", "yellow"];

function squareCenter(x0: number, y0: number, col: number): [number, number] {
    const sx = x0 + col * (PALETTE_SQUARE_PX + PALETTE_GAP_PX) + PALETTE_SQUARE_PX / 2;
    const sy = y0 + PALETTE_SQUARE_PX / 2;
    return [Math.round(sx), Math.round(sy)];
}

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
        // repro-timer-coil.ts's tickChecked(), applied here to the palette
        // instead of the timer's coil.
        tickChecked(nowMs: number) {
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
// sketch.c's wall-clock checks (CONFIRM_MS's persisted branch, and this
// feature's own hold-candidacy check) actually get re-evaluated - a single
// touch() call followed by many idle tick()s would NOT do this: the drain
// loop only re-checks time when a new sample is actually in the queue (see
// repro-touch-dropout-stroke-start.ts's scenario D for the same technique,
// applied there to the lift debounce instead of a hold).
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
    console.log("=== feature: sketchpad colour palette (long-press to open) ===\n");
    const dev = await loadDevice();
    dev.tickChecked(0);
    dev.appSwitch(APP_DRAW);
    dev.tickChecked(1000);
    check("switched into the sketchpad", dev.appCurrent() === APP_DRAW, `app_current()=${dev.appCurrent()}`);

    // ---- an existing drawing, in the device's own default black ink,
    // clear of where every palette rect used below will land (see
    // paletteRectFor()'s own placement rule - it always sits in y=[208,289]
    // for the touch points this file uses, since PALETTE_TOUCH_GAP_PX
    // placement is driven by touchY alone). --------------------------------
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

    // ---- scenario 1: hold still in an empty spot; the palette should
    // open, clear of the stroke above, with the four expected flat
    // colours. -------------------------------------------------------------
    console.log("\n-- long press in an empty spot (280,340), held past CONFIRM_MS+LONG_PRESS_MS --");
    const holdX = 280, holdY = 340;
    t = holdStill(dev, holdX, holdY, t + 300, CONFIRM_MS + LONG_PRESS_MS + 250);

    const rect1 = paletteRectFor(holdY);
    const fbOpen = dev.fbSnapshot();
    let allSquaresMatch = true;
    const mismatches: string[] = [];
    for (let col = 0; col < PALETTE_COUNT; col++) {
        const [cx, cy] = squareCenter(rect1.x0, rect1.y0, col);
        const idx = (cy * PANEL_W + cx) * 2;
        const [wantHi, wantLo] = PALETTE_COLORS_BE[col]!;
        if (fbOpen[idx] !== wantHi || fbOpen[idx + 1] !== wantLo) {
            allSquaresMatch = false;
            mismatches.push(`${COLOR_NAMES[col]} square at (${cx},${cy}): got [${fbOpen[idx]!.toString(16)},${fbOpen[idx + 1]!.toString(16)}], want [${wantHi.toString(16)},${wantLo.toString(16)}]`);
        }
    }
    check("the palette opened with all four expected flat colours, in order", allSquaresMatch, mismatches.join("; "));

    // The palette must sit clear of the touch point itself - a fingertip
    // (~75px) resting at (holdX,holdY) must not cover any square.
    const touchClear = !(holdX >= rect1.x0 - 37 && holdX <= rect1.x0 + rect1.w + 37 &&
                          holdY >= rect1.y0 - 37 && holdY <= rect1.y0 + rect1.h + 37);
    check("the palette panel sits clear of the touch point's own fingertip footprint",
        touchClear, `touch=(${holdX},${holdY}), palette rect=(${rect1.x0},${rect1.y0},${rect1.w},${rect1.h})`);

    await writeScreenshot("palette-open.png", fbOpen);
    console.log("    screenshot: the palette open over the existing black stroke");

    // ---- scenario 2: drag the still-down touch onto the red square and
    // release there - picks red without lifting first. ---------------------
    console.log("\n-- dragging onto the red square and releasing over it --");
    const [redX, redY] = squareCenter(rect1.x0, rect1.y0, 1);
    dev.touchChecked(true, redX, redY, (t += 15));
    dev.touchChecked(true, redX, redY, (t += 15));
    t = releaseAndWait(dev, t, PALETTE_LIFT_GRACE_MS + 100);

    const pickedLine = dev.fwLogLines().find((l) => l.includes("palette: picked"));
    check("the firmware logged a colour pick, not a cancel", !!pickedLine, pickedLine ?? "(no palette log line seen)");

    const fbAfterPick = dev.fbSnapshot();
    let paletteGoneAfterPick = true;
    for (let py = rect1.y0; py < rect1.y0 + rect1.h; py++) {
        for (let px = rect1.x0; px < rect1.x0 + rect1.w; px++) {
            const idx = (py * PANEL_W + px) * 2;
            if (fbAfterPick[idx] !== 0xff || fbAfterPick[idx + 1] !== 0xff) { paletteGoneAfterPick = false; break; }
        }
        if (!paletteGoneAfterPick) break;
    }
    check("the palette panel itself is gone (restored to white) once a colour is picked", paletteGoneAfterPick);

    // Settle well past LIFT_DEBOUNCE_MS/pendingStart windows before the next
    // fresh touch, same reasoning drawStroke()'s own trailing releaseAndWait
    // uses - this touch already released above, but a healthy margin before
    // the next gesture starts avoids any ambiguity with dropout-grace state.
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
    console.log("    screenshot: a stroke drawn in the chosen colour");

    // ---- scenario 3: open the palette again, then lift OUTSIDE every
    // square (the original hold point, which is clear of the panel by
    // construction) - the cancel gesture. Must restore the framebuffer to
    // BYTE-IDENTICAL what it was immediately before this second palette
    // opened. --------------------------------------------------------------
    console.log("\n-- long press again, then lift outside every square (cancel) --");
    const preSecondOpen = dev.fbSnapshot();

    const holdX2 = 100, holdY2 = 340;
    t = holdStill(dev, holdX2, holdY2, t + 300, CONFIRM_MS + LONG_PRESS_MS + 250);

    const rect2 = paletteRectFor(holdY2);
    const fbOpen2 = dev.fbSnapshot();
    // Probed via the red square (index 1), not the black one: a black
    // square on a white backdrop is indistinguishable from "nothing opened"
    // by colour alone, where red is unambiguous.
    const [rx2, ry2] = squareCenter(rect2.x0, rect2.y0, 1);
    const idx2 = (ry2 * PANEL_W + rx2) * 2;
    const paletteVisible2 = fbOpen2[idx2] === PALETTE_COLORS_BE[1]![0] && fbOpen2[idx2 + 1] === PALETTE_COLORS_BE[1]![1];
    check("the palette opened a second time", paletteVisible2);

    // Lift exactly where the hold started - never moved, so it was never
    // over a square (scenario 1's own "touch point clear of the palette"
    // check already established this holds for any touch this file uses).
    dev.touchChecked(true, holdX2, holdY2, (t += 15)); // one more sample at the same spot
    t = releaseAndWait(dev, t, PALETTE_LIFT_GRACE_MS + 100);

    const cancelLine = dev.fwLogLines().find((l) => l.includes("palette: cancelled"));
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
        "dismissing the palette restores EXACTLY the framebuffer from before it opened (byte-identical)",
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

    console.log("\n=== invariants across this whole run: 8px rule + no pixels outside pushed rects ===");
    const byKind = new Map<string, number>();
    for (const v of violations) byKind.set(v.kind, (byKind.get(v.kind) ?? 0) + 1);
    check(
        "every pushed window's row length was a multiple of 8 (decision 0001)",
        (byKind.get("8px-rule") ?? 0) === 0,
        `${byKind.get("8px-rule") ?? 0} violation(s)`,
    );
    check(
        "no framebuffer pixel ever changed outside that tick's own pushed rectangles (the palette included)",
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
