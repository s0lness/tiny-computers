// repro-morpion-grid-centred: the 3x3 slab used to sit flush against the
// right edge of the visible area (firmware/apps/morpion.c's SLAB_X1 was
// literally SAFE_X1), because the file's original layout spent every pixel
// of spare width on the tray to its left rather than centring the board -
// see the "THE 82px LEFT OVER ACROSS THE WIDTH IS THE TRAY" note that used
// to justify it. The owner asked for the grid centred instead ("centre the
// grid"), which this file pins as a standing regression check: measured
// straight off the rendered pixels, never off a copy of morpion.c's own
// SLAB_X0/SLAB_X1 formula, so a future edit that reintroduces the
// flush-right layout (or centres it against LAND_W instead of the visible
// SAFE_* area) fails here even if it never touches this file.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/repro-morpion-grid-centred.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const LAND_H = PANEL_W; // 368
const BEZEL = 10; // gfx.h PANEL_BEZEL_MARGIN_PX
const SAFE_X0 = BEZEL, SAFE_X1 = LAND_W - BEZEL;

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? ` (${detail})` : ""}`);
}

async function loadDevice() {
    let memory!: WebAssembly.Memory;
    const dec = new TextDecoder();
    const mod = await WebAssembly.compile(readFileSync(WASM_PATH));
    const instance = await WebAssembly.instantiate(mod, {
        env: {
            js_log(ptr: number, len: number) {
                console.log(`    [fw] ${dec.decode(new Uint8Array(memory.buffer, ptr, len)).trim()}`);
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
    });
    memory = instance.exports.memory as WebAssembly.Memory;
    const exp = instance.exports as any;
    if (exp.emu_init() !== 1) throw new Error("emu_init() failed");
    return {
        tick(ms: number) { exp.emu_tick(ms); },
        appSwitch(i: number) { exp.emu_app_switch(i); },
        appCurrent(): number { return exp.emu_app_current(); },
        fbBytes(): Uint8Array {
            return new Uint8Array(memory.buffer, exp.emu_fb(), PANEL_W * PANEL_H * 2).slice();
        },
        apps(): string[] {
            const b = new Uint8Array(memory.buffer, exp.emu_device());
            let end = 0;
            while (b[end] !== 0) end++;
            const json = JSON.parse(dec.decode(b.subarray(0, end)));
            return Array.isArray(json.apps) ? (json.apps as string[]) : [];
        },
    };
}

// Landscape (lx, ly) -> panel index, gfx.h's own mapping.
function landPx(fb: Uint8Array, lx: number, ly: number): [number, number] {
    const px = PANEL_W - 1 - Math.round(ly);
    const py = Math.round(lx);
    const idx = (py * PANEL_W + px) * 2;
    return [fb[idx]!, fb[idx + 1]!];
}
function near(got: [number, number], want: [number, number], tol = 10): boolean {
    return Math.abs(got[0] - want[0]) <= tol && Math.abs(got[1] - want[1]) <= tol;
}
// X plays first, so a freshly-entered board's slab wears X's warm tint from
// frame 0 (morpion.c's col_slab(P_X), the same bytes feature-morpion.ts
// matches as C_SLAB_X) - matched specifically, not "anything non-white",
// because the scan row also crosses the TRAY on its way in from the left
// edge, and the tray's waiting mark is solid red at (255,0,0)-ish: matching
// "non-white" would stop on the mark, not the slab.
const C_SLAB_X: [number, number] = [0xe6, 0xda];

async function main() {
    const dev = await loadDevice();
    dev.tick(0);
    const APP_MORPION = dev.apps().indexOf("morpion");
    if (APP_MORPION < 0) throw new Error(`no "morpion" app in this emu.wasm (apps=${JSON.stringify(dev.apps())})`);
    dev.appSwitch(APP_MORPION);
    dev.tick(16);
    if (dev.appCurrent() !== APP_MORPION) throw new Error("did not land in morpion");

    // Scan the fresh board's mid-row (a flat slab, no discs or bands
    // interrupt it there - see morpion.c's own THE LAYOUT comment) from
    // both edges of the VISIBLE area inward, stopping on the first pixel
    // that is not paper. That is the slab's true rendered left and right
    // edge, read off pixels rather than assumed from a formula.
    const fb = dev.fbBytes();
    // Vertical mid-line of the whole safe area is close enough to the
    // slab's own centre for this scan (the slab's own vertical placement
    // is not in question here): SAFE_Y range is bezel..LAND_H-bezel.
    const scanY = Math.round(LAND_H / 2);
    let left = -1, right = -1;
    for (let lx = SAFE_X0; lx <= SAFE_X1; lx++) {
        if (near(landPx(fb, lx, scanY), C_SLAB_X)) { left = lx; break; }
    }
    for (let lx = SAFE_X1; lx >= SAFE_X0; lx--) {
        if (near(landPx(fb, lx, scanY), C_SLAB_X)) { right = lx; break; }
    }
    check("the slab has a left and a right edge inside the visible area",
        left >= 0 && right >= 0 && left <= right,
        `left=${left} right=${right} scanY=${scanY} SAFE=[${SAFE_X0},${SAFE_X1}]`);

    if (left >= 0 && right >= 0) {
        const leftMargin = left - SAFE_X0;
        const rightMargin = SAFE_X1 - right;
        console.log(`    slab spans x=${left}..${right}; left margin ${leftMargin}px, right margin ${rightMargin}px`);
        check("the grid's bounding box is horizontally symmetric inside the visible area (centred, not flush to either edge)",
            Math.abs(leftMargin - rightMargin) <= 2,
            `left margin ${leftMargin}px vs right margin ${rightMargin}px`);
    }

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
