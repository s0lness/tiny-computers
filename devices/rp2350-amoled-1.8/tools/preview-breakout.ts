/**
 * Renders breakout (firmware/apps/breakout.c) from the REAL compiled
 * firmware, headlessly, for the owner to judge by eye - same technique as
 * tools/preview-level.ts and every file under emulator/wasm/tests/: load
 * emu.wasm, drive it, read the framebuffer back. Nothing here reimplements
 * the app; the wall, the ball and the paddle in these PNGs are the same C
 * that runs on the board.
 *
 *   bun run emulator/wasm/build.ts
 *   bun tools/preview-breakout.ts
 *
 * Breakout is in the default app table unconditionally (runtime_core.c) -
 * no build flag needed.
 *
 * Writes preview/breakout-<shot>.png, LANDSCAPE (448x368, the way this app
 * is held), the rotation applied at encode time only.
 *
 * THE SHOTS, and why these four:
 *
 *   entry        the picture at switch-in: full ten-brick wall on its arc,
 *                ball mid-field, paddle centred (no tilt has been sent).
 *   midgame      some time in, untouched: a few bricks gone, the ball and
 *                paddle wherever the physics alone put them.
 *   celebrating  mid-regrow wave, shortly after the wall was fully cleared:
 *                some bricks back, some still growing, some not yet
 *                started - the travelling-wave celebration in progress.
 *   regrown      the wall complete again, unattended.
 *
 * The run is driven with no touch and no tilt throughout (the paddle sits
 * at its default centred rest position the whole time) - this is
 * deliberately the "nobody is holding it" case breakout.c's own header
 * comment argues has to still be worth watching. tools/preview-level.ts's
 * equivalent instead varies tilt, because that IS its app's whole input;
 * this app's most interesting unattended behaviour is watching the clock
 * alone clear and regrow a wall.
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
const FRAME_MS = 20;

async function loadDevice() {
    let memory!: WebAssembly.Memory;
    const dec = new TextDecoder();
    const inst = await WebAssembly.instantiate(await WebAssembly.compile(readFileSync(WASM_PATH)), {
        env: {
            js_log(_p: number, _l: number) { /* quiet */ },
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
    const APP_BREAKOUT = apps.indexOf("breakout");
    if (APP_BREAKOUT < 0) {
        throw new Error(
            "this emu.wasm has no breakout app in its table - rebuild it: " +
            "bun run emulator/wasm/build.ts",
        );
    }

    e.emu_tick(0);
    e.emu_app_switch(APP_BREAKOUT);
    e.emu_tick(FRAME_MS);
    if (e.emu_app_current() !== APP_BREAKOUT) throw new Error("did not land in breakout");
    let t = FRAME_MS;
    return {
        now(): number { return t; },
        step(ms: number) {
            const end2 = t + ms;
            while (t < end2) { t += FRAME_MS; e.emu_tick(t); }
        },
        fb(): Uint8Array { return new Uint8Array(memory.buffer, e.emu_fb(), PANEL_W * PANEL_H * 2).slice(); },
    };
}

// ---- brick-ink measure, to find the clear/regrow moments -----------------
function isWhiteOrBlack(r: number, g: number, b: number): "white" | "black" | "colour" {
    if (r >= 30 && g >= 60 && b >= 30) return "white";
    if (r <= 1 && g <= 1 && b <= 1) return "black";
    return "colour";
}
function brickInk(fb: Uint8Array): number {
    let n = 0;
    for (let i = 0; i < PANEL_W * PANEL_H; i++) {
        const v = (fb[i * 2]! << 8) | fb[i * 2 + 1]!;
        const r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f;
        if (isWhiteOrBlack(r, g, b) === "colour") n++;
    }
    return n;
}

// ---- PNG (landscape, 24-bit RGB), same machinery as preview-level.ts -----
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
    const path = join(PREVIEW_DIR, `breakout-${shot}.png`);
    await Bun.write(path, out);
    console.log(`wrote ${path}`);
}

async function main() {
    const dev = await loadDevice();
    await write("entry", dev.fb());

    const initialInk = brickInk(dev.fb());

    // midgame: step until a few bricks are visibly gone, capped so this
    // never hangs if the constants ever change enough to make that slow.
    let midgameShot: Uint8Array | null = null;
    for (let i = 0; i < 3000 && !midgameShot; i++) {
        dev.step(200);
        const ink = brickInk(dev.fb());
        if (ink < initialInk * 0.85) midgameShot = dev.fb();
    }
    await write("midgame", midgameShot ?? dev.fb());

    // Keep going until the wall fully clears (ink bottoms out near zero),
    // then capture mid-regrow and fully-regrown. Measured empirically
    // during development at up to ~190s of simulated time for one clear;
    // the cap below is generous, not tuned.
    let clearedAtT: number | null = null;
    for (let i = 0; i < 15000 && clearedAtT === null; i++) {
        dev.step(50);
        if (brickInk(dev.fb()) < initialInk * 0.05) clearedAtT = dev.now();
    }
    if (clearedAtT === null) {
        console.log("did not observe a full clear within the time budget - skipping the celebrating/regrown shots");
        return;
    }

    dev.step(500); // partway through the travelling-wave regrow
    await write("celebrating", dev.fb());

    for (let i = 0; i < 200; i++) {
        dev.step(50);
        if (brickInk(dev.fb()) >= initialInk * 0.95) break;
    }
    await write("regrown", dev.fb());
}

main();
