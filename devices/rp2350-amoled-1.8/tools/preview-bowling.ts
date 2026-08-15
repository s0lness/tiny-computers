/**
 * Renders bowling (firmware/apps/bowling.c) from the REAL compiled
 * firmware, headlessly, for the owner to judge by eye. Same technique as
 * tools/preview-four.ts and every file under emulator/wasm/tests/: load
 * emu.wasm, drive it through emu_touch/emu_tick, read the framebuffer back.
 * Nothing here reimplements the app.
 *
 *   bun run emulator/wasm/build.ts
 *   bun tools/preview-bowling.ts
 *
 * Writes preview/bowling-<shot>.png, LANDSCAPE (448x368, the way the
 * device is held for this app) rather than in the panel's own portrait
 * orientation - the rotation is applied at encode time only.
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

// Mirrors of bowling.c's own constants - see feature-bowling.ts's comment
// on why these are derived the same way rather than hand-copied.
const BEZEL = 10;
const SAFE_X0 = BEZEL, SAFE_X1 = LAND_W - BEZEL, SAFE_Y0 = BEZEL, SAFE_Y1 = LAND_H - BEZEL;
const LANE_CY = (SAFE_Y0 + SAFE_Y1) / 2;
const BALL_START_X = SAFE_X0 + 58;
const PIN_ROW_GAP = 36, PIN_COL_GAP = 34;
const PIN_BACK_X = SAFE_X1 - 62;
const PIN_APEX_X = PIN_BACK_X - 2 * PIN_ROW_GAP;
const ARM_MS = 40;
const RELEASE_GRACE_MS = 300;
const PAUSE_STRIKE_MS = 1350;

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

    const jsonBytes = new Uint8Array(memory.buffer, e.emu_device());
    let end = 0; while (jsonBytes[end] !== 0) end++;
    const apps: string[] = JSON.parse(dec.decode(jsonBytes.subarray(0, end))).apps || [];
    const APP_BOWLING = apps.indexOf("bowling");
    if (APP_BOWLING < 0) {
        throw new Error(
            "this emu.wasm has no bowling app in its table - rebuild it: " +
            "bun run emulator/wasm/build.ts",
        );
    }

    e.emu_tick(0);
    e.emu_app_switch(APP_BOWLING);
    e.emu_tick(10);
    // Against the NAME, not against the index this tool just passed in. The
    // old assertion compared APP_BOWLING to itself, so it stayed green while
    // the eleven-app merge silently made index 4 the spirit level.
    const landed = apps[e.emu_app_current()];
    if (landed !== "bowling") throw new Error(`asked for index ${APP_BOWLING} and landed in "${landed}"`);
    return {
        touch(down: boolean, lx: number, ly: number, nowMs: number) {
            e.emu_touch(down ? 1 : 0, PANEL_W - 1 - Math.round(ly), Math.round(lx));
            e.emu_tick(nowMs);
        },
        tick(nowMs: number) { e.emu_tick(nowMs); },
        fb(): Uint8Array { return new Uint8Array(memory.buffer, e.emu_fb(), PANEL_W * PANEL_H * 2).slice(); },
        drainLog(): string[] { const out = log.slice(); log.length = 0; return out; },
    };
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

// ---- PNG (landscape, 24-bit RGB), same machinery as preview-four.ts's ----
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
    const path = join(PREVIEW_DIR, `bowling-${shot}.png`);
    await Bun.write(path, out);
    console.log(`wrote ${path}`);
}

async function main() {
    const dev = await loadDevice();
    let t = 1000;
    const idle = (ms: number) => { for (let i = 0; i < ms; i += 15) { t += 15; dev.touch(false, 0, 0, t); } };
    const hold = (x: number, y: number, ms: number) => { for (let i = 0; i < ms; i += 15) { t += 15; dev.touch(true, x, y, t); } };
    // A REAL, gradual drag (interpolated, like a real flick), not a
    // teleport - a same-tick jump to a far target produces a near-infinite
    // instantaneous velocity that clamps to MAX_LAUNCH_SPEED every time,
    // which is fine for demonstrating a hard throw but useless for showing
    // "mid-flight" at a legible speed.
    const drag = (x0: number, y0: number, x1: number, y1: number, ms: number) => {
        const steps = Math.max(1, Math.round(ms / 15));
        for (let i = 1; i <= steps; i++) {
            const u = i / steps;
            t += 15;
            dev.touch(true, x0 + (x1 - x0) * u, y0 + (y1 - y0) * u, t);
        }
    };
    const lift = () => { for (let i = 0; i < RELEASE_GRACE_MS + 60; i += 15) { t += 15; dev.touch(false, 0, 0, t); } };
    const BALL_START_Y = LANE_CY;

    // THE OPENING RACK: the settled entry screen. Six standing pins, the
    // ball at rest. This is the frame she sees every single time.
    idle(300);
    await write("ready", dev.fb());

    // A GRAB: the ball tracking a finger mid-drag, before release - the
    // only feedback that says "you have picked it up" (bowling.c section
    // 2).
    hold(BALL_START_X + 5, BALL_START_Y, ARM_MS + 20);
    drag(BALL_START_X + 5, BALL_START_Y, BALL_START_X + 90, BALL_START_Y - 20, 150);
    await write("held", dev.fb());
    lift();

    // MID-FLIGHT: a fresh, moderate throw, captured shortly after release
    // while the ball is still well short of the pins.
    dev.drainLog();
    hold(BALL_START_X + 5, BALL_START_Y, ARM_MS + 20);
    drag(BALL_START_X + 5, BALL_START_Y, BALL_START_X + 130, BALL_START_Y, 260);
    lift();
    idle(90);
    await write("flight", dev.fb());

    // A KNOCKDOWN: the moment several pins are mid-topple, some standing,
    // some falling, one lying flat - the picture that has to read as
    // "this is happening" without a word. Continues the SAME throw rather
    // than starting a new one.
    idle(260);
    const midLog = dev.drainLog().join("");
    console.log(`    mid-throw log so far: ${midLog.trim() || "(nothing yet)"}`);
    await write("knockdown", dev.fb());

    // Let this throw fully resolve and reset back to a fresh rack before
    // the strike attempt, so that shot is not confused by leftover fallen
    // pins - PAUSE_STRIKE_MS (the longer of the two) plus the whole reset
    // sequence, with margin.
    idle(PAUSE_STRIKE_MS + 2000);

    // A STRIKE ATTEMPT: aimed dead centre with real speed, so the domino
    // chain has its best chance of taking all six down, and the breathing
    // ring gets its moment in PH_PAUSE.
    dev.drainLog();
    hold(BALL_START_X + 5, BALL_START_Y, ARM_MS + 20);
    drag(BALL_START_X + 5, BALL_START_Y, BALL_START_X + 170, BALL_START_Y, 150);
    lift();
    idle(1200);
    const strikeLog = dev.drainLog().join("");
    console.log(`    strike attempt log: ${strikeLog.trim() || "(no resolution logged yet)"}`);
    await write("resolved", dev.fb());
    if (strikeLog.includes("strike=1")) {
        idle(300);
        await write("strike-ring", dev.fb());
    } else {
        console.log("    (not a strike this run - physics is physics; resolved.png still shows the outcome)");
    }

    console.log("\ndone.");
}

main();
