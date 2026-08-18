// feature-clock: headless verification of the clock (firmware/apps/clock.c),
// against the REAL firmware compiled to wasm (decision 0003). Nothing here
// reimplements the app: every assertion reads the framebuffer, the firmware's
// own log lines, or the push-window list the firmware itself produced.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/feature-clock.ts
//
// WHAT THIS FILE PROVES, IN ORDER:
//   1. A CLOCK THAT HAS NOT BEEN TOLD THE TIME SAYS SO. The emulator's
//      stand-in RTC starts with no time (emu_shim.c, deliberately: a browser
//      tab has no battery-backed PCF85063 and pretending otherwise would let
//      this app ship having never been seen in the state a real puck comes up
//      in after a flat battery). The face is then four ghost numerals - every
//      segment lit, thin and pale - and it breathes. Asserted as: no dark ink
//      anywhere, all seven segments present in all four cells, and the ink
//      level actually changing over time.
//   2. THE TIME IS SET WITH ONE GESTURE AND NO COMPUTER: hold BOOT, slide a
//      finger over the pair you want. The pair is chosen by the axis that
//      SEPARATES the two pairs, the value by the other axis, in both layouts.
//      Checked in both, including that the field is latched when the finger
//      LANDS rather than re-picked as it slides.
//   3. THE FACE THEN READS THE TIME OFF THE PANEL, not off a log line: the
//      lit segments of each of the four cells are matched against the same
//      seven-segment table the firmware draws from.
//   4. A minute later, ONE cell changes and ONE window is pushed. This is the
//      "no seconds" decision stated as a measurement: on a working face the
//      only thing that ever moves is a minute digit, once a minute.
//   5. HELD UPRIGHT the hours sit over the minutes, the SAME SIZE (identical
//      ink bounding boxes, measured), with NOTHING between them (the whole
//      band between the two lines is blank paper, every pixel of it), and the
//      block is centred with equal paper on all four sides.
//   6. Every pixel of every layout is inside PANEL_BEZEL_MARGIN_PX, which the
//      emulator cannot show and the real case hides.
//   7. EVERY tick of all of the above obeys decision 0001's 8px rule and the
//      "no pixel changes outside the pushed rectangle" invariant.
//
// Screenshots go to preview/clock-*.png. The two long-ways ones are written
// LANDSCAPE (448x368) and the upright one PORTRAIT (368x448), each the way
// the device is actually held for that face; the rotation is applied at
// encode time only, and every assertion reads the framebuffer in its real
// layout.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const ROOT = join(import.meta.dir, "..", "..", "..");
const PREVIEW_DIR = join(ROOT, "preview");

const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const LAND_H = PANEL_W; // 368
// g_apps[] = { chrono, sketch("draw"), timer, four, clock, morpion, ... }
// (level removed from the table 2026-08-17). The clock used to sit at its
// own private negative index (APP_INDEX_CLOCK) because appending it to
// g_apps[] moved every menu column under the old row-of-columns layout.
// Decision 0013's grid replaced that layout, so the private slot is gone
// and the clock is just g_apps[4] like any other app.
const APP_CLOCK = 4;
const APP_ARENA_BYTES = 65536; // app.h APP_ARENA_BYTES
const BEZEL = 10; // gfx.h PANEL_BEZEL_MARGIN_PX
const BTN_BOOT = 0;
const BTN_PWR = 1;

// ---- mirrors of clock.c's own constants. Lifted, not re-derived: the same
// convention every other test in this directory uses for the app it drives.
const L_DIGIT_W = 80, L_DIGIT_H = 208, L_SEG_T = 20, L_Y0 = 80;
const L_DIGIT_X = [16, 112, 256, 352];
const L_DOTS_X = 208, L_DOTS_W = 32;
const P_DIGIT_W = 112, P_DIGIT_H = 168, P_SEG_T = 28;
const P_DIGIT_X = [60, 196, 60, 196];
const P_Y_HOURS = 28, P_Y_MINUTES = 252;
const P_DIGIT_Y = [P_Y_HOURS, P_Y_HOURS, P_Y_MINUTES, P_Y_MINUTES];
const GHOST_T = (t: number) => Math.floor((t * 2) / 5);
const SOFT_INSET = 0.75;
const DOUBLE_PRESS_WINDOW_MS = 500; // clock.c DOUBLE_PRESS_WINDOW_MS
const SET_MODE_TIMEOUT_MS = 60000;  // clock.c SET_MODE_TIMEOUT_MS
const CHEV_H = 9;       // clock.c CHEV_H (long-ways base-to-apex)
const CHEV_P_GAP = 6, CHEV_P_H = 9;   // clock.c CHEV_P_GAP / CHEV_P_H
const CHEV_L_GAP = 36;                // clock.c CHEV_L_GAP
// clock.c's CHEV_P_*/CHEV_L_* macros, evaluated here the same way the C
// preprocessor evaluates them there. Upright now points left/right (the
// owner's mockup - a "<" left of each line, a ">" right of it), so the two
// positions that matter are the shared left/right X offsets, not a per-line
// Y offset the way the old up/down layout needed.
const CHEV_P_HOURS_CY = P_Y_HOURS + P_DIGIT_H / 2, CHEV_P_MIN_CY = P_Y_MINUTES + P_DIGIT_H / 2;
// Upright chevrons are centred in the free space between the bezel margin and
// the digit block, not offset from the digits - clock.c's CHEV_P_LEFT_MID /
// CHEV_P_RIGHT_MID, evaluated the same way here. Derived from the same spans
// the firmware uses, so moving either the digits or the margin moves both
// together instead of leaving this file probing empty paper.
const CHEV_P_LEFT_MID = Math.floor((BEZEL + P_DIGIT_X[0]!) / 2);
const CHEV_P_RIGHT_MID = Math.floor(((P_DIGIT_X[1]! + P_DIGIT_W) + (PANEL_W - BEZEL)) / 2);
const CHEV_P_LEFT_BASE = CHEV_P_LEFT_MID + Math.floor(CHEV_P_H / 2);
const CHEV_P_LEFT_APEX = CHEV_P_LEFT_MID - Math.floor(CHEV_P_H / 2);
const CHEV_P_RIGHT_BASE = CHEV_P_RIGHT_MID - Math.floor(CHEV_P_H / 2);
const CHEV_P_RIGHT_APEX = CHEV_P_RIGHT_MID + Math.floor(CHEV_P_H / 2);
const CHEV_L_UP_BASE = L_Y0 - CHEV_L_GAP, CHEV_L_UP_APEX = CHEV_L_UP_BASE - CHEV_H;
const CHEV_L_DN_BASE = L_Y0 + L_DIGIT_H + CHEV_L_GAP, CHEV_L_DN_APEX = CHEV_L_DN_BASE + CHEV_H;

// digits.c's SEVEN_SEG, so the test can say "cell 0 is showing an 8" by
// looking at which segments are lit rather than by trusting a log line.
// Bit order: a=0x01 b=0x02 c=0x04 d=0x08 e=0x10 f=0x20 g=0x40.
const SEVEN_SEG = [0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f];

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

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
                // Trimmed: the firmware's own printf ends every line with a
                // CRLF for a serial console that is not here, and a test that
                // compares whole lines should not have to carry that.
                fwLogLines.push(decoder.decode(new Uint8Array(memory.buffer, ptr, len)).trimEnd());
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
    if (exp.emu_init() !== 1) throw new Error("emu_init() failed - see the log lines above");

    function fbSnapshot(): Uint8Array {
        return new Uint8Array(memory.buffer, exp.emu_fb(), PANEL_W * PANEL_H * 2).slice();
    }

    function pushRects(): { x: number; y: number; w: number; h: number }[] {
        const n = exp.emu_push_count();
        const out = [];
        for (let i = 0; i < n; i++) {
            out.push({ x: exp.emu_push_x(i), y: exp.emu_push_y(i), w: exp.emu_push_w(i), h: exp.emu_push_h(i) });
        }
        return out;
    }

    let lastRects: { x: number; y: number; w: number; h: number }[] = [];

    return {
        appSwitch(index: number) { exp.emu_app_switch(index); },
        appCurrent(): number { return exp.emu_app_current(); },
        boot(down: boolean) { exp.emu_button(0, down ? 1 : 0); },
        // Generic button level/verdict, for PWR (index 1): a real press is a
        // level edge PLUS, on release, the PMIC's own short/long verdict
        // (sensors.h) - see shortPressPWR()/doublePressPWR() below, which
        // drive both exactly the way emu_shim.c documents.
        button(index: number, down: boolean) { exp.emu_button(index, down ? 1 : 0); },
        buttonVerdict(index: number, isLong: boolean) { exp.emu_button_verdict(index, isLong ? 1 : 0); },
        // Sets the pose and holds it - emu_tick() resubmits the last value
        // every tick (emu_shim.c), exactly like a hand holding the puck
        // still. The argument is Panel-space g, per tilt.h's own convention
        // (AGENTS.md's axis ritual): flat, screen up, is (0,0,1); top edge
        // up is (0,1,0). emu_sensor_vector() itself wants DEVICE axes (the
        // same as a real IMU sample), so this undoes
        // firmware/runtime/tilt.c's device_to_panel() on the way in - see
        // feature-tilt.ts's gravity() for the fuller comment on why this
        // repeats that formula (currently its own inverse) rather than a
        // different one.
        gravity(x: number, y: number, z: number) { exp.emu_sensor_vector(1, y, x, -z); },
        touch(down: boolean, x: number, y: number) { exp.emu_touch(down ? 1 : 0, Math.round(x), Math.round(y)); },

        // Snapshots before, ticks, then diffs the framebuffer against the
        // union of this tick's own pushed rectangles and checks every
        // rectangle's row length. Every tick this file drives goes through
        // here, the breathing empty face included.
        tick(nowMs: number) {
            ticksChecked++;
            const before = fbSnapshot();
            exp.emu_tick(nowMs);
            const after = fbSnapshot();
            const rects = pushRects();
            lastRects = rects;

            for (const r of rects) {
                if (r.w % 8 !== 0) {
                    violations.push({ kind: "8px-rule", detail: `t=${nowMs} pushed rect (${r.x},${r.y},${r.w},${r.h}) has width ${r.w}, not a multiple of 8` });
                }
            }

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

        pushesLastTick() { return lastRects; },
        fbSnapshot,
        fwLogLines(): readonly string[] { return fwLogLines; },
        drainLog(): string[] { const out = fwLogLines.slice(); fwLogLines.length = 0; return out; },
    };
}

type Device = Awaited<ReturnType<typeof loadDevice>>;

// ---- reading the framebuffer ---------------------------------------------
// Panel space is the framebuffer's own layout. Landscape space is gfx.h's
// mapping, (lx, ly) -> panel (PANEL_W-1-ly, lx).
function panelGray(fb: Uint8Array, px: number, py: number): number {
    const x = Math.round(px), y = Math.round(py);
    if (x < 0 || y < 0 || x >= PANEL_W || y >= PANEL_H) return 255;
    const idx = (y * PANEL_W + x) * 2;
    const v = (fb[idx]! << 8) | fb[idx + 1]!;
    // The green channel is the ink/coverage value on this monochrome panel
    // (gfx.h's px_to_gray), which is the same thing the firmware composites
    // against.
    return ((v >> 5) & 0x3f) << 2;
}
function landGray(fb: Uint8Array, lx: number, ly: number): number {
    return panelGray(fb, PANEL_W - 1 - Math.round(ly), Math.round(lx));
}

const isDark = (g: number) => g < 100;      // a lit numeral
const isInk = (g: number) => g < 245;       // any ink at all, ghost included
const isPaper = (g: number) => g > 250;

// ---- cells, mirrored from clock.c ----------------------------------------
type Cell = { upright: boolean; x: number; y: number; w: number; h: number; t: number };
function cellFor(upright: boolean, i: number): Cell {
    return upright
        ? { upright, x: P_DIGIT_X[i]!, y: P_DIGIT_Y[i]!, w: P_DIGIT_W, h: P_DIGIT_H, t: P_SEG_T }
        : { upright, x: L_DIGIT_X[i]!, y: L_Y0, w: L_DIGIT_W, h: L_DIGIT_H, t: L_SEG_T };
}
function cellGray(fb: Uint8Array, c: Cell, x: number, y: number): number {
    return c.upright ? panelGray(fb, x, y) : landGray(fb, x, y);
}

// Same as cellGray, but reads through a 180-degree turn - the same
// panel_rotate_180() clock.c's paint_all() applies when flip180 is set (see
// clock.c's "THE ORIENTATION SIGNAL" and "PICTURE, ROTATED" sections): the
// probe's own cell-space coordinates resolve to panel space exactly as
// cellGray does, and then, if flipped, that panel point is point-reflected
// about the panel's own centre before sampling.
function cellGrayFlip(fb: Uint8Array, c: Cell, x: number, y: number, flip: boolean): number {
    let px: number, py: number;
    if (c.upright) { px = x; py = y; } else { px = PANEL_W - 1 - Math.round(y); py = Math.round(x); }
    if (flip) { px = PANEL_W - 1 - px; py = PANEL_H - 1 - py; }
    return panelGray(fb, px, py);
}

// The seven segment axes, derived exactly the way digits.c derives them
// (radius plus SOFT_INSET off each edge, the middle rail at half height),
// so a probe lands on a segment's centre line rather than near it. `strokeT`
// is the thickness actually used, which differs between a lit numeral and
// the thinner ghost.
function segmentProbes(c: Cell, strokeT: number): [number, number][] {
    const r = strokeT / 2;
    const xL = c.x + r + SOFT_INSET, xR = c.x + c.w - r - SOFT_INSET;
    const yT = c.y + r + SOFT_INSET, yB = c.y + c.h - r - SOFT_INSET;
    const yM = c.y + c.h / 2;
    const xM = c.x + c.w / 2;
    return [
        [xM, yT],                 // a
        [xR, (yT + yM) / 2],      // b
        [xR, (yM + yB) / 2],      // c
        [xM, yB],                 // d
        [xL, (yM + yB) / 2],      // e
        [xL, (yT + yM) / 2],      // f
        [xM, yM],                 // g
    ];
}

// Which segments of this cell carry ink, as the same bitmask SEVEN_SEG uses.
function segmentMask(fb: Uint8Array, c: Cell, strokeT: number, lit: (g: number) => boolean): number {
    let mask = 0;
    segmentProbes(c, strokeT).forEach(([x, y], i) => {
        if (lit(cellGray(fb, c, x, y))) mask |= 1 << i;
    });
    return mask;
}
// Same, reading through a 180-degree turn - see cellGrayFlip.
function segmentMaskFlip(fb: Uint8Array, c: Cell, strokeT: number, lit: (g: number) => boolean, flip: boolean): number {
    let mask = 0;
    segmentProbes(c, strokeT).forEach(([x, y], i) => {
        if (lit(cellGrayFlip(fb, c, x, y, flip))) mask |= 1 << i;
    });
    return mask;
}

// The darkest pixel anywhere in a cell: 255 means the cell is blank paper.
function cellDarkest(fb: Uint8Array, c: Cell): number {
    let darkest = 255;
    for (let y = c.y; y < c.y + c.h; y++) {
        for (let x = c.x; x < c.x + c.w; x++) {
            const g = cellGray(fb, c, x, y);
            if (g < darkest) darkest = g;
        }
    }
    return darkest;
}

// The bounding box of every inked pixel in a panel-space region.
function inkBox(fb: Uint8Array, x0: number, y0: number, x1: number, y1: number,
                lit: (g: number) => boolean = isInk) {
    let minX = 1e9, minY = 1e9, maxX = -1, maxY = -1, count = 0;
    for (let y = y0; y < y1; y++) {
        for (let x = x0; x < x1; x++) {
            if (!lit(panelGray(fb, x, y))) continue;
            count++;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }
    return { minX, minY, maxX, maxY, count, w: maxX - minX + 1, h: maxY - minY + 1 };
}

// Darkest pixel in a small region around a chevron's own midpoint (apex and
// base averaged), in whichever space it was drawn - just enough to say
// "ink is here" without needing the chevron's exact capsule geometry.
function chevronDarkest(fb: Uint8Array, upright: boolean, cx: number, apexY: number, baseY: number): number {
    const midY = (apexY + baseY) / 2;
    let darkest = 255;
    for (let dy = -6; dy <= 6; dy++) {
        for (let dx = -10; dx <= 10; dx++) {
            const g = upright ? panelGray(fb, cx + dx, midY + dy) : landGray(fb, cx + dx, midY + dy);
            if (g < darkest) darkest = g;
        }
    }
    return darkest;
}
// Same, for the upright face's LEFT/RIGHT-pointing chevrons (the owner's
// mockup): probes around the arrow's own midpoint (midX, cy) instead.
function chevronDarkestH(fb: Uint8Array, cy: number, apexX: number, baseX: number): number {
    const midX = (apexX + baseX) / 2;
    let darkest = 255;
    for (let dy = -10; dy <= 10; dy++) {
        for (let dx = -6; dx <= 6; dx++) {
            const g = panelGray(fb, midX + dx, cy + dy);
            if (g < darkest) darkest = g;
        }
    }
    return darkest;
}

// ---- the setting gesture: double-press PWR to open/commit, tap a chevron
// zone to step a field - mirrored from clock.c's pwr_double_press() and
// chevron_zone() ------------------------------------------------------------

// One PWR press-and-release, with the PMIC's own short-press verdict
// (KEY_SHORT) - the real edges a physical short tap produces, per
// sensors.h and emu_shim.c's PWR key section.
function shortPressPWR(dev: Device, t0: number): number {
    let t = t0;
    dev.button(BTN_PWR, true);
    t += 20; dev.tick(t);
    dev.button(BTN_PWR, false);
    dev.buttonVerdict(BTN_PWR, false);
    t += 20; dev.tick(t);
    return t;
}
// Two short presses inside DOUBLE_PRESS_WINDOW_MS - opens set mode the
// first time this is driven, commits it the second.
function doublePressPWR(dev: Device, t0: number): number {
    let t = shortPressPWR(dev, t0);
    t += 100; // well inside the window, comfortably clear of it too
    t = shortPressPWR(dev, t);
    return t;
}

// A tap in one of the four chevron zones - clock.c's chevron_zone(), mirrored
// exactly: upright quarters the panel's own Y (X unscoped); long-ways splits
// by touchY (which pair) then by touchX (which direction), converted to the
// panel point emu_touch speaks the same way clock.c's own comment documents.
type Field = "H+" | "H-" | "M+" | "M-";
function tapZone(dev: Device, upright: boolean, field: Field, t0: number): number {
    let px: number, py: number;
    if (upright) {
        // Y separates the pair (top half hours, bottom half minutes); X
        // carries the direction (left half decreases, right half increases -
        // the owner's mockup, "<" left of a line, ">" right of it).
        py = field[0] === "H" ? PANEL_H * 0.25 : PANEL_H * 0.75;
        px = field[1] === "+" ? PANEL_W * 0.75 : PANEL_W * 0.25;
    } else {
        const lx = field[0] === "H" ? LAND_W * 0.25 : LAND_W * 0.75;
        const ly = field[1] === "+" ? LAND_H * 0.25 : LAND_H * 0.75;
        px = PANEL_W - 1 - ly;
        py = lx;
    }
    let t = t0 + 20;
    dev.touch(true, px, py);
    dev.tick(t);
    t += 20;
    dev.touch(false, 0, 0);
    dev.tick(t);
    // Clear clock.c's own TAP_COOLDOWN_MS debounce (250ms - see that
    // constant's comment) before the NEXT tap, or a caller looping this to
    // step a field several times would have every call after the first
    // silently ignored, the same way a real dropout-flickered single tap
    // used to be silently ACCEPTED several times over before the guard
    // existed (repro-touch-dropout-clock-set.ts).
    t += 260;
    dev.tick(t);
    return t;
}

// Drives a gravity pose and lets tilt.h's filter converge before returning -
// the same margin every orientation section in this file already used.
function settleGravity(dev: Device, t0: number, g: [number, number, number], ms = 1000): number {
    let t = t0;
    dev.gravity(g[0], g[1], g[2]);
    for (let e = 0; e < ms; e += 20) { t += 20; dev.tick(t); }
    return t;
}

// What sensors_clock()'s own formula (mirrored, not re-derived) says the
// face should read `elapsedMs` after a commit of `h`:`m` - used instead of a
// hardcoded string wherever a section needs to keep checking the panel
// after time has gone on running in the background (an orientation sweep,
// say), so the check is honest about a real clock advancing rather than
// silently assuming nothing rolled over.
function expectedHM(h: number, m: number, elapsedMs: number): [number, number] {
    const totalSec = (h * 3600 + m * 60 + Math.floor(elapsedMs / 1000)) % 86400;
    return [Math.floor(totalSec / 3600), Math.floor(totalSec / 60) % 60];
}
function digitsOf(h: number, m: number): number[] {
    return [Math.floor(h / 10), h % 10, Math.floor(m / 10), m % 10];
}

function settle(dev: Device, t0: number, ms: number, stepMs = 25): number {
    let t = t0;
    for (let waited = 0; waited < ms; waited += stepMs) {
        t += stepMs;
        dev.tick(t);
    }
    return t;
}

// ---------------------------------------------------------------------
// PNG, same chunk/CRC/zlib-wrap machinery as feature-four.ts's encoder. The
// only difference here is that it is handed a sampler, so the same code
// writes a landscape face and an upright one.
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
function encodePNG(w: number, h: number, rgbAt: (x: number, y: number) => [number, number, number]): Uint8Array {
    const raw = new Uint8Array((w * 3 + 1) * h);
    for (let y = 0; y < h; y++) {
        const rowOff = y * (w * 3 + 1);
        raw[rowOff] = 0;
        for (let x = 0; x < w; x++) {
            const [r, g, b] = rgbAt(x, y);
            const o = rowOff + 1 + x * 3;
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
function rgb565At(fb: Uint8Array, px: number, py: number): [number, number, number] {
    const idx = (py * PANEL_W + px) * 2;
    const v = (fb[idx]! << 8) | fb[idx + 1]!;
    const r5 = (v >> 11) & 0x1f, g6 = (v >> 5) & 0x3f, b5 = v & 0x1f;
    return [Math.round((r5 * 255) / 31), Math.round((g6 * 255) / 63), Math.round((b5 * 255) / 31)];
}
const shots: string[] = [];
async function writeShot(name: string, fb: Uint8Array, upright: boolean, what: string) {
    const png = upright
        ? encodePNG(PANEL_W, PANEL_H, (x, y) => rgb565At(fb, x, y))
        : encodePNG(LAND_W, LAND_H, (lx, ly) => rgb565At(fb, PANEL_W - 1 - ly, lx));
    const path = join(PREVIEW_DIR, name);
    await Bun.write(path, png);
    shots.push(path);
    console.log(`    wrote ${path} (${png.length} bytes) - ${what}`);
}

// ---------------------------------------------------------------------
async function main() {
    console.log("=== feature: the clock (firmware/apps/clock.c) ===\n");
    const dev = await loadDevice();
    dev.tick(0);
    dev.appSwitch(APP_CLOCK);
    // Two ticks: app_switch_to() defers the switch to the END of the tick it
    // is called in (runtime_core.c), so the clock's own first tick - and
    // therefore its first log line - is the one after that.
    dev.tick(100);
    dev.tick(125);
    check("switched into the clock", dev.appCurrent() === APP_CLOCK, `app_current()=${dev.appCurrent()}`);

    const sizeLine = dev.fwLogLines().find((l) => l.includes("clock: state="));
    const sizeMatch = sizeLine?.match(/state=(\d+) bytes \(arena (\d+)\)/);
    const measuredBytes = sizeMatch ? Number(sizeMatch[1]) : -1;
    check("clock_state_t's measured size fits the arena, with the fit reported (not assumed)",
        !!sizeMatch && measuredBytes > 0 && measuredBytes <= APP_ARENA_BYTES,
        sizeLine ?? "(no sizeof line in the firmware log)");

    // ---- 1. the empty face --------------------------------------------------
    // Explicit from here on: the flat, unmeasured boot default is now
    // UPRIGHT (see clock.c's own "THE ORIENTATION SIGNAL" section - this is
    // a real, stated behaviour change from before), so every section below
    // drives a specific gravity pose rather than trusting an implicit
    // default. Long-ways first, since that is still the face the owner
    // described first and the one "reads like the stopwatch" is about.
    console.log("\n-- a clock that has never been told the time, held long-ways --");
    let t = 125;
    t = settleGravity(dev, t, [1, 0, 0]); // panel LEFT edge up -> land.up=BOTTOM -> long-ways, unflipped (buttons toward the top edge - the owner's own definition)
    check("the firmware says outright that it does not know the time",
        dev.fwLogLines().some((l) => l.includes("the time is not known")),
        dev.fwLogLines().find((l) => l.includes("not known")) ?? "(nothing said)");

    let fb = dev.fbSnapshot();
    let ghostOk = true;
    let darkestAnywhere = 255;
    const ghostDetail: string[] = [];
    for (let i = 0; i < 4; i++) {
        const c = cellFor(false, i);
        const mask = segmentMask(fb, c, GHOST_T(c.t), isInk);
        const darkest = cellDarkest(fb, c);
        if (darkest < darkestAnywhere) darkestAnywhere = darkest;
        if (mask !== 0x7f) ghostOk = false;
        ghostDetail.push(`cell ${i}: segments 0x${mask.toString(16)}, darkest ${darkest}`);
    }
    check("all four cells show a full ghost numeral - every segment, so it is a face with no number in it",
        ghostOk, ghostDetail.join("; "));
    check("and none of it is dark ink, so it can never be mistaken for a time",
        darkestAnywhere > 140, `darkest pixel in any cell is ${darkestAnywhere}`);

    await writeShot("clock-lost.png", fb, false,
        "the time has been lost: four pale ghost numerals, breathing, no number and no words");

    // It breathes: the ink level has to actually move, and it has to move
    // through more than two values (a blink is not a breath).
    const levels = new Set<number>();
    const probe = cellFor(false, 0);
    const [gx, gy] = segmentProbes(probe, GHOST_T(probe.t))[6]!; // the middle bar
    for (let i = 0; i < 100; i++) {
        t += 60;
        dev.tick(t);
        levels.add(cellGray(dev.fbSnapshot(), probe, gx, gy));
    }
    check("the empty face breathes rather than sitting frozen - the ink level moves through several values over six seconds",
        levels.size >= 3, `${levels.size} distinct ink levels: ${[...levels].sort((a, b) => a - b).join(", ")}`);

    // ---- 2. the double-press is reachable from the empty face -------------
    console.log("\n-- the double-press opens set mode even from the empty face --");
    dev.drainLog();
    t = doublePressPWR(dev, t);
    // Two different lines both contain "clock: set mode opened" - setting_tick()'s
    // own line (with the seed) and clock_tick()'s later paint-transition line
    // (just "opened"/"closed") - so match the seeded one specifically rather
    // than take whichever comes last.
    const openedFromUnknown = dev.fwLogLines().findLast((l) => l.includes("clock: set mode opened, seeded"));
    check("the first double-press opens set mode from the empty face, seeded at 12:00",
        openedFromUnknown === "clock: set mode opened, seeded at 12:00", openedFromUnknown ?? "(no open line)");

    fb = dev.fbSnapshot();
    let chevOk = true;
    const chevDetail: string[] = [];
    const landChevCentres: [number, number][] = [
        [(L_DIGIT_X[0]! + L_DIGIT_X[1]! + L_DIGIT_W) / 2, 0],
        [(L_DIGIT_X[2]! + L_DIGIT_X[3]! + L_DIGIT_W) / 2, 0],
    ];
    for (const [cx] of landChevCentres) {
        for (const [apexY, baseY] of [[CHEV_L_UP_APEX, CHEV_L_UP_BASE], [CHEV_L_DN_APEX, CHEV_L_DN_BASE]] as const) {
            const d = chevronDarkest(fb, false, cx, apexY, baseY);
            if (d > 140) chevOk = false;
            chevDetail.push(`cx=${cx} y~${(apexY + baseY) / 2}: darkest ${d}`);
        }
    }
    check("all four chevrons are visible at once - both fields are live, no mode step to discover",
        chevOk, chevDetail.join("; "));

    // ---- 3. tapping each field's zone, then committing ----------------------
    console.log("\n-- tapping the hours' zone down to 8, the minutes' down to 38 --");
    for (let i = 0; i < 4; i++) t = tapZone(dev, false, "H-", t); // 12 -> 8
    await writeShot("clock-set-hours.png", dev.fbSnapshot(), false,
        "set mode, long-ways: chevrons flank both pairs; the hours have just been tapped down to 08, the minutes still seeded at 00");
    // A LONG run in one direction - the shape of tap the owner asked to have
    // checked against TAP_COOLDOWN_MS: 22 consecutive decrements, back to
    // back, at tapZone()'s own just-past-cooldown pace (280ms/tap: 20ms down
    // + 20ms up + 260ms to clear TAP_COOLDOWN_MS's 250ms - the minimum gap
    // that still registers every tap, not a leisurely one).
    const minutesRunStart = t;
    for (let i = 0; i < 22; i++) t = tapZone(dev, false, "M-", t); // 0 -> 38 (wraps: 60-22=38)
    const minutesRunMs = t - minutesRunStart;
    console.log(`    22 consecutive decrements took ${minutesRunMs}ms of simulated time ` +
        `(${(minutesRunMs / 22).toFixed(0)}ms/tap) and all landed - see the segment check below`);
    await writeShot("clock-set-minutes.png", dev.fbSnapshot(), false,
        "set mode, long-ways: the minutes have just been tapped down (wrapping) to 38, the hours untouched at 08");

    dev.drainLog();
    t = doublePressPWR(dev, t); // commit
    const setLine = dev.fwLogLines().findLast((l) => l.includes("clock: set to"));
    check("the second double-press commits the dialled-in time, once",
        setLine === "clock: set to 08:38", setLine ?? "(no set line)");
    check("...and exactly one commit happened, not one per tap",
        dev.fwLogLines().filter((l) => l.includes("clock: set to")).length === 1,
        `${dev.fwLogLines().filter((l) => l.includes("clock: set to")).length} commits`);
    const committedAtMs = t;

    // Read the time back off the PANEL, through the segment table.
    fb = dev.fbSnapshot();
    const expected = [0, 8, 3, 8];
    let facesOk = true;
    const faceDetail: string[] = [];
    for (let i = 0; i < 4; i++) {
        const c = cellFor(false, i);
        const mask = segmentMask(fb, c, c.t, isDark);
        if (mask !== SEVEN_SEG[expected[i]!]) facesOk = false;
        faceDetail.push(`cell ${i}: 0x${mask.toString(16)} want 0x${SEVEN_SEG[expected[i]!]!.toString(16)}`);
    }
    check("the panel itself now reads 08:38, segment by segment", facesOk, faceDetail.join("; "));

    const dotsCell: Cell = { upright: false, x: L_DOTS_X, y: L_Y0, w: L_DOTS_W, h: L_DIGIT_H, t: L_SEG_T };
    // Sampled across a WHOLE pulse cycle, not at one instant. The pulse used
    // to alternate between two ink levels, so any single snapshot found
    // something dark; the owner then asked for "visible one second /
    // invisible one second", and on the invisible half the separator cell is
    // honestly blank paper. A one-shot probe therefore fails half the time
    // through no fault of the firmware. This asks the only question that
    // still means anything: are the dots there at SOME point in a cycle.
    let dotsDarkestSeen = 255;
    for (let i = 0; i < 12; i++) {
        t += 250;
        dev.tick(t);
        dotsDarkestSeen = Math.min(dotsDarkestSeen, cellDarkest(dev.fbSnapshot(), dotsCell));
    }
    check("held long-ways it reads like the stopwatch, with the two dots between the pairs",
        isInk(dotsDarkestSeen), `darkest pixel seen in the separator cell across a full pulse cycle is ${dotsDarkestSeen}`);
    check("the chevrons are gone now that set mode is closed",
        chevronDarkest(fb, false, landChevCentres[0]![0], CHEV_L_UP_APEX, CHEV_L_UP_BASE) > 140,
        `darkest pixel where the hours' up chevron was`);

    await writeShot("clock-running.png", fb, false,
        "the running clock, held long-ways: 08:38, dots pulsing, no chevrons");

    // ---- 4. one cell, once a minute --------------------------------------
    console.log("\n-- a minute passes --");
    dev.drainLog();
    let pushesOnRollover: { x: number; y: number; w: number; h: number }[] = [];
    let rolled = false;
    for (let i = 0; i < 30 && !rolled; i++) {
        t += 2500;
        dev.tick(t);
        if (dev.pushesLastTick().length > 0) {
            pushesOnRollover = dev.pushesLastTick();
            rolled = dev.fwLogLines().some((l) => l.includes("clock: 08:39"));
        }
    }
    check("the minute rolls over on its own", rolled,
        dev.fwLogLines().findLast((l) => l.startsWith("clock: 08:")) ?? "(no line)");
    // ONE digit window, plus the dots' own cell whenever the pulse toggles in
    // the same tick - which, since the pulse's half-period became 500ms (one
    // blink a second, at the owner's request), is EVERY minute boundary: a
    // minute is a whole number of half-seconds. So the honest bound is "the
    // digit, and nothing except possibly the separator", not "exactly one".
    // Loosened deliberately rather than by reflex: what this check exists to
    // catch is a rollover that repaints the whole face, and it still does.
    const digitCells = pushesOnRollover.filter((r) => r.h !== L_DOTS_W);
    const dotsCells = pushesOnRollover.filter((r) => r.h === L_DOTS_W);
    check("...and it costs ONE digit window, plus the separator only when its own blink lands in the same tick",
        digitCells.length === 1 && dotsCells.length <= 1,
        JSON.stringify(pushesOnRollover));

    // ---- 5. the pulse ---------------------------------------------------------
    console.log("\n-- the dots pulse on a working face --");
    const pulseLevels = new Set<number>();
    const dotsProbeX = L_DOTS_X + L_DOTS_W / 2, dotsProbeY = L_Y0 + L_DIGIT_H / 3;
    for (let i = 0; i < 40; i++) {
        t += 125;
        dev.tick(t);
        pulseLevels.add(landGray(dev.fbSnapshot(), dotsProbeX, dotsProbeY));
    }
    // Was "exactly two levels", which pinned the hard blink this started as.
    // The owner then asked for a fade ("j'aimerais un fondu plus doux comme
    // un pulse vraiment doux"), so two levels is now the FAILURE it would be
    // measuring for: a switch, not a breath. What still has to hold is that
    // it sweeps, and that it reaches both ends rather than hovering in some
    // comfortable grey - a fade that never fully arrives reads as a smudge.
    const sorted = [...pulseLevels].sort((a, b) => a - b);
    const lo = sorted[0] ?? 255, hi = sorted[sorted.length - 1] ?? 0;
    check("the dots fade rather than switch, and the sweep reaches both fully lit and fully gone",
        pulseLevels.size >= 8 && lo <= 16 && hi >= 244,
        `${pulseLevels.size} distinct ink levels over 5s, ${lo}..${hi}: ${sorted.join(", ")}`);

    // ---- 6. all four TURN edges, checked against the panel itself -----------
    console.log("\n-- the four TURN edges: top/bottom portrait, right/left long-ways --");
    // Panel-space g per puckpose.ts's own documented poses (turn=0,90,180,-90
    // at tilt=90): TOP=(0,1,0), RIGHT=(-1,0,0), BOTTOM=(0,-1,0), LEFT=(1,0,0).
    // Mapping and flip verified empirically against this emu.wasm on
    // 2026-08-18 (see clock.c's "THE ORIENTATION SIGNAL"), not re-derived -
    // TWICE now: the first pass had LEFT/RIGHT's flip backwards (the owner's
    // own definition, "in LEFT up is towards the boot and pwr button", both
    // on the panel's native RIGHT edge per AGENTS.md, settled via
    // emulator/src/device.ts's actual CSS - applyRotation()'s
    // rotate(totalDeg), standard clockwise-positive - rather than reasoned
    // out on paper a third time).
    const edgePoses: { name: string; g: [number, number, number]; upright: boolean; flip: boolean }[] = [
        { name: "TOP up (panel)",    g: [0, 1, 0],  upright: true,  flip: false },
        { name: "BOTTOM up (panel)", g: [0, -1, 0], upright: true,  flip: true },
        { name: "RIGHT up (panel)",  g: [-1, 0, 0], upright: false, flip: true },
        { name: "LEFT up (panel)",   g: [1, 0, 0],  upright: false, flip: false },
    ];
    let topFb: Uint8Array | null = null;
    for (const pose of edgePoses) {
        t = settleGravity(dev, t, pose.g);
        fb = dev.fbSnapshot();
        if (pose.name.startsWith("TOP")) topFb = fb;
        const [h, m] = expectedHM(8, 38, t - committedAtMs);
        const want = digitsOf(h, m);
        let ok = true;
        const detail: string[] = [];
        for (let i = 0; i < 4; i++) {
            const c = cellFor(pose.upright, i);
            const mask = segmentMaskFlip(fb, c, c.t, isDark, pose.flip);
            const wantMask = SEVEN_SEG[want[i]!]!;
            if (mask !== wantMask) ok = false;
            detail.push(`cell ${i}: 0x${mask.toString(16)} want 0x${wantMask.toString(16)}`);
        }
        check(`${pose.name}: reads ${String(h).padStart(2, "0")}:${String(m).padStart(2, "0")} correctly, hours-side up, not mirrored`,
            ok, detail.join("; "));
    }

    // The owner's precise requirement for the upright face, checked against
    // the TOP-up (unflipped) capture above: same size, and NOTHING between
    // them.
    fb = topFb!;
    const hoursBox = inkBox(fb, 0, 0, PANEL_W, PANEL_H / 2);
    const minutesBox = inkBox(fb, 0, PANEL_H / 2, PANEL_W, PANEL_H);
    check("the two lines are exactly the same size - identical ink width and height",
        hoursBox.w === minutesBox.w && hoursBox.h === minutesBox.h,
        `hours ${hoursBox.w}x${hoursBox.h}, minutes ${minutesBox.w}x${minutesBox.h}`);

    let bandInk: string | null = null;
    for (let y = hoursBox.maxY + 1; y < minutesBox.minY && !bandInk; y++) {
        for (let x = 0; x < PANEL_W; x++) {
            if (!isPaper(panelGray(fb, x, y))) { bandInk = `(${x},${y}) is not paper`; break; }
        }
    }
    check("there is NO colon and no separator of any kind between them - the whole band is blank paper",
        bandInk === null, bandInk ?? `${minutesBox.minY - hoursBox.maxY - 1} blank rows between the lines`);
    check("the gap between the lines is wider than the gap between the digits within a line, which is what pairs each line up without a separator",
        minutesBox.minY - hoursBox.maxY - 1 > P_DIGIT_X[1]! - (P_DIGIT_X[0]! + P_DIGIT_W),
        `${minutesBox.minY - hoursBox.maxY - 1}px between the lines, ${P_DIGIT_X[1]! - (P_DIGIT_X[0]! + P_DIGIT_W)}px between the digits`);

    // Measured on solid ink rather than on any ink at all: an anti-aliased
    // edge puts a pixel of one or two percent coverage half a pixel outside
    // the glyph, and which side of a boundary that faint pixel rounds onto is
    // not what "centred" means here.
    const whole = inkBox(fb, 0, 0, PANEL_W, PANEL_H, isDark);
    check("the block is centred: equal paper left and right, equal paper above and below",
        Math.abs(whole.minX - (PANEL_W - 1 - whole.maxX)) <= 2 &&
        Math.abs(whole.minY - (PANEL_H - 1 - whole.maxY)) <= 2,
        `left ${whole.minX}, right ${PANEL_W - 1 - whole.maxX}, top ${whole.minY}, bottom ${PANEL_H - 1 - whole.maxY}`);

    await writeShot("clock-portrait.png", fb, true,
        "held upright (panel TOP edge up): the hours on one line, the minutes on the line below, the same size, nothing between them");

    // One of the four, screenshotted, RAW (unrotated) panel space, so the
    // rotation is visible rather than undone by the viewer helper - this is
    // the puck held upside down (panel BOTTOM up), portrait, flipped.
    t = settleGravity(dev, t, [0, -1, 0]);
    await writeShot("clock-turned-upside-down.png", dev.fbSnapshot(), true,
        "held upside down (panel BOTTOM edge up): the RAW panel framebuffer - " +
        "the whole picture is turned 180 so it still reads hours-over-minutes " +
        "right side up once the panel itself is physically upside down");

    // The long-ways flip, the one that was wrong: panel RIGHT edge up puts
    // the buttons toward the BOTTOM of the panel (opposite the owner's own
    // "up is towards the buttons" LEFT example), so this is the half of the
    // long-ways pair that gets turned. Shown through the STANDARD landscape
    // mapping (upright=false), the same "buttons-at-top" viewing convention
    // clock-running.png (panel LEFT edge up) uses - if the internal flip is
    // correct, this same viewer should show the picture turned end for end
    // relative to that one, buttons-toward-the-glass-bottom instead.
    t = settleGravity(dev, t, [-1, 0, 0]);
    await writeShot("clock-turned-longways-flipped.png", dev.fbSnapshot(), false,
        "panel RIGHT edge up (buttons toward the bottom of the glass): the same " +
        "buttons-at-top landscape viewer clock-running.png uses, so the 180 turn " +
        "shows as the whole HH:MM reading upside down in THIS picture - that is " +
        "correct here, it is the half of the pair that needed the flip");

    // ---- 7. the bezel -----------------------------------------------------
    t = settleGravity(dev, t, [1, 0, 0]); // back to long-ways, unflipped (panel LEFT up), for the rest of this file
    fb = dev.fbSnapshot();
    let outside: string | null = null;
    for (let y = 0; y < PANEL_H && !outside; y++) {
        for (let x = 0; x < PANEL_W; x++) {
            const edge = x < BEZEL || y < BEZEL || x >= PANEL_W - BEZEL || y >= PANEL_H - BEZEL;
            if (edge && isInk(panelGray(fb, x, y))) { outside = `(${x},${y})`; break; }
        }
    }
    check(`no ink within PANEL_BEZEL_MARGIN_PX (${BEZEL}) of any edge, where the case hides it`,
        outside === null, outside ? `ink at ${outside}` : "every pixel of the face is inside the visible canvas");

    // ---- 8. the same gesture, upright: chevrons point left/right ----------
    console.log("\n-- the same gesture upright: chevrons point left/right, and the direction follows the arrow --");
    t = settleGravity(dev, t, [0, 1, 0]); // panel TOP up -> upright, unflipped
    dev.drainLog();
    t = doublePressPWR(dev, t);
    const openLine = dev.fwLogLines().findLast((l) => l.includes("clock: set mode opened, seeded at"));
    const seedMatch = openLine?.match(/seeded at (\d+):(\d+)/);
    check("set mode opened upright, seeded from the running clock", !!seedMatch, openLine ?? "(no open line)");
    let seedH = seedMatch ? Number(seedMatch[1]) : 0;
    let seedM = seedMatch ? Number(seedMatch[2]) : 0;

    fb = dev.fbSnapshot();
    let chevOkP = true;
    const chevDetailP: string[] = [];
    for (const cy of [CHEV_P_HOURS_CY, CHEV_P_MIN_CY]) {
        for (const [apexX, baseX] of [[CHEV_P_LEFT_APEX, CHEV_P_LEFT_BASE], [CHEV_P_RIGHT_APEX, CHEV_P_RIGHT_BASE]] as const) {
            const d = chevronDarkestH(fb, cy, apexX, baseX);
            if (d > 140) chevOkP = false;
            chevDetailP.push(`cy=${cy} x~${(apexX + baseX) / 2}: darkest ${d}`);
        }
    }
    check("upright: all four left/right chevrons are visible - \"<\" left of each line, \">\" right",
        chevOkP, chevDetailP.join("; "));
    await writeShot("clock-set-portrait.png", fb, true,
        "set mode, upright: \"<\" outside the left edge of each line, \">\" outside the right, well clear of the digits near the bezel");

    // Left of a line decreases it, right increases - the owner's mockup,
    // checked in BOTH directions, on BOTH fields, not assumed symmetric.
    t = tapZone(dev, true, "M-", t);
    seedM = (seedM + 60 - 1) % 60;
    await writeShot("clock-set-portrait-after.png", dev.fbSnapshot(), true,
        "set mode, upright, after one tap left of the minutes: only the minutes moved, one step down");
    t = tapZone(dev, true, "H+", t);
    seedH = (seedH + 24 + 1) % 24;
    t = doublePressPWR(dev, t);
    const upSetLine = dev.fwLogLines().findLast((l) => l.includes("clock: set to"));
    const wantUpSet = `clock: set to ${String(seedH).padStart(2, "0")}:${String(seedM).padStart(2, "0")}`;
    check("tapping left of a line decrements it and right increments it, upright, exactly as the mockup's arrows promise",
        upSetLine === wantUpSet, `${upSetLine ?? "(no set line)"} (wanted ${wantUpSet})`);

    // ---- 9. the double-press window: two taps too far apart is not a
    // double-press ----------------------------------------------------------
    console.log("\n-- a slow double-press outside the window opens nothing --");
    dev.drainLog();
    let tt = shortPressPWR(dev, t);
    tt += DOUBLE_PRESS_WINDOW_MS + 250; // comfortably outside the window
    tt = shortPressPWR(dev, tt);
    check("two short presses further apart than DOUBLE_PRESS_WINDOW_MS never open set mode",
        !dev.fwLogLines().some((l) => l.includes("clock: set mode opened")),
        dev.fwLogLines().filter((l) => l.includes("clock:")).join(" | "));
    t = tt;

    // ---- 10. abandonment: wandering off mid-set discards, silently --------
    console.log("\n-- wandering off mid-set times out and discards --");
    dev.drainLog();
    // Past the confirm lockout first. A confirm deafens both button
    // detectors for one DOUBLE_PRESS_WINDOW_MS so that the second press of
    // the owner's own double-press cannot reopen what the first just closed
    // (see setting_tick()). A test pressing again with zero delay is not a
    // person; a person pausing half a second is.
    t += DOUBLE_PRESS_WINDOW_MS + 100;
    dev.tick(t);
    t = doublePressPWR(dev, t);
    check("set mode opened for the abandonment check",
        dev.fwLogLines().some((l) => l.includes("clock: set mode opened")));
    t = tapZone(dev, true, "H+", t); // one interaction, to arm lastActivityMs properly
    dev.drainLog();
    t += SET_MODE_TIMEOUT_MS + 500; // one tick, straight past the timeout
    dev.tick(t);
    check("set mode abandons itself after SET_MODE_TIMEOUT_MS of inactivity",
        dev.fwLogLines().some((l) => l.includes("clock: set mode abandoned")),
        dev.fwLogLines().filter((l) => l.includes("clock:")).join(" | "));
    check("...and nothing was committed - the abandoned dial never reaches the RTC",
        !dev.fwLogLines().some((l) => l.includes("clock: set to")));

    // ---- 11. the invariants ------------------------------------------------
    console.log("\n=== invariants across this whole run (EVERY tick, the breathing face included) ===");
    console.log(`    ${ticksChecked} ticks checked in total`);
    const byKind = new Map<string, number>();
    for (const v of violations) byKind.set(v.kind, (byKind.get(v.kind) ?? 0) + 1);
    check("every pushed window's row length was a multiple of 8 (decision 0001)",
        (byKind.get("8px-rule") ?? 0) === 0, `${byKind.get("8px-rule") ?? 0} violation(s)`);
    check("no framebuffer pixel ever changed outside that tick's own pushed rectangles",
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
