// repro-menu-icon-resample-ghost: a grown menu icon must not carry a ghost
// of its own native render underneath the resampled one.
//
// THE BUG. decision 0020 draws a cell's icon at its native ICON_W (96px),
// reads that render back (menu_icon_capture(), firmware/apps/menu.c), and
// repaints a bilinear resample of it into the grown footprint
// (menu_icon_blit()). The first version of that code never erased the
// native 96px render it had just used for the capture - and composition
// everywhere in this codebase is MIN (shapes.h's own rule: a pixel only
// ever darkens, never brightens). Left in place, the native render is a
// second, smaller copy of the icon sitting UNDER the resampled one. For an
// icon whose features are centred on the icon box's own centre this is
// invisible (two concentric circles, the smaller one entirely inside the
// bigger one's ink). Four's icon is not: it draws four sub-circles, each
// offset from the box centre, so its native and resampled renders place
// the SAME ring at two DIFFERENT positions - and MIN keeps the ink from
// both. The empty rings' holes are only white where BOTH the native and
// the resampled hole agree, which is not a circle: two circles of
// different sizes, both centred on the same point but reached by radially
// scaling from that point, still overlap almost entirely near the centre
// and diverge at the edge - so the true failure is a hole that is smaller
// and shaped like the intersection of two near-concentric circles, visibly
// a crescent, not the arithmetic centre going dark. This test samples a
// ring drawn from PANEL SPACE, not from menu.c's own reasoning about it:
// what a corner probe is to hit-testing (docs/decisions/0010), this is to
// rendering.
//
// THE FIX. render_cell() (menu.c) now repaints the native 96px footprint
// to plain white immediately after menu_icon_capture() returns, before the
// halo or the resampled blit ever touch it - see that call site's own
// comment for the full argument.
//
// WHAT THIS FILE PROVES. Four's icon (g_apps[3]) is drawn at ICON_W=96 in
// the default build (no grown cell reaches it) and at whatever
// menu_icon_size() computes once a cell grows past 96 - today, 128 at five
// and six apps. This file forces the five-app default build's own grid
// (no MENU_STUB_APPS needed) and probes the two EMPTY rings' own holes,
// both at their geometric centre (where the intersection-of-two-circles
// failure mode above stays white even when broken, so this alone would
// NOT have caught the bug - included as the "is anything drawn at all"
// sanity check) and along the inner arc nearer the box centre, which is
// exactly where the crescent's dark bite lands. Confirmed to fail before
// the fix and pass after - see this file's own task history.
//
//   bun run emulator/wasm/build.ts
//   bun run emulator/wasm/tests/repro-menu-icon-resample-ghost.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { MENU_CELL_FLOOR, menuRows, rowSpan, menuCellH, cellRect } from "../../../tools/menu-layout.ts";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H;
const LAND_H = PANEL_W;
const APP_INDEX_MENU = -1;

// menu.c's own constants for the grown-cell layout (decision 0020) and the
// four icon's own geometry (draw_icon_four) - re-derived, same posture as
// every other test in this directory: a claim, checked against the real
// framebuffer below, not trusted on its own (docs/decisions/0010).
const ICON_W = 96;
// Geometry from tools/menu-layout.ts, not a fourth hand-copy: this file was
// one of three that reported failures of their own making when the grid
// started centring itself.
const MENU_ICON_PAD = 8;
const FOUR_ICON_R = 17.5;
const FOUR_ICON_A = 25.5;
const FOUR_ICON_B = 70.5;
const LUCIDE_STROKE_HALF = 5.0;
// Which SLOT the four-in-a-row icon occupies is roster-dependent and has
// already moved once (the clock was inserted before it on 2026-08-18). Found
// by asking the roster rather than written down, so the next insertion does
// not silently point this whole file at somebody else's icon.
const FOUR_APP_INDEX = 3; // g_apps[]: chrono, sketch, timer, four, ...

function menuIconSize(cellW: number, cellH: number): number {
    return Math.max(ICON_W, Math.min(cellW, cellH) - 2 * MENU_ICON_PAD);
}

let passCount = 0, failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

async function main() {
    console.log("=== repro: a grown menu icon must not carry a native-render ghost ===\n");
    const compiled = await WebAssembly.compile(readFileSync(WASM_PATH));
    let memory!: WebAssembly.Memory;
    const decoder = new TextDecoder();
    const instance = await WebAssembly.instantiate(compiled, {
        env: {
            js_log(ptr: number, len: number) { console.log(`    [fw] ${decoder.decode(new Uint8Array(memory.buffer, ptr, len)).trim()}`); },
            sinf: Math.sin, cosf: Math.cos,
            atan2f: (y: number, x: number) => Math.atan2(y, x),
            sqrtf: Math.sqrt, fabsf: Math.abs, floorf: Math.floor,
            fmodf: (x: number, y: number) => x % y, powf: Math.pow, expf: Math.exp,
        },
    });
    memory = instance.exports.memory as WebAssembly.Memory;
    const exp = instance.exports as any;
    if (exp.emu_init() !== 1) throw new Error("emu_init() failed");

    const menuAppCount = (): number => exp.emu_menu_app_count();
    const menuAppIndex = (slot: number): number => exp.emu_menu_app_index(slot);

    exp.emu_tick(0);
    exp.emu_app_switch(APP_INDEX_MENU);
    exp.emu_tick(16);
    if (exp.emu_app_current() !== APP_INDEX_MENU) throw new Error("did not land in the menu");

    const n = menuAppCount();
    let FOUR_SLOT = -1;
    for (let i = 0; i < n; i++) if (menuAppIndex(i) === FOUR_APP_INDEX) { FOUR_SLOT = i; break; }
    check("this build's roster shows the four-in-a-row app, so the geometry below applies",
        FOUR_SLOT >= 0, `n=${n}, four found at slot ${FOUR_SLOT}`);

    const { bx, by, bw, bh } = cellRect(n, FOUR_SLOT);
    const size = menuIconSize(bw, bh);
    const cx = bx + bw / 2, cy = by + bh / 2;
    const scale = size / ICON_W;
    console.log(`    four's cell: ${bw}x${bh} at (${bx},${by}); icon size ${size} (scale ${scale.toFixed(3)})`);
    check("this is actually a grown-icon build (the bug cannot manifest below native size)",
        size > ICON_W, `size=${size}, ICON_W=${ICON_W}`);

    const fb = new Uint8Array(memory.buffer, exp.emu_fb(), PANEL_W * PANEL_H * 2);
    function grayAt(lx: number, ly: number): number {
        const px = PANEL_W - 1 - Math.round(ly);
        const py = Math.round(lx);
        const idx = (py * PANEL_W + px) * 2;
        const v = (fb[idx]! << 8) | fb[idx + 1]!;
        return ((v >> 5) & 0x3f) << 2;
    }

    // The two EMPTY rings (draw_icon_four: index 0 top-left, index 3
    // bottom-right - CX/CY = {A,B,A,B}/{A,A,B,B}, filled are 1 and 2).
    // Offset from the icon box's own centre (48,48 in native units),
    // scaled by the same factor menu_icon_blit() scales the whole icon by.
    const rings: { name: string; offX: number; offY: number }[] = [
        { name: "top-left ring (index 0)", offX: FOUR_ICON_A - ICON_W / 2, offY: FOUR_ICON_A - ICON_W / 2 },
        { name: "bottom-right ring (index 3)", offX: FOUR_ICON_B - ICON_W / 2, offY: FOUR_ICON_B - ICON_W / 2 },
    ];
    const INK_MAX = 200; // gray <= this counts as "ink" for this check's purposes

    for (const ring of rings) {
        const ringCx = cx + ring.offX * scale, ringCy = cy + ring.offY * scale;
        // Centre of the hole: stays white even under the bug (the
        // intersection of two near-concentric circles still covers the
        // shared centre), included as a sanity check that anything drew
        // here at all, not as the check that catches the regression.
        const centreGray = grayAt(ringCx, ringCy);
        check(`${ring.name}: hole centre is clear`, centreGray > INK_MAX, `gray=${centreGray}`);

        // THE BITE, worked out geometrically rather than guessed (a first
        // attempt guessed "toward the icon centre" and that sample point
        // turned out to sit inside the RESAMPLED ring's own legitimate ink
        // band, passing whether the bug was present or not - worth naming
        // since it is exactly the trap of an instrument that agrees with
        // itself, docs/decisions/0010, and it was only caught by checking
        // this file against a deliberately reintroduced bug rather than
        // trusting the arithmetic on the first try).
        //
        // Call A the NATIVE ring's own centre (offset (offX,offY) from the
        // icon box's centre, UNSCALED - where menu_icon_capture()'s native
        // draw actually put it) and B the RESAMPLED ring's centre (the
        // same offset scaled by `scale`, i.e. `ringCx,ringCy` above). Both
        // sit on the same ray out from the box centre, A closer in, B
        // further out (`scale` > 1). A's own ring band, drawn at NATIVE
        // (unscaled) radii, has a FAR crossing on that same ray - the
        // native ring's edge on the side away from the box centre - at
        // distance-from-the-box-centre `offsetMag + [innerR, outerR]`
        // (native, unscaled). That whole band sits inside B's own
        // resampled hole (B's hole reaches `offsetMag*scale +
        // resampledInnerR`, comfortably past it), so under the bug it
        // shows as an ink island stranded in the middle of what should be
        // clear - the visible crescent. Sampled at the middle of that far
        // crossing, `offsetMag + FOUR_ICON_R` (FOUR_ICON_R already being
        // the midpoint radius, (inner+outer)/2).
        const offsetMag = Math.sqrt(ring.offX * ring.offX + ring.offY * ring.offY);
        const sampleDist = offsetMag + FOUR_ICON_R;
        const perAxis = sampleDist / Math.SQRT2; // offX and offY have equal magnitude for every corner of this icon
        const biteX = cx + Math.sign(ring.offX) * perAxis;
        const biteY = cy + Math.sign(ring.offY) * perAxis;
        const biteGray = grayAt(biteX, biteY);
        check(`${ring.name}: no ghost bite where the native render's own far ring edge used to show through`,
            biteGray > INK_MAX, `gray=${biteGray}`);
    }

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
