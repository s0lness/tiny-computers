// clock-spacing-sweep: measures the clock face's ink margins across every
// one of the 1440 displayable times (00:00..23:59), in both orientations,
// against the REAL compiled firmware (decision 0003) - not a re-implemented
// model of digits.c's geometry.
//
// THE COMPLAINT this answers, the owner's own words: "c'est surtout quand
// les chiffres changent que parfois ça bouge. fais une simulation sur
// toutes les positions heures/minutes qu'on peut avoir pour avoir toujours
// un bon espacement. y a peut être une bonne règle." The digit CELLS never
// move (fixed rectangles, pushed as fixed windows); what varies is how much
// INK a digit puts inside its own cell, and a seven-segment "1" lights only
// two of seven segments, so it is much narrower than its neighbours. That
// already got a partial fix (digits_draw_soft() centres a "1" in its own
// cell instead of hugging the right rail, digits.c), but centring only
// halves the effect: it swaps "the cell's slack sits on one side" for "the
// cell's slack sits on both sides equally", and either way, a cell that
// happens to be the FACE's own outermost one changes the face's outer
// margin every time a "1" moves in or out of it.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run tools/clock-spacing-sweep.ts
//
// HOW A TIME IS PRODUCED, so this measures a REAL, reachable state rather
// than poking firmware internals no hardware could reach. There is no
// direct "set the clock" entry point in emu_abi.h and there must not be one
// (emu_abi.h's own "KEEPING THE EMULATOR HONEST" rule: "the emulator must
// never deliver an input the hardware cannot produce", and clock.c's own
// "WHY NOT OVER USB" section already argues at length against a back-door
// clock setter). So this drives the SAME double-press-then-tap gesture a
// real thumb would (feature-clock.ts's doublePressPWR()/tapZone(),
// mirrored here, not re-derived): open set mode once, then walk every
// (hour, minute) pair as a Hamiltonian path over 1439 single-step taps
// (tap M+ to advance the minute, wrapping 59->0; tap H+ once whenever that
// wrap happens, since the two fields do not carry into each other) rather
// than reopening/closing 1440 times. Sampling happens while set mode is
// still open - face_now() draws the LIVE dialled-in value the same way it
// draws a committed one (clock.c: "the face shows what he is dialling in,
// not what the chip still holds") - so every sample is pixel-for-pixel the
// same digit rendering a running face would show, without paying for 1440
// open/commit/lockout cycles.
//
// Set mode also draws four chevrons, which a running face never shows and
// which this tool does not want to measure. They do not need masking: every
// chevron sits, by construction (clock.c's own CHEV_* layout comments),
// outside the digit ROW in long-ways (above/below L_Y0..L_Y0+L_DIGIT_H) and
// outside the digit COLUMNS in upright (left/right of P_X0..P_X1+P_DIGIT_W).
// Restricting the scan window to exactly the digit cells' own span excludes
// them structurally, the same way clock.c's own cell rectangles do.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "emulator", "wasm", "dist", "emu.wasm");
const ROOT = join(import.meta.dir, "..");
const PREVIEW_DIR = join(ROOT, "preview");

const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const LAND_H = PANEL_W; // 368
const APP_CLOCK = 4; // g_apps[4] - see feature-clock.ts's own comment on this index
const BTN_PWR = 1;

// ---- clock.c's own LAYOUT constants, lifted (not re-derived) the same way
// feature-clock.ts already does for this file - see clock.c's "LAYOUT"
// section. Only POSITION/SIZE constants, deliberately not L_SEG_T/P_SEG_T:
// those are stroke thickness, live inside the range another agent is
// working on this same day (clock.c's tunables), and this tool never needs
// them - it finds ink by scanning for dark pixels, not by predicting where
// a stroke of a particular thickness would be.
const L_DIGIT_W = 80, L_DIGIT_H = 208, L_Y0 = 80;
const L_DIGIT_X = [16, 112, 256, 352];
const P_DIGIT_W = 112, P_DIGIT_H = 168;
const P_X0 = 60, P_X1 = 196;
const P_Y_HOURS = 28, P_Y_MINUTES = 252;
const DOUBLE_PRESS_WINDOW_MS = 500;
const TAP_COOLDOWN_MS = 250;

const isDark = (g: number) => g < 100; // clock.c's INK_LIT=0; feature-clock.ts's own threshold

async function loadDevice() {
    const bytes = readFileSync(WASM_PATH);
    const mod = await WebAssembly.compile(bytes);
    let memory!: WebAssembly.Memory;
    const decoder = new TextDecoder();
    const fwLogLines: string[] = [];
    const imports = {
        env: {
            js_log(ptr: number, len: number) {
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
    return {
        tick(nowMs: number) { exp.emu_tick(nowMs); },
        appSwitch(index: number) { exp.emu_app_switch(index); },
        button(index: number, down: boolean) { exp.emu_button(index, down ? 1 : 0); },
        buttonVerdict(index: number, isLong: boolean) { exp.emu_button_verdict(index, isLong ? 1 : 0); },
        gravity(x: number, y: number, z: number) { exp.emu_sensor_vector(1, y, x, -z); },
        touch(down: boolean, x: number, y: number) { exp.emu_touch(down ? 1 : 0, Math.round(x), Math.round(y)); },
        fbSnapshot,
        fwLogLines(): readonly string[] { return fwLogLines; },
    };
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

// ---- reading the framebuffer, mirrored from feature-clock.ts -------------
function panelGray(fb: Uint8Array, px: number, py: number): number {
    const x = Math.round(px), y = Math.round(py);
    if (x < 0 || y < 0 || x >= PANEL_W || y >= PANEL_H) return 255;
    const idx = (y * PANEL_W + x) * 2;
    const v = (fb[idx]! << 8) | fb[idx + 1]!;
    return ((v >> 5) & 0x3f) << 2;
}
function landGray(fb: Uint8Array, lx: number, ly: number): number {
    return panelGray(fb, PANEL_W - 1 - Math.round(ly), Math.round(lx));
}

// ---- the setting gesture, mirrored from feature-clock.ts's own driver ----
function shortPressPWR(dev: Device, t0: number): number {
    let t = t0;
    dev.button(BTN_PWR, true);
    t += 20; dev.tick(t);
    dev.button(BTN_PWR, false);
    dev.buttonVerdict(BTN_PWR, false);
    t += 20; dev.tick(t);
    return t;
}
function doublePressPWR(dev: Device, t0: number): number {
    let t = shortPressPWR(dev, t0);
    t += 100;
    t = shortPressPWR(dev, t);
    return t;
}
type Field = "H+" | "H-" | "M+" | "M-";
function tapZone(dev: Device, upright: boolean, field: Field, t0: number): number {
    let px: number, py: number;
    if (upright) {
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
    t += TAP_COOLDOWN_MS + 10; // clear clock.c's own debounce before the next tap
    dev.tick(t);
    return t;
}
// Reads back the TRUE seed a just-opened set-mode session started from,
// off the firmware's own log line, rather than assuming 12:00.
//
// THE BUG THIS REPLACES, found by this tool measuring itself: "seeded at
// 12:00" is only true the FIRST time set mode ever opens (an unset clock,
// clock.c's face_now() default). Every later open - the second orientation's
// sweep, every worst-case screenshot after the first - seeds from whatever
// was last COMMITTED instead (clock.c: "seeded from whatever is on
// screen"), which by then is wherever the previous sweep's own dial left
// off. Assuming 12:00 there silently dialled from the wrong starting point,
// producing a real, internally-consistent-looking rendering of a time
// nobody asked for - caught only by cross-checking a screenshot against its
// own intended label. Reading the log line this app already prints
// ("clock: set mode opened, seeded at HH:MM") is the same discipline
// feature-clock.ts's own openedFromUnknown check uses, and makes this tool
// correct regardless of what state a previous call left the firmware in.
function readSeed(dev: Device): [number, number] {
    const line = dev.fwLogLines().findLast((l) => l.includes("clock: set mode opened, seeded at"));
    const m = line?.match(/seeded at (\d+):(\d+)/);
    if (!m) throw new Error(`readSeed: no seed log line found; last lines: ${dev.fwLogLines().slice(-5).join(" | ")}`);
    return [Number(m[1]), Number(m[2])];
}

function settleGravity(dev: Device, t0: number, g: [number, number, number], ms = 1000): number {
    let t = t0;
    dev.gravity(g[0], g[1], g[2]);
    for (let e = 0; e < ms; e += 20) { t += 20; dev.tick(t); }
    return t;
}

// ---- margin measurement ---------------------------------------------------
// Scans a window for the darkest-ink extremes and reports where the face's
// own ink starts relative to the canvas edge on each side. `x0human width`
// is the coordinate space's own extent (LAND_W for long-ways, PANEL_W for
// upright), and margins are measured against that, not against the scan
// window - the scan window only exists to exclude the chevrons.
type Margins = { left: number; right: number; diff: number; hasInk: boolean };
function measureMargins(
    fb: Uint8Array,
    width: number,
    x0: number, x1: number, y0: number, y1: number,
    grayAt: (fb: Uint8Array, x: number, y: number) => number,
): Margins {
    let minX = Infinity, maxX = -Infinity;
    for (let y = y0; y < y1; y++) {
        for (let x = x0; x < x1; x++) {
            if (!isDark(grayAt(fb, x, y))) continue;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
        }
    }
    if (!Number.isFinite(minX)) return { left: NaN, right: NaN, diff: NaN, hasInk: false };
    const left = minX;
    const right = (width - 1) - maxX;
    return { left, right, diff: Math.abs(left - right), hasInk: true };
}

// ---- PNG encoder, lifted from feature-clock.ts's own (same chunk/CRC/zlib
// machinery every feature-*.ts test in this directory already carries) ----
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
async function writeShot(name: string, fb: Uint8Array, upright: boolean, what: string) {
    const png = upright
        ? encodePNG(PANEL_W, PANEL_H, (x, y) => rgb565At(fb, x, y))
        : encodePNG(LAND_W, LAND_H, (lx, ly) => rgb565At(fb, PANEL_W - 1 - ly, lx));
    const path = join(PREVIEW_DIR, name);
    await Bun.write(path, png);
    console.log(`    wrote ${path} (${png.length} bytes) - ${what}`);
}

// ---- one digit's identity, for grouping the histogram by digit pattern ---
function digitsOf(h: number, m: number): [number, number, number, number] {
    return [Math.floor(h / 10), h % 10, Math.floor(m / 10), m % 10];
}
function patternLabel(h: number, m: number): string {
    const [ht, hu, mt, mu] = digitsOf(h, m);
    const mark = (d: number) => (d === 1 ? "1" : ".");
    return `${mark(ht)}${mark(hu)}${mark(mt)}${mark(mu)}`; // "1..." etc: which of the 4 slots is a "1"
}

type Sample = {
    h: number; m: number;
    overall: Margins;
    hours?: Margins;   // upright only
    minutes?: Margins; // upright only
};

function histogram(label: string, diffs: number[]) {
    const buckets = new Map<number, number>();
    for (const d of diffs) {
        const rounded = Math.round(d * 4) / 4; // quarter-pixel buckets, since SOFT_INSET is 0.75
        buckets.set(rounded, (buckets.get(rounded) ?? 0) + 1);
    }
    const keys = [...buckets.keys()].sort((a, b) => a - b);
    console.log(`  ${label}: ${diffs.length} samples`);
    for (const k of keys) {
        const n = buckets.get(k)!;
        const pct = ((n / diffs.length) * 100).toFixed(1);
        const bar = "#".repeat(Math.max(1, Math.round((n / diffs.length) * 60)));
        console.log(`    diff=${k.toFixed(2).padStart(6)}px  ${String(n).padStart(5)} (${pct.padStart(5)}%)  ${bar}`);
    }
    const max = Math.max(...diffs);
    const mean = diffs.reduce((a, b) => a + b, 0) / diffs.length;
    const zero = diffs.filter((d) => d < 0.5).length;
    console.log(`    max=${max.toFixed(2)}px  mean=${mean.toFixed(2)}px  within 0.5px of symmetric: ${zero}/${diffs.length} (${((zero / diffs.length) * 100).toFixed(1)}%)`);
}

async function sweepOrientation(dev: Device, upright: boolean, t0: number): Promise<{ samples: Sample[]; tEnd: number }> {
    let t = t0;
    t = settleGravity(dev, t, upright ? [0, 1, 0] : [1, 0, 0]); // TOP-up / LEFT-up: the unflipped pose for each family - feature-clock.ts's own edgePoses table
    t = doublePressPWR(dev, t); // open - seed read back below, not assumed
    const [h0, m0] = readSeed(dev);
    for (let i = 0; i < h0; i++) t = tapZone(dev, upright, "H-", t);
    for (let i = 0; i < m0; i++) t = tapZone(dev, upright, "M-", t);

    const samples: Sample[] = [];
    const sample = (h: number, m: number) => {
        const fb = dev.fbSnapshot();
        let overall: Margins, hours: Margins | undefined, minutes: Margins | undefined;
        if (upright) {
            overall = measureMargins(fb, PANEL_W, P_X0, P_X1 + P_DIGIT_W, 0, PANEL_H, panelGray);
            hours = measureMargins(fb, PANEL_W, P_X0, P_X1 + P_DIGIT_W, P_Y_HOURS, P_Y_HOURS + P_DIGIT_H, panelGray);
            minutes = measureMargins(fb, PANEL_W, P_X0, P_X1 + P_DIGIT_W, P_Y_MINUTES, P_Y_MINUTES + P_DIGIT_H, panelGray);
        } else {
            overall = measureMargins(fb, LAND_W, 0, LAND_W, L_Y0, L_Y0 + L_DIGIT_H, landGray);
        }
        samples.push({ h, m, overall, hours, minutes });
    };

    sample(0, 0);
    let curH = 0, curM = 0;
    for (let total = 1; total < 1440; total++) {
        t = tapZone(dev, upright, "M+", t);
        curM = (curM + 1) % 60;
        if (curM === 0) {
            t = tapZone(dev, upright, "H+", t);
            curH = (curH + 1) % 24;
        }
        sample(curH, curM);
    }
    // Close this session before returning. THE BUG THIS GUARDS AGAINST, found
    // by this tool while measuring itself: a double-press when set mode is
    // ALREADY active does not open a fresh session, it CONFIRMS the open one
    // (clock.c: "pressing boot or pwr once should confirm the time" while
    // active) and then the confirm's own lockout (DOUBLE_PRESS_WINDOW_MS)
    // swallows the second press of what looked like a fresh double-press. A
    // caller that left a session open and then called doublePressPWR() again
    // would silently end up with set mode CLOSED and the real clock running
    // free, every further tap landing on nothing - which is exactly what
    // produced impossible-looking times (a hoursDiff stuck at 0 the whole
    // upright sweep, a re-dialled screenshot showing a time nobody asked for)
    // before this close was added.
    t = shortPressPWR(dev, t);
    return { samples, tEnd: t };
}

function worstN(samples: Sample[], n: number, pick: (s: Sample) => Margins): Sample[] {
    return [...samples]
        .filter((s) => pick(s).hasInk)
        .sort((a, b) => pick(b).diff - pick(a).diff)
        .slice(0, n);
}

async function main() {
    console.log("=== clock-spacing-sweep: every displayable time, both orientations ===\n");
    const dev = await loadDevice();
    dev.tick(0);
    dev.appSwitch(APP_CLOCK);
    dev.tick(100);
    dev.tick(125);

    // ---- long-ways ----------------------------------------------------------
    console.log("-- long-ways (448x368 landscape), scanning the digit row only (chevrons excluded by y-window) --");
    const { samples: land, tEnd } = await sweepOrientation(dev, false, 125);
    histogram("overall face", land.map((s) => s.overall.diff));

    const landWorst = worstN(land, 3, (s) => s.overall);
    console.log("  worst 3 (long-ways, overall face margin difference):");
    for (const s of landWorst) {
        console.log(`    ${String(s.h).padStart(2, "0")}:${String(s.m).padStart(2, "0")}  pattern=${patternLabel(s.h, s.m)}  left=${s.overall.left.toFixed(2)}  right=${s.overall.right.toFixed(2)}  diff=${s.overall.diff.toFixed(2)}px`);
    }
    // A "clean" reference (no 1 anywhere) for contrast.
    const landClean = land.find((s) => patternLabel(s.h, s.m) === "....");
    if (landClean) console.log(`  reference, no '1' anywhere: ${String(landClean.h).padStart(2, "0")}:${String(landClean.m).padStart(2, "0")}  left=${landClean.overall.left.toFixed(2)}  right=${landClean.overall.right.toFixed(2)}  diff=${landClean.overall.diff.toFixed(2)}px`);

    // ---- upright --------------------------------------------------------------
    console.log("\n-- upright (368x448 portrait), scanning each row and the whole face --");
    const { samples: up, tEnd: tEndUp } = await sweepOrientation(dev, true, tEnd + DOUBLE_PRESS_WINDOW_MS + 200);
    histogram("whole face (hours+minutes union)", up.map((s) => s.overall.diff));
    histogram("hours row alone", up.map((s) => s.hours!.diff));
    histogram("minutes row alone", up.map((s) => s.minutes!.diff));

    const upWorstWhole = worstN(up, 3, (s) => s.overall);
    console.log("  worst 3 (upright, WHOLE-FACE margin difference):");
    for (const s of upWorstWhole) {
        console.log(`    ${String(s.h).padStart(2, "0")}:${String(s.m).padStart(2, "0")}  pattern=${patternLabel(s.h, s.m)}  left=${s.overall.left.toFixed(2)}  right=${s.overall.right.toFixed(2)}  diff=${s.overall.diff.toFixed(2)}px`);
    }
    const upWorstRow = worstN(up, 5, (s) => (s.hours!.diff >= s.minutes!.diff ? s.hours! : s.minutes!));
    console.log("  worst 5 (upright, WORST SINGLE ROW - this is what a viewer actually looks at, one row at a time):");
    for (const s of upWorstRow) {
        const which = s.hours!.diff >= s.minutes!.diff ? "hours" : "minutes";
        const m = which === "hours" ? s.hours! : s.minutes!;
        console.log(`    ${String(s.h).padStart(2, "0")}:${String(s.m).padStart(2, "0")}  pattern=${patternLabel(s.h, s.m)}  row=${which}  left=${m.left.toFixed(2)}  right=${m.right.toFixed(2)}  diff=${m.diff.toFixed(2)}px`);
    }
    const upClean = up.find((s) => patternLabel(s.h, s.m) === "....");
    if (upClean) console.log(`  reference, no '1' anywhere: ${String(upClean.h).padStart(2, "0")}:${String(upClean.m).padStart(2, "0")}  hours diff=${upClean.hours!.diff.toFixed(2)}  minutes diff=${upClean.minutes!.diff.toFixed(2)}`);

    // ---- worst-case renders, so the numbers are anchored to pictures --------
    // Time keeps running forward from wherever the sweeps left it (tEndUp) -
    // emu_tick()'s nowMs must never go backward, or the debounce/double-press
    // windows above see a huge unsigned-wrap "elapsed" time instead of a
    // small real one.
    console.log("\n-- writing worst-case previews --");
    let shotT = tEndUp;
    for (const s of landWorst.slice(0, 2)) {
        shotT = await gotoAndShoot(dev, false, s.h, s.m, shotT, `clock-spacing-worst-landscape-${String(s.h).padStart(2, "0")}${String(s.m).padStart(2, "0")}.png`,
            `long-ways ${String(s.h).padStart(2, "0")}:${String(s.m).padStart(2, "0")}, pattern ${patternLabel(s.h, s.m)}, overall diff ${s.overall.diff.toFixed(1)}px`);
    }
    for (const s of upWorstRow.slice(0, 2)) {
        shotT = await gotoAndShoot(dev, true, s.h, s.m, shotT, `clock-spacing-worst-upright-${String(s.h).padStart(2, "0")}${String(s.m).padStart(2, "0")}.png`,
            `upright ${String(s.h).padStart(2, "0")}:${String(s.m).padStart(2, "0")}, pattern ${patternLabel(s.h, s.m)}`);
    }

    console.log("\ndone.");
}

// Re-dials to a specific (h, m) from a fresh open, for a one-off screenshot -
// simpler than threading the sweep's own running position through, and this
// only runs a handful of times. Takes and returns the running simulated
// clock explicitly (see the comment at its call site above) rather than
// resetting to 0, which would run emu_tick()'s nowMs backward.
async function gotoAndShoot(dev: Device, upright: boolean, h: number, m: number, t0: number, name: string, what: string): Promise<number> {
    let tt = t0 + DOUBLE_PRESS_WINDOW_MS + 300;
    tt = settleGravity(dev, tt, upright ? [0, 1, 0] : [1, 0, 0], 200);
    tt = doublePressPWR(dev, tt); // open - seed read back below, not assumed
    const [h0, m0] = readSeed(dev);
    for (let i = 0; i < h0; i++) tt = tapZone(dev, upright, "H-", tt); // -> 0
    for (let i = 0; i < m0; i++) tt = tapZone(dev, upright, "M-", tt); // -> 0
    for (let i = 0; i < h; i++) tt = tapZone(dev, upright, "H+", tt);
    for (let i = 0; i < m; i++) tt = tapZone(dev, upright, "M+", tt);
    tt = shortPressPWR(dev, tt); // close - see sweepOrientation's own comment on why this matters
    tt += 50; dev.tick(tt); // let the post-commit paint settle before the shot
    // Shoot the CLOSED, running face - chevrons gone - so the preview matches
    // what the owner actually sees day to day, not the editing UI.
    await writeShot(name, dev.fbSnapshot(), upright, what);
    return tt;
}

main();
