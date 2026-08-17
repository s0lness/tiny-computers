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
        // Sets the pose and holds it - emu_tick() resubmits the last value
        // every tick (emu_shim.c), exactly like a hand holding the puck
        // still. Panel-space g, per tilt.h's own convention (AGENTS.md's
        // axis ritual): flat, screen up, is (0,0,1); top edge up is (0,1,0).
        gravity(x: number, y: number, z: number) { exp.emu_sensor_vector(1, x, y, z); },
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

// ---- the setting gesture, mirrored from clock.c's value_from_axis() ------
function valueFromAxis(v: number, lo: number, hi: number, range: number): number {
    if (v <= lo) return 0;
    if (v >= hi) return range - 1;
    return Math.floor(((v - lo) * range) / (hi - lo));
}
// The middle of the band of positions that produces `want`, so a test never
// sits on a boundary where an off-by-one in either direction changes the
// answer.
function axisForValue(want: number, lo: number, hi: number, range: number): number {
    let first = -1, last = -1;
    for (let v = lo; v <= hi; v++) {
        if (valueFromAxis(v, lo, hi, range) === want) {
            if (first < 0) first = v;
            last = v;
        }
    }
    if (first < 0) throw new Error(`no position on [${lo},${hi}] yields ${want} of ${range}`);
    return Math.round((first + last) / 2);
}

// Drives the whole set gesture for one field, in whichever layout is
// showing, and returns the new clock. `field` 0 = hours, 1 = minutes.
function setField(dev: Device, upright: boolean, field: number, value: number, t0: number): number {
    let t = t0;
    const range = field === 0 ? 24 : 60;
    let px: number, py: number;
    if (upright) {
        // Upright: vertical position picks the pair, horizontal carries the value.
        px = axisForValue(value, BEZEL, PANEL_W - BEZEL, range);
        py = field === 0 ? PANEL_H / 4 : (3 * PANEL_H) / 4;
    } else {
        // Long-ways: the other way round, in landscape coordinates, mapped
        // back to the panel space emu_touch speaks.
        const ly = axisForValue(value, BEZEL, LAND_H - BEZEL, range);
        const lx = field === 0 ? LAND_W / 4 : (3 * LAND_W) / 4;
        px = PANEL_W - 1 - ly;
        py = lx;
    }
    for (let i = 0; i < 6; i++) {
        t += 20;
        dev.touch(true, px, py);
        dev.tick(t);
    }
    t += 20;
    dev.touch(false, 0, 0);
    dev.tick(t);
    return t;
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

    // ---- 1. the empty face ------------------------------------------------
    console.log("\n-- a clock that has never been told the time --");
    let t = 125;
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

    // ---- 2 & 3. setting it, long-ways ------------------------------------
    console.log("\n-- the owner sets it: hold BOOT, slide over the hours, then over the minutes --");
    dev.drainLog();
    dev.boot(true);
    t = setField(dev, false, 0, 8, t);   // hours: the left pair, slid vertically
    t = setField(dev, false, 1, 38, t);  // minutes: the right pair
    dev.boot(false);
    t = settle(dev, t, 100);

    const setLine = dev.fwLogLines().findLast((l) => l.includes("clock: set to"));
    check("releasing BOOT commits the time that was dialled in, once",
        setLine === "clock: set to 08:38", setLine ?? "(no set line)");
    check("...and exactly one commit happened for the whole hold, not one per frame of the slide",
        dev.fwLogLines().filter((l) => l.includes("clock: set to")).length === 1,
        `${dev.fwLogLines().filter((l) => l.includes("clock: set to")).length} commits`);

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
    check("held long-ways it reads like the stopwatch, with the two dots between the pairs",
        cellDarkest(fb, dotsCell) < 100, `darkest pixel in the separator cell is ${cellDarkest(fb, dotsCell)}`);

    await writeShot("clock-landscape.png", fb, false,
        "held long-ways: one line, HH : MM, read like the stopwatch");

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
    check("...and it costs exactly ONE pushed window - one digit, once a minute, which is what not showing seconds buys",
        pushesOnRollover.length === 1, JSON.stringify(pushesOnRollover));

    // ---- 5. held upright --------------------------------------------------
    console.log("\n-- the puck is turned upright --");
    // g_clockApp.landscape is true, so the runtime hands this app gravity
    // already rotated into ITS OWN landscape space, and clock_is_upright()
    // reads TILT_UP_TOP/BOTTOM off that as "upright" (see clock.c's own
    // header comment: MEASURED, not derived from the rotation formula on
    // paper alone - that derivation got the pairing backwards once already).
    // Panel-space (-1,0,0) - AGENTS.md's own axis-ritual "quarter turn, its
    // RIGHT edge up" pose - is one of the two panel poses this app's own
    // rotation turns into land.up=TOP; empirically confirmed against this
    // emu.wasm (switch to the clock, inject this pose, read emu_tilt()),
    // not re-derived from the formula that already got it wrong once.
    dev.drainLog();
    dev.gravity(-1, 0, 0);
    // tilt.h's filter needs time to converge on a hard step (feature-tilt.ts
    // measures most of the way within one board-cadence tick, fully settled
    // well under a second); 1000ms of 20ms-cadence ticks, matching the
    // board's own sensor rate, is generous margin, not a tuned minimum.
    let layoutLine: string | undefined;
    for (let e = 0; e < 1000 && layoutLine === undefined; e += 20) {
        t += 20;
        dev.tick(t);
        layoutLine = dev.fwLogLines().findLast((l) => l.includes("clock: layout"));
    }
    check("turning the puck upright switches the face", layoutLine === "clock: layout upright", layoutLine ?? "(no layout line)");
    const flipPushes = dev.pushesLastTick();
    check("...and the whole panel is repainted and pushed in that one tick, since every pixel of it changed",
        flipPushes.length === 1 && flipPushes[0]!.w === PANEL_W && flipPushes[0]!.h === PANEL_H,
        JSON.stringify(flipPushes));

    t = settle(dev, t, 100);
    fb = dev.fbSnapshot();

    let uprightOk = true;
    const uprightDetail: string[] = [];
    for (let i = 0; i < 4; i++) {
        const c = cellFor(true, i);
        const mask = segmentMask(fb, c, c.t, isDark);
        const want = SEVEN_SEG[[0, 8, 3, 9][i]!]!;
        if (mask !== want) uprightOk = false;
        uprightDetail.push(`cell ${i}: 0x${mask.toString(16)} want 0x${want.toString(16)}`);
    }
    check("the upright face reads the same time, hours on one line and minutes on the line below",
        uprightOk, uprightDetail.join("; "));

    // The owner's precise requirement: same size, and NOTHING between them.
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
    // Within two pixels, and the two are a sub-pixel artefact rather than
    // slack. The block's geometry is exactly centred (60px of paper each
    // side of a 248px block on a 368px panel); what differs is which side of
    // a pixel boundary each anti-aliased edge falls on, because the left
    // edge of the ink lands on a .25 phase and the right edge on a .75 one.
    // A threshold applied to coverage then keeps one more pixel at one end
    // than the other. Nobody can see two pixels at 322ppi, and tightening
    // this to zero would mean moving the glyph off the grid it is centred on.
    check("the block is centred: equal paper left and right, equal paper above and below",
        Math.abs(whole.minX - (PANEL_W - 1 - whole.maxX)) <= 2 &&
        Math.abs(whole.minY - (PANEL_H - 1 - whole.maxY)) <= 2,
        `left ${whole.minX}, right ${PANEL_W - 1 - whole.maxX}, top ${whole.minY}, bottom ${PANEL_H - 1 - whole.maxY}`);

    await writeShot("clock-portrait.png", fb, true,
        "held upright: the hours on one line, the minutes on the line below, the same size, nothing between them");

    // ---- 6. the bezel -----------------------------------------------------
    let outside: string | null = null;
    for (let y = 0; y < PANEL_H && !outside; y++) {
        for (let x = 0; x < PANEL_W; x++) {
            const edge = x < BEZEL || y < BEZEL || x >= PANEL_W - BEZEL || y >= PANEL_H - BEZEL;
            if (edge && isInk(panelGray(fb, x, y))) { outside = `(${x},${y})`; break; }
        }
    }
    check(`no ink within PANEL_BEZEL_MARGIN_PX (${BEZEL}) of any edge, where the case hides it`,
        outside === null, outside ? `ink at ${outside}` : "every pixel of the face is inside the visible canvas");

    // ---- the same gesture, in the upright layout --------------------------
    console.log("\n-- setting it again while upright: the axes swap, the rule does not --");
    dev.drainLog();
    dev.boot(true);
    t = setField(dev, true, 1, 45, t);  // minutes: the LOWER line now, slid sideways
    dev.boot(false);
    t = settle(dev, t, 100);
    const upSetLine = dev.fwLogLines().findLast((l) => l.includes("clock: set to"));
    check("touching the lower line sets the minutes, and touching it does not disturb the hours",
        upSetLine === "clock: set to 08:45", upSetLine ?? "(no set line)");

    // The field is latched where the finger LANDS: a slide that starts on the
    // minutes and runs the length of the panel must not hand itself over to
    // the hours partway.
    console.log("\n-- a slide that starts on one pair and crosses the whole panel --");
    dev.drainLog();
    dev.boot(true);
    for (let x = BEZEL + 4; x < PANEL_W - BEZEL - 4; x += 12) {
        t += 20;
        dev.touch(true, x, (3 * PANEL_H) / 4); // starts and stays a minutes gesture
        dev.tick(t);
    }
    t += 20; dev.touch(false, 0, 0); dev.tick(t);
    dev.boot(false);
    t = settle(dev, t, 100);
    const crossLine = dev.fwLogLines().findLast((l) => l.includes("clock: set to"));
    check("the hours are untouched by a minutes slide, however far it travels",
        !!crossLine && crossLine.startsWith("clock: set to 08:"), crossLine ?? "(no set line)");

    // ---- 7. the invariants ------------------------------------------------
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
