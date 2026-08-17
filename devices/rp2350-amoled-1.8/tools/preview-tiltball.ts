/**
 * Renders the tilt-a-ball (firmware/apps/tiltball.c) from the REAL compiled
 * firmware, headlessly, for the owner to judge by eye. Same technique as
 * tools/preview-breakout.ts and every file under emulator/wasm/tests/: load
 * emu.wasm, drive it, read the framebuffer back. Nothing here reimplements
 * the app - the dish, the hole, the ball and the ripple in these PNGs are
 * the same C that runs on the board.
 *
 *   bun run emulator/wasm/build.ts
 *   bun tools/preview-tiltball.ts
 *
 * The tilt-a-ball is in the default app table unconditionally
 * (runtime_core.c) - no build flag needed.
 *
 * Writes preview/tiltball-<shot>.png, LANDSCAPE (448x368, the way this app
 * is held), the rotation applied at encode time only.
 *
 * THE SHOTS, and why these five:
 *
 *   rest        held flat, settled. The ball at the dish's own centre.
 *   gentle-tip  8 degrees toward the hole - a careful, adult-scale nudge
 *               that moves the ball but falls well short of the hole (see
 *               feature-tiltball.ts's own "15deg falls short" finding).
 *   hard-tip    35 degrees toward the hole, held - close to or past the
 *               capture threshold, so this shot is taken at the moment the
 *               ball is deep in its approach.
 *   captured    stopped partway through the fall-in animation (roughly the
 *               midpoint of CAPTURE_ANIM_MS): the ball mid-shrink, the
 *               ripple already expanding from the hole.
 *   respawning  partway through growing back at the dish's centre after
 *               the hidden pause.
 *
 * Tilt is injected through emu_sensor_vector (emu_abi.h), the same shared
 * "gravity" sensor the tests drive, so these renders are reproducible
 * rather than captured off a moving hand.
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
const SENSOR_GRAVITY = 1; // emu_device()'s sensors array: 0 shake, 1 gravity
const FRAME_MS = 16;

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
    const APP_TILTBALL = apps.indexOf("tiltball");
    if (APP_TILTBALL < 0) {
        throw new Error(
            "this emu.wasm has no tiltball app in its table - rebuild it: " +
            "bun run emulator/wasm/build.ts",
        );
    }

    e.emu_tick(0);
    e.emu_app_switch(APP_TILTBALL);
    e.emu_tick(FRAME_MS);
    if (e.emu_app_current() !== APP_TILTBALL) throw new Error("did not land in tiltball");
    let t = FRAME_MS;
    let soundSeqSeen = e.emu_sound_play_seq() >>> 0;
    return {
        // Holds a tilt for `ms` of frames.
        hold(deg: number, phiDeg: number, ms: number) {
            const th = (deg * Math.PI) / 180, p = (phiDeg * Math.PI) / 180;
            const g: [number, number, number] = [
                Math.sin(th) * Math.cos(p),
                Math.sin(th) * Math.sin(p),
                Math.cos(th),
            ];
            const end = t + ms;
            while (t < end) { e.emu_sensor_vector(SENSOR_GRAVITY, g[0], g[1], g[2]); t += FRAME_MS; e.emu_tick(t); }
        },
        // Holds a tilt UNTIL the capture sound fires (emu_sound_play_seq()
        // bumps), then returns - the same "tick until the real edge, don't
        // guess a duration" technique feature-tiltball.ts uses.
        holdUntilCaptured(deg: number, phiDeg: number, maxMs: number) {
            const th = (deg * Math.PI) / 180, p = (phiDeg * Math.PI) / 180;
            const g: [number, number, number] = [
                Math.sin(th) * Math.cos(p),
                Math.sin(th) * Math.sin(p),
                Math.cos(th),
            ];
            const before = e.emu_sound_play_seq() >>> 0;
            const end = t + maxMs;
            while (t < end) {
                e.emu_sensor_vector(SENSOR_GRAVITY, g[0], g[1], g[2]);
                t += FRAME_MS;
                e.emu_tick(t);
                if ((e.emu_sound_play_seq() >>> 0) !== before) { soundSeqSeen = before; return; }
            }
            throw new Error(`capture did not trigger within ${maxMs}ms`);
        },
        fb(): Uint8Array { return new Uint8Array(memory.buffer, e.emu_fb(), PANEL_W * PANEL_H * 2).slice(); },
    };
}

// ---- PNG (landscape, 24-bit RGB), same machinery as preview-breakout.ts --
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
    const path = join(PREVIEW_DIR, `tiltball-${shot}.png`);
    await Bun.write(path, out);
    console.log(`wrote ${path}`);
}

async function main() {
    {
        const dev = await loadDevice();
        dev.hold(0, 0, 3000);
        await write("rest", dev.fb());
    }
    {
        const dev = await loadDevice();
        dev.hold(8, 90, 3000);
        await write("gentle-tip", dev.fb());
    }
    {
        const dev = await loadDevice();
        dev.hold(35, 90, 260); // brief: at this angle the ball reaches the
                                // hole's trigger radius well before 700ms,
                                // so this shot has to stop short to catch it
                                // still mid-approach rather than already
                                // captured
        await write("hard-tip", dev.fb());
    }
    {
        const dev = await loadDevice();
        dev.holdUntilCaptured(40, 90, 2000);
        dev.hold(0, 0, 190); // ~halfway through CAPTURE_ANIM_MS (380ms)
        await write("captured", dev.fb());
    }
    {
        const dev = await loadDevice();
        dev.holdUntilCaptured(40, 90, 2000);
        dev.hold(0, 0, 380 + 260 + 130); // CAPTURE_ANIM_MS + HIDDEN_PAUSE_MS + half of RESPAWN_ANIM_MS
        await write("respawning", dev.fb());
    }
}

main();
