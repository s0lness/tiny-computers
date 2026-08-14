/**
 * Renders Connect Four (firmware/apps/four.c) from the REAL compiled
 * firmware, headlessly, for the owner to judge by eye. Same technique as
 * tools/preview-menu-icons.ts and every file under emulator/wasm/tests/:
 * load emu.wasm, drive it through emu_touch/emu_tick, read the framebuffer
 * back. Nothing here reimplements the app.
 *
 * It exists because this app's layout has been decided by looking at renders
 * rather than by argument, twice - full height against a hopper, then a wide
 * slab against a narrow one - and each of those comparisons meant building
 * two variants and rendering both:
 *
 *   bun run emulator/wasm/build.ts
 *   bun tools/preview-four.ts --label whatever-this-one-is
 *
 * Both of those questions are settled now and four.c carries no layout
 * variant, so --label is just a name for the run; pass EMU_EXTRA_DEFINES to
 * build.ts if a future one needs comparing again.
 *
 * Writes preview/four-<shot>-<label>.png, LANDSCAPE (448x368, the way the
 * device is held for this app) rather than in the panel's own portrait
 * orientation - the rotation is applied at encode time only.
 *
 * THE SHOT THAT DECIDES THINGS is `steady`: nobody touching the glass, the
 * hand-off animation long faded. That is the frame where the only things
 * left saying whose turn it is are the arrow and the slab's tint, so it is
 * the one that answers whether the turn cue survived losing the waiting
 * piece.
 *
 * Deliberately does NOT open a serial port or touch a physical device.
 */
import { readFileSync, mkdirSync } from "node:fs";
import { join } from "node:path";

const ROOT = join(import.meta.dir, "..");
const WASM_PATH = join(ROOT, "emulator", "wasm", "dist", "emu.wasm");
const PREVIEW_DIR = join(ROOT, "preview");

const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const LAND_H = PANEL_W; // 368
const APP_FOUR = 3;

const args = process.argv.slice(2);
const labelIdx = args.indexOf("--label");
const LABEL = labelIdx >= 0 ? args[labelIdx + 1]! : "current";

// four.c's own column partition. Only the WIDTH is needed here (the touch
// target is the full height of the column), and it is the one thing that is
// identical in both layout variants: seven columns over the whole canvas.
const COLS = 7;
const CELL_W = LAND_W / COLS;
const colX = (c: number) => CELL_W / 2 + c * CELL_W;

async function loadDevice() {
    let memory!: WebAssembly.Memory;
    const dec = new TextDecoder();
    const log: string[] = [];
    const inst = await WebAssembly.instantiate(await WebAssembly.compile(readFileSync(WASM_PATH)), {
        env: {
            js_log(p: number, l: number) { log.push(dec.decode(new Uint8Array(memory.buffer, p, l))); },
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
    e.emu_tick(0);
    e.emu_app_switch(APP_FOUR);
    e.emu_tick(10);
    if (e.emu_app_current() !== APP_FOUR) throw new Error("did not land in four");
    return {
        // Landscape in, panel out: gfx.h's mapping is (lx,ly) -> (PANEL_W-1-ly, lx).
        touch(down: boolean, lx: number, ly: number, nowMs: number) {
            e.emu_touch(down ? 1 : 0, PANEL_W - 1 - Math.round(ly), Math.round(lx));
            e.emu_tick(nowMs);
        },
        fb(): Uint8Array { return new Uint8Array(memory.buffer, e.emu_fb(), PANEL_W * PANEL_H * 2).slice(); },
        drainLog(): string[] { const out = log.slice(); log.length = 0; return out; },
    };
}

// ---- PNG (landscape, 24-bit RGB), same machinery as the test files' -----
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
    const idx = (Math.round(lx) * PANEL_W + (PANEL_W - 1 - Math.round(ly))) * 2;
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
    const path = join(PREVIEW_DIR, `four-${shot}-${LABEL}.png`);
    await Bun.write(path, out);
    console.log(`wrote ${path}`);
}

async function main() {
    const dev = await loadDevice();
    let t = 1000;
    const THUMB_LY = 200;
    const idle = (ms: number) => { for (let i = 0; i < ms; i += 15) { t += 15; dev.touch(false, 0, 0, t); } };
    const hold = (col: number, ms: number) => { for (let i = 0; i < ms; i += 15) { t += 15; dev.touch(true, colX(col), THUMB_LY, t); } };
    const play = (col: number) => { hold(col, 140); idle(1600); };

    // An opening board with a few pieces in it, so the highlight has a stack
    // to sit on and the board does not read as an empty template. Two pieces
    // each into two columns cannot make a four, so no game ends underneath
    // the screenshots.
    for (const c of [2, 4, 2, 4, 5, 1]) play(c);
    dev.drainLog();

    // THE DECIDING FRAME: nobody touching, hand-off long faded.
    idle(1200);
    await write("steady", dev.fb());

    // A thumb on the glass, mid-game: chute, landing ring, arrow.
    hold(3, 300);
    await write("highlight", dev.fb());
    idle(2000);

    console.log(`(variant label: ${LABEL})`);
}

main();
