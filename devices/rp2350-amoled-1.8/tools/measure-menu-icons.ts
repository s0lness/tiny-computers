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
const LAND_H = PANEL_W; // 368
const APP_INDEX_MENU = -1;
const ICON_W = 96;
const ICON_H = 96;

// menu.c's grid (THE LAYOUT there): cells MENU_CELL_H tall, at most
// MENU_COLS_MAX across, packed to the top, each row's cells contiguous with
// the last one absorbing the remainder. The icon is CENTRED in its cell, so
// there is no fixed ICON_TOP_MARGIN any more - reading one would crop the
// wrong 96x96 box and every measurement below would be of white paper.
const MENU_CELL_H = 112;
const MENU_COLS_MAX = 4;
const MENU_ROWS_MAX = Math.floor(LAND_H / MENU_CELL_H);

function cellRectLand(i: number, n: number): { bx: number; by: number; bw: number; bh: number } {
    const rows = Math.min(Math.max(Math.ceil(n / MENU_COLS_MAX), 1), MENU_ROWS_MAX);
    const base = Math.floor(n / rows);
    const extra = n % rows;
    for (let r = 0; r < rows; r++) {
        const first = r * base + Math.min(r, extra);
        const cols = base + (r < extra ? 1 : 0);
        if (i < first + cols) {
            const c = i - first;
            const w = Math.floor(LAND_W / cols);
            const bx = c * w;
            return { bx, by: r * MENU_CELL_H, bw: c === cols - 1 ? LAND_W - bx : w, bh: MENU_CELL_H };
        }
    }
    throw new Error(`no cell for index ${i} at ${n} apps`);
}

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

    console.log("icon        optical WxH   ink px   ink %   stroke px");
    console.log("----------  -----------   ------   -----   ---------");
    const rows: { name: string; w: number; h: number; ink: number; pct: number; stroke: number }[] = [];
    for (let i = 0; i < n; i++) {
        const { bx, by, bw, bh } = cellRectLand(i, n);
        const x0 = bx + Math.floor((bw - ICON_W) / 2);
        const y0 = by + Math.floor((bh - ICON_H) / 2);

        let minX = 1e9, maxX = -1, minY = 1e9, maxY = -1, ink = 0;
        const solid: boolean[][] = [];
        for (let ly = y0; ly < y0 + ICON_H; ly++) {
            const row: boolean[] = [];
            for (let lx = x0; lx < x0 + ICON_W; lx++) {
                const g = gray(lx, ly);
                row.push(g < 128);
                if (g >= 128) { continue; }          // paper, or a light AA fringe
                ink += (255 - g) / 255;               // coverage-weighted, so an
                                                       // anti-aliased edge counts
                                                       // for what it actually is
                if (lx < minX) minX = lx;
                if (lx > maxX) maxX = lx;
                if (ly < minY) minY = ly;
                if (ly > maxY) maxY = ly;
            }
            solid.push(row);
        }
        const ow = maxX - minX + 1, oh = maxY - minY + 1;
        const pct = (ink / (ow * oh)) * 100;

        // STROKE WEIGHT, which is the axis the eye reads as "belongs to this
        // set" and the one this tool did not report until an icon passed both
        // of the others and the owner rejected it on sight: "l'epaisseur du
        // trait de l'icone du puissance 4 est pas la meme que les autres".
        //
        // Measured as the MODE of the run lengths of consecutive ink pixels,
        // taken along both rows and columns. For a stroked glyph almost every
        // run is one crossing of the stroke, so they pile up at the stroke's
        // width and the mode lands on it; a SOLID area contributes runs
        // spread thinly across every length up to its own diameter, so it
        // never forms a peak and cannot outvote the strokes.
        //
        // The median was tried first and is wrong for exactly that reason:
        // this icon has two filled counters, whose long runs dragged it to
        // 25px on a glyph whose stroke is 10. The mode is checkable rather
        // than merely plausible - the other three are Lucide conversions at a
        // known LUCIDE_STROKE_HALF of 5, so if this metric does not report
        // about 10 for them, the metric is what is broken.
        const runs: number[] = [];
        const collect = (get: (a: number, b: number) => boolean, outer: number, inner: number) => {
            for (let a = 0; a < outer; a++) {
                let run = 0;
                for (let b = 0; b < inner; b++) {
                    if (get(a, b)) { run++; continue; }
                    if (run > 0) runs.push(run);
                    run = 0;
                }
                if (run > 0) runs.push(run);
            }
        };
        collect((r, c) => solid[r]![c]!, ICON_H, ICON_W);
        collect((c, r) => solid[r]![c]!, ICON_W, ICON_H);
        const hist = new Map<number, number>();
        for (const r of runs) hist.set(r, (hist.get(r) ?? 0) + 1);
        let stroke = 0, best = -1;
        for (const [len, count] of hist) if (count > best || (count === best && len < stroke)) { best = count; stroke = len; }

        rows.push({ name: names[i]!, w: ow, h: oh, ink, pct, stroke });
        console.log(
            `${names[i]!.padEnd(10)}  ${String(ow).padStart(3)} x ${String(oh).padStart(3)}   ` +
            `${Math.round(ink).toString().padStart(6)}   ${pct.toFixed(1).padStart(5)}%   ` +
            `${String(stroke).padStart(9)}`,
        );
    }

    // The comparison the eye actually makes: is the new one an outlier?
    const others = rows.filter((r) => r.name !== "four");
    const four = rows.find((r) => r.name === "four");
    if (four && others.length) {
        const rng = (f: (r: typeof rows[0]) => number) => [Math.min(...others.map(f)), Math.max(...others.map(f))];
        const [hLo, hHi] = rng((r) => r.h);
        const [iLo, iHi] = rng((r) => r.ink);
        const [sLo, sHi] = rng((r) => r.stroke);
        const verdict = (v: number, lo: number, hi: number) => (v >= lo && v <= hi ? "IN range" : "OUT of range");
        console.log("");
        console.log(`the other icons: ${hLo}-${hHi}px tall, ${Math.round(iLo)}-${Math.round(iHi)} ink px, ${sLo}-${sHi}px stroke`);
        console.log(`four:            ${four.h}px tall (${verdict(four.h, hLo, hHi)}), ` +
            `${Math.round(four.ink)} ink px (${verdict(four.ink, iLo, iHi)}), ` +
            `${four.stroke}px stroke (${verdict(four.stroke, sLo, sHi)})`);
        console.log("");
        console.log("STROKE is the one to trust first: two icons can agree on size and ink");
        console.log("and still not look related, and that is what happened here twice.");
    }
}

main();
