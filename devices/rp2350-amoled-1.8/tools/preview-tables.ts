/**
 * Renders multiplication-tables practice (firmware/apps/tables.c) from the
 * REAL compiled firmware, headlessly, for the owner to judge by eye. Same
 * technique as tools/preview-tiltball.ts: load emu.wasm, drive it, read the
 * framebuffer back. Nothing here reimplements the app - the numpad, the
 * loupe, the question and the counters in these PNGs are the same C that
 * runs on the board.
 *
 *   bun run emulator/wasm/build.ts
 *   bun tools/preview-tables.ts
 *
 * Resolves the app index by NAME from emu_device(), never a hardcoded
 * number - the same discipline every preview-*.ts in this directory uses,
 * because a hardcoded index silently points at the wrong app the moment
 * the table is reordered.
 *
 * Writes preview/tables-<shot>.png, LANDSCAPE (448x368).
 *
 * THE SHOTS, and why these six:
 *
 *   rest         fresh entry - the question, an empty answer, all three
 *                counters at zero.
 *   mid-drag     a finger held on a numpad digit - the LOUPE, mid-gesture.
 *                A still PNG cannot show a gesture moving, but it can show
 *                the one frame that matters: the bubble open, showing the
 *                magnified digit, with the same cell lit in the grid below
 *                it - which is the entire point of the redesign this app
 *                shipped with (see tables.c's own header comment) and the
 *                one thing no arithmetic-only argument can demonstrate.
 *   typed        two digits typed into the answer, loupe closed (released).
 *   correct      immediately after a correct submit - the brief positive
 *                pause (PHASE_RIGHT).
 *   wrong-retry  immediately after a first wrong submit - the answer just
 *                cleared, same question still showing (PHASE_WRONG_RETRY).
 *   reveal       immediately after a second wrong submit - the correct
 *                product shown in the answer cell (PHASE_WRONG_REVEAL).
 *
 * Touch is injected through emu_touch (emu_abi.h), panel coordinates, the
 * same ABI the tests use - not a script re-implementing tables.c's own
 * hit-testing.
 *
 * Deliberately does NOT open a serial port or touch a physical device.
 */
import { readFileSync, mkdirSync } from "node:fs";
import { join } from "node:path";

const ROOT = join(import.meta.dir, "..");
const WASM_PATH = join(ROOT, "emulator", "wasm", "dist", "emu.wasm");
const PREVIEW_DIR = join(ROOT, "preview");

const PANEL_W = 368, PANEL_H = 448;
const LAND_W = PANEL_H, LAND_H = PANEL_W; // 448 x 368
const FRAME_MS = 16;

// tables.c's own layout constants, lifted (see this file's header on why
// that is the right discipline rather than re-deriving them).
const OX = 10, OY = 10;
const NUMPAD_X0 = OX, NUMPAD_Y0 = OY + 92;
const CELL_W = 99, CELL_H = 64, NUMPAD_COLS = 3;
const CELL_BACK = 9, CELL_ZERO = 10, CELL_CHECK = 11;
const cellCx = (c: number) => NUMPAD_X0 + (c % NUMPAD_COLS) * CELL_W + CELL_W / 2;
const cellCy = (c: number) => NUMPAD_Y0 + Math.floor(c / NUMPAD_COLS) * CELL_H + CELL_H / 2;
const digitCell = (d: number) => (d === 0 ? CELL_ZERO : d - 1);

function landToPanel(lx: number, ly: number): [number, number] {
    return [PANEL_W - 1 - Math.round(ly), Math.round(lx)];
}

async function loadDevice() {
    let memory!: WebAssembly.Memory;
    const dec = new TextDecoder();
    const fwLog: string[] = [];
    const inst = await WebAssembly.instantiate(await WebAssembly.compile(readFileSync(WASM_PATH)), {
        env: {
            js_log(ptr: number, len: number) { fwLog.push(dec.decode(new Uint8Array(memory.buffer, ptr, len))); },
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
    memory = inst.exports.memory as WebAssembly.Memory;
    const e = inst.exports as any;
    if (e.emu_init() !== 1) throw new Error("emu_init() failed");

    const jsonBytes = new Uint8Array(memory.buffer, e.emu_device());
    let end = 0; while (jsonBytes[end] !== 0) end++;
    const apps: string[] = JSON.parse(dec.decode(jsonBytes.subarray(0, end))).apps || [];
    const APP_TABLES = apps.map((a) => a.toLowerCase()).indexOf("tables");
    if (APP_TABLES < 0) {
        throw new Error("this emu.wasm has no 'tables' app in its table - rebuild it: bun run emulator/wasm/build.ts");
    }

    e.emu_tick(0);
    e.emu_app_switch(APP_TABLES);
    e.emu_tick(FRAME_MS);
    if (e.emu_app_current() !== APP_TABLES) throw new Error("did not land in tables");
    let t = FRAME_MS;
    fwLog.length = 0;
    return {
        tick(ms: number) { const end2 = t + ms; while (t < end2) { t += FRAME_MS; e.emu_tick(t); } },
        touch(down: boolean, px: number, py: number) { e.emu_touch(down ? 1 : 0, px, py); },
        // Press a numpad cell, holding long enough to arm and confirm
        // (tables.c's ARM_MS/COMMIT_CONFIRM_MS), leaving it DOWN - for the
        // mid-drag shot, which wants the loupe caught open.
        pressAndHold(cell: number, ms: number) {
            const [px, py] = landToPanel(cellCx(cell), cellCy(cell));
            const end2 = t + ms;
            while (t < end2) { t += FRAME_MS; e.emu_touch(1, px, py); e.emu_tick(t); }
        },
        // A full press-hold-release on one cell.
        tapCell(cell: number) {
            const [px, py] = landToPanel(cellCx(cell), cellCy(cell));
            const end2 = t + 200;
            while (t < end2) { t += FRAME_MS; e.emu_touch(1, px, py); e.emu_tick(t); }
            e.emu_touch(0, 0, 0);
            const end3 = t + 400;
            while (t < end3) { t += FRAME_MS; e.emu_tick(t); }
        },
        drainLog(): string[] { const o = fwLog.slice(); fwLog.length = 0; return o; },
        fb(): Uint8Array { return new Uint8Array(memory.buffer, e.emu_fb(), PANEL_W * PANEL_H * 2).slice(); },
    };
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

function typeDigits(dev: Device, digits: number[]) {
    for (const d of digits) dev.tapCell(digitCell(d));
}

// ---- PNG (landscape, 24-bit RGB), same machinery as preview-tiltball.ts --
function crc32Table(): Uint32Array {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
    return t;
}
const CRC = crc32Table();
function crc32(b: Uint8Array): number {
    let c = 0xffffffff;
    for (let i = 0; i < b.length; i++) c = CRC[(c ^ b[i]!) & 0xff]! ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
}
function adler32(d: Uint8Array): number {
    let a = 1, b = 0;
    for (let i = 0; i < d.length; i++) { a = (a + d[i]!) % 65521; b = (b + a) % 65521; }
    return ((b << 16) | a) >>> 0;
}
function chunk(type: string, data: Uint8Array): Uint8Array {
    const tb = new TextEncoder().encode(type);
    const body = new Uint8Array(tb.length + data.length);
    body.set(tb, 0); body.set(data, tb.length);
    const out = new Uint8Array(4 + body.length + 4);
    const dv = new DataView(out.buffer);
    dv.setUint32(0, data.length, false);
    out.set(body, 4);
    dv.setUint32(4 + body.length, crc32(body), false);
    return out;
}
function landPx(fb: Uint8Array, lx: number, ly: number): [number, number, number] {
    const idx = (lx * PANEL_W + (PANEL_W - 1 - ly)) * 2;
    const v = (fb[idx]! << 8) | fb[idx + 1]!;
    return [
        Math.round((((v >> 11) & 0x1f) * 255) / 31),
        Math.round((((v >> 5) & 0x3f) * 255) / 63),
        Math.round(((v & 0x1f) * 255) / 31),
    ];
}
async function write(shot: string, fb: Uint8Array) {
    const raw = new Uint8Array((LAND_W * 3 + 1) * LAND_H);
    for (let ly = 0; ly < LAND_H; ly++) {
        const off = ly * (LAND_W * 3 + 1);
        raw[off] = 0;
        for (let lx = 0; lx < LAND_W; lx++) {
            const [r, g, b] = landPx(fb, lx, ly);
            const o = off + 1 + lx * 3;
            raw[o] = r; raw[o + 1] = g; raw[o + 2] = b;
        }
    }
    const z = Bun.deflateSync(raw);
    const idat = new Uint8Array(2 + z.length + 4);
    idat[0] = 0x78; idat[1] = 0x9c;
    idat.set(z, 2);
    new DataView(idat.buffer).setUint32(2 + z.length, adler32(raw), false);
    const ihdr = new Uint8Array(13);
    const dv = new DataView(ihdr.buffer);
    dv.setUint32(0, LAND_W, false); dv.setUint32(4, LAND_H, false);
    ihdr[8] = 8; ihdr[9] = 2;
    const sig = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    const parts = [sig, chunk("IHDR", ihdr), chunk("IDAT", idat), chunk("IEND", new Uint8Array(0))];
    const total = parts.reduce((s, p) => s + p.length, 0);
    const out = new Uint8Array(total);
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    mkdirSync(PREVIEW_DIR, { recursive: true });
    const path = join(PREVIEW_DIR, `tables-${shot}.png`);
    await Bun.write(path, out);
    console.log(`wrote ${path}`);
}

async function main() {
    {
        const dev = await loadDevice();
        await write("rest", dev.fb());
    }
    {
        const dev = await loadDevice();
        // Hold on digit "8" (cell 7) long enough to arm and confirm - the
        // loupe should be open, showing "8" magnified, with cell 7 lit.
        dev.pressAndHold(7, 200);
        await write("mid-drag", dev.fb());
    }
    {
        const dev = await loadDevice();
        typeDigits(dev, [4, 2]);
        await write("typed", dev.fb());
    }
    {
        // Find the real product by giving up twice on a probe device, then
        // type it correctly on a fresh one (same first question - see
        // feature-tables.ts's own comment on why this is deterministic).
        const probe = await loadDevice();
        typeDigits(probe, [9, 9]);
        probe.tapCell(CELL_CHECK);
        typeDigits(probe, [9, 9]);
        probe.tapCell(CELL_CHECK);
        const log = probe.drainLog();
        const m = log.join("\n").match(/tables: (\d+) x (\d+) = (\d+), gave up/);
        if (!m) throw new Error("could not read the product off the probe device: " + log.join(" | "));
        const product = Number(m[3]);
        const digits = product < 10 ? [product] : [Math.floor(product / 10), product % 10];

        const dev = await loadDevice();
        typeDigits(dev, digits);
        dev.tapCell(CELL_CHECK);
        await write("correct", dev.fb());
    }
    {
        const dev = await loadDevice();
        typeDigits(dev, [9, 9]); // certainly wrong in the 6/7/8 tables x 1..10
        dev.tapCell(CELL_CHECK);
        await write("wrong-retry", dev.fb());
    }
    {
        const dev = await loadDevice();
        typeDigits(dev, [9, 9]);
        dev.tapCell(CELL_CHECK);
        typeDigits(dev, [9, 9]);
        dev.tapCell(CELL_CHECK);
        await write("reveal", dev.fb());
    }
}

main();
