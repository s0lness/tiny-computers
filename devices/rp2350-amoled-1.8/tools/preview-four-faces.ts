// preview-four-faces: four screenshots of Connect Four's new emoji-style
// tokens (firmware/apps/four.c, section 10 of its own header), for the
// owner to look at without flashing a board. Not a test - feature-four.ts
// already asserts the face pixels exist (see its "the pieces on the board
// are faces, not plain discs" block); this tool exists only to produce the
// four pictures the task asked for: an empty board, a mid-game board with
// both faces present, a token mid-drop, and a winning line.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run tools/preview-four-faces.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "emulator", "wasm", "dist", "emu.wasm");
const PREVIEW_DIR = join(import.meta.dir, "..", "preview");

const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const LAND_H = PANEL_W; // 368
const APP_FOUR = 3;

const COLS = 7;
const BEZEL = 10;
const SAFE_X0 = BEZEL, SAFE_X1 = LAND_W - BEZEL;
const SAFE_W = SAFE_X1 - SAFE_X0;
const CELL = 48;
const BOARD_X0 = SAFE_X0 + Math.floor((SAFE_W - COLS * CELL) / 2);
const BOARD_Y0 = (LAND_H - BEZEL) - 6 - 6 * CELL;
const colX = (c: number) => BOARD_X0 + Math.floor(CELL / 2) + c * CELL;
const rowY = (r: number) => BOARD_Y0 + CELL / 2 + r * CELL;

const RELEASE_GRACE_MS = 300;
const HANDOFF_MS = 420;
const THUMB_LY = 200;

async function loadDevice() {
    const mod = await WebAssembly.compile(readFileSync(WASM_PATH));
    let memory!: WebAssembly.Memory;
    const decoder = new TextDecoder();
    const fwLog: string[] = [];
    const instance = await WebAssembly.instantiate(mod, {
        env: {
            js_log(ptr: number, len: number) { fwLog.push(decoder.decode(new Uint8Array(memory.buffer, ptr, len))); },
            sinf: (x: number) => Math.sin(x), cosf: (x: number) => Math.cos(x),
            atan2f: (y: number, x: number) => Math.atan2(y, x), sqrtf: (x: number) => Math.sqrt(x),
            fabsf: (x: number) => Math.abs(x), floorf: (x: number) => Math.floor(x),
            fmodf: (x: number, y: number) => x % y, powf: (x: number, y: number) => Math.pow(x, y),
            expf: (x: number) => Math.exp(x),
        },
    });
    memory = instance.exports.memory as WebAssembly.Memory;
    const exp = instance.exports as any;
    if (exp.emu_init() !== 1) throw new Error("emu_init() failed");
    return {
        appSwitch(i: number) { exp.emu_app_switch(i); },
        touchLand(down: boolean, lx: number, ly: number, nowMs: number) {
            exp.emu_touch(down ? 1 : 0, PANEL_W - 1 - Math.round(ly), Math.round(lx));
            exp.emu_tick(nowMs);
        },
        fbSnapshot(): Uint8Array {
            const ptr = exp.emu_fb();
            return new Uint8Array(memory.buffer, ptr, PANEL_W * PANEL_H * 2).slice();
        },
        drainLog(): string[] { const out = fwLog.slice(); fwLog.length = 0; return out; },
    };
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

function settle(dev: Device, t0: number, ms: number, step = 15): number {
    let t = t0;
    for (let w = 0; w < ms; w += step) { t += step; dev.touchLand(false, 0, 0, t); }
    return t;
}
function holdLand(dev: Device, lx: number, ly: number, t0: number, ms: number, step = 15): number {
    let t = t0;
    for (let h = 0; h < ms; h += step) { t += step; dev.touchLand(true, lx, ly, t); }
    return t;
}
function playMove(dev: Device, col: number, t0: number): number {
    let t = holdLand(dev, colX(col), THUMB_LY, t0, 140);
    return settle(dev, t, RELEASE_GRACE_MS + 1300);
}

// ---- minimal PNG writer (same machinery as feature-four.ts's own) -------
function crc32Table(): Uint32Array {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
    return t;
}
const CRC_TABLE = crc32Table();
function crc32(buf: Uint8Array): number { let c = 0xffffffff; for (const b of buf) c = CRC_TABLE[(c ^ b) & 0xff]! ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; }
function adler32(d: Uint8Array): number { let a = 1, b = 0; for (const x of d) { a = (a + x) % 65521; b = (b + a) % 65521; } return ((b << 16) | a) >>> 0; }
function zlibWrap(raw: Uint8Array, src: Uint8Array): Uint8Array {
    const out = new Uint8Array(2 + raw.length + 4);
    out[0] = 0x78; out[1] = 0x9c; out.set(raw, 2);
    new DataView(out.buffer).setUint32(2 + raw.length, adler32(src), false);
    return out;
}
function pngChunk(type: string, data: Uint8Array): Uint8Array {
    const tb = new TextEncoder().encode(type);
    const body = new Uint8Array(tb.length + data.length);
    body.set(tb, 0); body.set(data, tb.length);
    const out = new Uint8Array(4 + body.length + 4);
    const dv = new DataView(out.buffer);
    dv.setUint32(0, data.length, false); out.set(body, 4); dv.setUint32(4 + body.length, crc32(body), false);
    return out;
}
function concatBytes(chunks: Uint8Array[]): Uint8Array {
    const total = chunks.reduce((s, c) => s + c.length, 0);
    const out = new Uint8Array(total); let off = 0;
    for (const c of chunks) { out.set(c, off); off += c.length; }
    return out;
}
function landPx(fb: Uint8Array, lx: number, ly: number): [number, number] {
    const px = PANEL_W - 1 - Math.round(ly);
    const py = Math.round(lx);
    const idx = (py * PANEL_W + px) * 2;
    return [fb[idx]!, fb[idx + 1]!];
}
function decodeRgb565Be(hi: number, lo: number): [number, number, number] {
    const v = (hi << 8) | lo;
    return [Math.round((((v >> 11) & 0x1f) * 255) / 31), Math.round((((v >> 5) & 0x3f) * 255) / 63), Math.round(((v & 0x1f) * 255) / 31)];
}
function encodeLandscapePNG(fb: Uint8Array): Uint8Array {
    const w = LAND_W, h = LAND_H;
    const raw = new Uint8Array((w * 3 + 1) * h);
    for (let ly = 0; ly < h; ly++) {
        const rowOff = ly * (w * 3 + 1);
        raw[rowOff] = 0;
        for (let lx = 0; lx < w; lx++) {
            const [hi, lo] = landPx(fb, lx, ly);
            const [r, g, b] = decodeRgb565Be(hi, lo);
            const o = rowOff + 1 + lx * 3;
            raw[o] = r; raw[o + 1] = g; raw[o + 2] = b;
        }
    }
    const idat = zlibWrap(Bun.deflateSync(raw), raw);
    const sig = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    const ihdr = new Uint8Array(13);
    const dv = new DataView(ihdr.buffer);
    dv.setUint32(0, w, false); dv.setUint32(4, h, false);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    return concatBytes([sig, pngChunk("IHDR", ihdr), pngChunk("IDAT", idat), pngChunk("IEND", new Uint8Array(0))]);
}
async function shot(name: string, fb: Uint8Array, what: string) {
    const path = join(PREVIEW_DIR, name);
    await Bun.write(path, encodeLandscapePNG(fb));
    console.log(`    wrote ${path} - ${what}`);
}

async function main() {
    console.log("=== four's emoji-style tokens: four pictures ===\n");
    const dev = await loadDevice();
    dev.touchLand(false, 0, 0, 0);
    dev.appSwitch(APP_FOUR);
    dev.touchLand(false, 0, 0, 1000);

    // Pick "vs human" and let the opening hand-off settle: an empty board,
    // red's face waiting above the centre column.
    let t = holdLand(dev, LAND_W * 0.25, 200, 1000, 350);
    t = settle(dev, t, RELEASE_GRACE_MS + 350);
    t = settle(dev, t, HANDOFF_MS + 200);
    await shot("four-faces-empty.png", dev.fbSnapshot(), "an empty board: red's face waiting above the centre column");

    // A few moves, alternating colours, to get both faces on the board at
    // once - caught mid-drop for the third one.
    t = playMove(dev, 2, t); // red
    t = playMove(dev, 4, t); // blue
    t = playMove(dev, 3, t); // red

    // Column 5, blue's turn: catch this one mid-fall.
    t = holdLand(dev, colX(5), THUMB_LY, t, 140);
    dev.drainLog();
    let caught: Uint8Array | null = null;
    for (let i = 0; i < 40 && !caught; i++) {
        t += 15;
        dev.touchLand(false, 0, 0, t);
        const fb = dev.fbSnapshot();
        const mid = landPx(fb, colX(5), rowY(2));
        const bottom = landPx(fb, colX(5), rowY(5));
        // mid-column ink present, bottom hole still paper: genuinely in flight
        if (!(mid[0] === 0xff && mid[1] === 0xff) && bottom[0] === 0xff && bottom[1] === 0xff) caught = fb;
    }
    if (caught) await shot("four-faces-drop.png", caught, "a face mid-drop, passing partway down its own lit chute");
    t = settle(dev, t, RELEASE_GRACE_MS + 1300);

    t = playMove(dev, 4, t); // red
    await shot("four-faces-midgame.png", dev.fbSnapshot(), "a mid-game board: both faces present, at rest");

    // Finish the game with an easy diagonal-ish sequence so a win is
    // reached quickly for the fourth picture.
    t = playMove(dev, 5, t); // blue
    t = playMove(dev, 5, t); // red
    t = playMove(dev, 6, t); // blue
    t = playMove(dev, 6, t); // red
    t = playMove(dev, 6, t); // blue
    t = playMove(dev, 0, t); // red completes col 0/2/4/6? fall back: just play the centre a few more times
    let winSeen = dev.drainLog().some((l) => l.includes("four: win"));
    let tries = 0;
    while (!winSeen && tries < 30) {
        t = playMove(dev, tries % 7, t);
        winSeen = dev.drainLog().some((l) => l.includes("four: win"));
        tries++;
    }
    if (winSeen) {
        // let the pulse run a few frames so a swollen piece is on screen
        let best = dev.fbSnapshot(), bestSpread = -1;
        for (let i = 0; i < 20; i++) {
            t = settle(dev, t, 70);
            const fb = dev.fbSnapshot();
            // crude "how far does ink reach past a hole" probe, reused from
            // feature-four.ts's own discRadius idea, just to pick a good frame
            let widest = 0;
            for (let c = 0; c < 7; c++) for (let r = 0; r < 6; r++) {
                const centre = landPx(fb, colX(c), rowY(r));
                if (centre[0] === 0xff && centre[1] === 0xff) continue;
                let d = 0;
                while (d < 28) {
                    const p = landPx(fb, colX(c) + d, rowY(r));
                    if (p[0] === 0xff && p[1] === 0xff) break;
                    d++;
                }
                if (d > widest) widest = d;
            }
            if (widest > bestSpread) { bestSpread = widest; best = fb; }
        }
        await shot("four-faces-win.png", best, "a winning line: the four faces swollen and blooming");
    } else {
        console.log("    (no win reached in the scripted sequence - skipping four-faces-win.png)");
    }

    console.log("\ndone.");
}

main();
