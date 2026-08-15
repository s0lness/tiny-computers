/**
 * Renders the dino game (firmware/apps/dino.c) from the REAL compiled
 * firmware, headlessly, for the owner to judge by eye - same technique as
 * tools/preview-four.ts and every file under emulator/wasm/tests/: load
 * emu.wasm, drive it through emu_touch/emu_tick, read the framebuffer
 * back. Nothing here reimplements the app or its shapes.
 *
 *   bun run emulator/wasm/build.ts
 *   bun tools/preview-dino.ts
 *
 * Writes preview/dino-<shot>.png, LANDSCAPE (448x368, the way the device
 * is held for this app) rather than in the panel's own portrait
 * orientation - the rotation is applied at encode time only, exactly as
 * gfx.h's own landscape mapping does it in the firmware.
 *
 * Also captures the menu icon (draw_icon_dino, firmware/apps/menu.c) by
 * switching into APP_INDEX_MENU with only the real apps present (no
 * MENU_STUB_APPS build needed - dino is the fifth real app), so the icon
 * is judged in situ next to the other four rather than alone.
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
const APP_DINO = 4; // g_apps[] = { chrono, sketch("draw"), timer, four, dino }
const APP_INDEX_MENU = -1;

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
    return {
        switchTo(index: number) { e.emu_app_switch(index); e.emu_tick(10); },
        touch(down: boolean, nowMs: number) { e.emu_touch(down ? 1 : 0, 184, 224); e.emu_tick(nowMs); },
        tick(nowMs: number) { e.emu_touch(0, 0, 0); e.emu_tick(nowMs); },
        fb(): Uint8Array { return new Uint8Array(memory.buffer, e.emu_fb(), PANEL_W * PANEL_H * 2).slice(); },
        drainLog(): string[] { const out = log.slice(); log.length = 0; return out; },
    };
}

// ---- PNG (landscape, 24-bit RGB), identical machinery to tools/preview-four.ts
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
    const path = join(PREVIEW_DIR, `dino-${shot}.png`);
    await Bun.write(path, out);
    console.log(`wrote ${path}`);
}

async function main() {
    // ---- idle standing pose --------------------------------------------
    {
        const dev = await loadDevice();
        dev.switchTo(APP_DINO);
        await write("idle", dev.fb());
    }

    // ---- mid-run, grounded, mid-stride: tap once (jump), let it land,
    // then a snapshot while she is running with no obstacle nearby. -------
    {
        const dev = await loadDevice();
        dev.switchTo(APP_DINO);
        let t = 10;
        const tapMs = (ms: number) => { for (let e = 0; e < ms; e += 15) { t += 15; dev.touch(true, t); } t += 15; dev.touch(false, t); };
        const idleMs = (ms: number) => { for (let e = 0; e < ms; e += 15) { t += 15; dev.touch(false, t); } };
        tapMs(80); // start the run
        idleMs(900); // land, settle into a run cadence
        await write("running", dev.fb());
    }

    // ---- airborne, near the apex of a jump - the frame that has to read
    // as "jumping", not "floating" or "falling". --------------------------
    {
        const dev = await loadDevice();
        dev.switchTo(APP_DINO);
        let t = 10;
        const tapMs = (ms: number) => { for (let e = 0; e < ms; e += 15) { t += 15; dev.touch(true, t); } t += 15; dev.touch(false, t); };
        const idleMs = (ms: number) => { for (let e = 0; e < ms; e += 15) { t += 15; dev.touch(false, t); } };
        tapMs(80); // start
        idleMs(700); // clear of the initial jump, grounded again
        tapMs(80); // jump
        idleMs(300); // ~near apex (JUMP_AIRTIME_MS/2 = 300ms after the press)
        await write("airborne", dev.fb());
    }

    // ---- an obstacle close up, both kinds, next to the dino for scale.
    // Timed by advancing the clock in small steps and grabbing the first
    // frame after each obstacle-related log line, rather than guessing a
    // fixed offset against the randomised spawn timer. ---------------------
    for (let attempt = 0; attempt < 2; attempt++) {
        const dev = await loadDevice();
        dev.switchTo(APP_DINO);
        let t = attempt === 0 ? 10 : 50000; // different seed (dino.c reseeds
                                             // from nowMs at run start), so
                                             // the two attempts are likely to
                                             // land on different obstacle kinds
        const tapMs = (ms: number) => { for (let e = 0; e < ms; e += 15) { t += 15; dev.touch(true, t); } t += 15; dev.touch(false, t); };
        tapMs(80); // start the run, never jump again - she will meet the
                   // first obstacle and this shot is taken just before that
        let sawHit = false;
        for (let e = 0; e < 6000 && !sawHit; e += 15) {
            t += 15;
            dev.touch(false, t);
            if (dev.drainLog().some((l) => l.includes("dino: hit at"))) sawHit = true;
        }
        // The frame is the frozen collision pose itself - dino.c holds the
        // scene still for DEATH_FREEZE_MS, so this is exactly what a real
        // collision looks like on screen.
        await write(`obstacle-${attempt === 0 ? "a" : "b"}`, dev.fb());
    }

    // ---- the menu icon, in situ next to the other four. -------------------
    {
        const dev = await loadDevice();
        dev.switchTo(APP_INDEX_MENU);
        await write("menu-icon", dev.fb());
    }

    console.log("done");
}

main();
