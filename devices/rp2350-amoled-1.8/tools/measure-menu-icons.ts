/**
 * Measures the menu's icons against each other, from the REAL compiled
 * firmware (emulator/wasm/dist/emu.wasm), so "does this new icon sit with
 * the others" is answered with numbers instead of an opinion.
 *
 *   bun run emulator/wasm/build.ts
 *   bun tools/measure-menu-icons.ts
 *
 * Reports, per icon: the ink's own bounding box (its OPTICAL size, which is
 * what the eye compares - not the 96x96 box it was drawn into), and the ink
 * coverage as a percentage of that box. An icon that is much shorter than
 * its neighbours reads as small however carefully it was drawn, and one
 * that is much darker reads as belonging to a different set.
 *
 * This exists because the first Connect Four icon was wrong in both
 * directions at once - 68px tall against the others' 90, and 25 to 45
 * percent more ink - and neither fact was visible by looking at the icon on
 * its own. Only the comparison shows it.
 *
 * Deliberately does NOT open a serial port or touch a physical device.
 */
import { readFileSync } from "node:fs";
import { join } from "node:path";

const ROOT = join(import.meta.dir, "..");
const WASM_PATH = join(ROOT, "emulator", "wasm", "dist", "emu.wasm");

const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const APP_INDEX_MENU = -1;
const ICON_TOP_MARGIN = 24;
const ICON_W = 96;
const ICON_H = 96;

async function main() {
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
    e.emu_tick(0);
    e.emu_app_switch(APP_INDEX_MENU);
    e.emu_tick(16);

    const ptr = e.emu_device();
    const bytes = new Uint8Array(memory.buffer, ptr);
    let end = 0;
    while (bytes[end] !== 0) end++;
    const names: string[] = JSON.parse(dec.decode(bytes.subarray(0, end))).apps ?? [];
    const n = names.length;

    const fb = new Uint8Array(memory.buffer, e.emu_fb(), PANEL_W * PANEL_H * 2).slice();
    // Landscape read; 0 = black ink, 255 = white paper (gfx.h px_to_gray).
    const gray = (lx: number, ly: number) => {
        const i = (lx * PANEL_W + (PANEL_W - 1 - ly)) * 2;
        const v = (fb[i]! << 8) | fb[i + 1]!;
        return (((v >> 5) & 0x3f) << 2);
    };

    console.log("icon        optical WxH   ink px   ink % of its own box");
    console.log("----------  -----------   ------   --------------------");
    const rows: { name: string; w: number; h: number; ink: number; pct: number }[] = [];
    for (let i = 0; i < n; i++) {
        const w = Math.floor(LAND_W / n);
        const bx = i * w;
        const bw = i === n - 1 ? LAND_W - bx : w;
        const x0 = bx + Math.floor((bw - ICON_W) / 2);
        const y0 = ICON_TOP_MARGIN;

        let minX = 1e9, maxX = -1, minY = 1e9, maxY = -1, ink = 0;
        for (let ly = y0; ly < y0 + ICON_H; ly++) {
            for (let lx = x0; lx < x0 + ICON_W; lx++) {
                const g = gray(lx, ly);
                if (g >= 128) continue;           // paper, or a light AA fringe
                ink += (255 - g) / 255;            // coverage-weighted, so an
                                                    // anti-aliased edge counts
                                                    // for what it actually is
                if (lx < minX) minX = lx;
                if (lx > maxX) maxX = lx;
                if (ly < minY) minY = ly;
                if (ly > maxY) maxY = ly;
            }
        }
        const ow = maxX - minX + 1, oh = maxY - minY + 1;
        const pct = (ink / (ow * oh)) * 100;
        rows.push({ name: names[i]!, w: ow, h: oh, ink, pct });
        console.log(
            `${names[i]!.padEnd(10)}  ${String(ow).padStart(3)} x ${String(oh).padStart(3)}   ` +
            `${Math.round(ink).toString().padStart(6)}   ${pct.toFixed(1).padStart(5)}%`,
        );
    }

    // The comparison the eye actually makes: is the new one an outlier?
    const others = rows.filter((r) => r.name !== "four");
    const four = rows.find((r) => r.name === "four");
    if (four && others.length) {
        const hLo = Math.min(...others.map((r) => r.h)), hHi = Math.max(...others.map((r) => r.h));
        const iLo = Math.min(...others.map((r) => r.ink)), iHi = Math.max(...others.map((r) => r.ink));
        console.log("");
        console.log(`the other icons span ${hLo}-${hHi}px tall and ${Math.round(iLo)}-${Math.round(iHi)} ink px`);
        console.log(`four is ${four.h}px tall (${four.h >= hLo && four.h <= hHi ? "IN range" : "OUT of range"}) ` +
            `and ${Math.round(four.ink)} ink px (${four.ink >= iLo && four.ink <= iHi ? "IN range" : "OUT of range"})`);
    }
}

main();
