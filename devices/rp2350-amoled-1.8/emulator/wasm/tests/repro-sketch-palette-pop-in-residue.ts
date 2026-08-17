// repro-sketch-palette-pop-in-residue: headless reproduction of the
// owner's report on real hardware, 2026-08-15, the day after the fifth
// round's timing tune (shorter pop, faster throttle) landed and was
// flashed:
//
//   "L'animation de palette est pire qu'avant: maintenant les rectangles
//   s'overlapent, et c'est pas plus rapide. Si je fais popper la palette,
//   puis je parcours tout l'ecran avec mon doigt, les rectangles reprennent
//   leur bonne forme."
//
// Cells draw overlapping after the pop, and sweeping a finger across the
// grid repairs them one by one - the coordinator's own diagnosis, confirmed
// below: sweeping drives palette_render_handoff() (the settled hand-off
// path), which redraws a cell fresh from a clean whiten. That a sweep
// FIXES it is what proves the settled path is correct and the ANIMATING
// path is the one leaving something behind - residue, not layout.
//
// THE TEST GAP THIS FILE CLOSES. repro-sketch-palette-animation-starved.ts
// (added fourth round) asserts pixels CHANGE between frames and that the
// animation passes through several distinct visible states. Both are true
// of a smeared animation - changing pixels and passing through distinct
// (wrong) states is exactly what residue looks like frame to frame. Neither
// assertion says the RESULT is correct. This file adds the assertion that
// would have caught this round's regression, and it is exact rather than a
// proxy: once the pop-in animation has finished, the framebuffer must be
// BYTE IDENTICAL to what the settled hand-off path alone would have
// produced for the same final candidate - the same trick feature-sketch-
// palette.ts's own dismiss-at-several-points scenario already uses to prove
// the canvas comes back byte for byte, applied to the palette's own grid
// instead of the drawing underneath it.
//
// HOW "the same settled state, without animating" IS PRODUCED. There is no
// dedicated "draw the whole grid instantly, no animation" function in
// sketch.c to call directly (nor should there be one just for a test) - so
// this file builds the reference the same way palette_render_handoff()
// itself is proven correct elsewhere: by touring the touch position across
// EVERY cell once, with the animation's own "still animating" window
// already elapsed (a single large forward tick right after open, before any
// further touch sample). Every cell that becomes the candidate at some
// point during that tour gets whitened to its own provable maximum extent
// (palette_cell_max_extent()) and redrawn at its exact final scale by
// palette_render_handoff() alone - the animating path (palette_render_
// animating(), the one under suspicion) never runs again after the single
// unavoidable frame open_palette() itself renders at elapsed=0, which the
// tour's own whiten-then-redraw of every cell fully overwrites regardless
// of whatever that first frame left behind.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/repro-sketch-palette-pop-in-residue.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const APP_DRAW = 1; // g_apps[] = { chrono, sketch("draw"), timer, four }
const PANEL_W = 368;
const PANEL_H = 448;

// Mirrors of sketch.c's own #defines (fifth round's values).
const CONFIRM_MS = 40;
const LONG_PRESS_MS = 550;
const PALETTE_POP_MS = 160;
const PALETTE_STAGGER_MS = 14;
const PALETTE_RENDER_MIN_MS = 16;
const PALETTE_ANIM_SETTLE_MARGIN_MS = 16;
const PALETTE_ANIM_TOTAL_MS = PALETTE_POP_MS + 2 * PALETTE_STAGGER_MS + PALETTE_ANIM_SETTLE_MARGIN_MS; // the full "still animating" window
const PALETTE_COLS = 3;
const PALETTE_ROWS = 3;
const PALETTE_CANDIDATE_GROW_PX = 3;
const PANEL_BEZEL_MARGIN_PX = 10; // gfx.h PANEL_BEZEL_MARGIN_PX
const PALETTE_GRID_INSET_PX = PANEL_BEZEL_MARGIN_PX + PALETTE_CANDIDATE_GROW_PX;
const PALETTE_GRID_W = PANEL_W - 2 * PALETTE_GRID_INSET_PX;
const PALETTE_GRID_H = PANEL_H - 2 * PALETTE_GRID_INSET_PX;

// Mirror of sketch.c's palette_cell_bounds().
function paletteCellCentre(index: number): { cx: number; cy: number } {
    const col = index % PALETTE_COLS;
    const row = Math.floor(index / PALETTE_COLS);
    const x0 = PALETTE_GRID_INSET_PX + Math.floor((PALETTE_GRID_W * col) / PALETTE_COLS);
    const x1 = PALETTE_GRID_INSET_PX + Math.floor((PALETTE_GRID_W * (col + 1)) / PALETTE_COLS);
    const y0 = PALETTE_GRID_INSET_PX + Math.floor((PALETTE_GRID_H * row) / PALETTE_ROWS);
    const y1 = PALETTE_GRID_INSET_PX + Math.floor((PALETTE_GRID_H * (row + 1)) / PALETTE_ROWS);
    return { cx: Math.floor((x0 + x1) / 2), cy: Math.floor((y0 + y1) / 2) };
}

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

async function loadDevice() {
    const bytes = readFileSync(WASM_PATH);
    const mod = await WebAssembly.compile(bytes);
    let memory!: WebAssembly.Memory;
    const decoder = new TextDecoder();
    const fwLog: string[] = [];
    const imports = {
        env: {
            js_log(ptr: number, len: number) { const b = new Uint8Array(memory.buffer, ptr, len); fwLog.push(decoder.decode(b)); },
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
    };
    const instance = await WebAssembly.instantiate(mod, imports);
    memory = instance.exports.memory as WebAssembly.Memory;
    const exp = instance.exports as any;
    if (exp.emu_init() !== 1) throw new Error("emu_init() failed - see fw log lines above");

    function fbSnapshot(): Uint8Array {
        const ptr = exp.emu_fb();
        return new Uint8Array(memory.buffer, ptr, PANEL_W * PANEL_H * 2).slice();
    }

    return {
        tick(nowMs: number) { exp.emu_tick(nowMs); },
        touch(down: boolean, x: number, y: number) { exp.emu_touch(down ? 1 : 0, Math.round(x), Math.round(y)); },
        appSwitch(index: number) { exp.emu_app_switch(index); },
        drainLog(): string[] { const out = fwLog.slice(); fwLog.length = 0; return out; },
        fbSnapshot,
    };
}

type Device = Awaited<ReturnType<typeof loadDevice>>;

// Opens the palette the ordinary way - a genuine long press, real touch
// samples, a realistic ~15ms report cadence - landing on cell `holdIndex`.
// Returns the nowMs the open itself fired at, so both scenarios below can
// diverge from the identical instant.
async function openPaletteOn(dev: Device, holdIndex: number, startMs: number): Promise<number> {
    const { cx, cy } = paletteCellCentre(holdIndex);
    let t = startMs;
    const HOLD_STEP_MS = 15;
    let opened = false;
    let openedAtMs = -1;
    for (let held = 0; held < CONFIRM_MS + LONG_PRESS_MS + 100 && !opened; held += HOLD_STEP_MS) {
        t += HOLD_STEP_MS;
        dev.touch(true, cx, cy);
        dev.tick(t);
        if (dev.drainLog().some((l) => l.includes("palette: open"))) { opened = true; openedAtMs = t; }
    }
    if (!opened) throw new Error(`palette did not open on cell ${holdIndex}`);
    return openedAtMs;
}

async function main() {
    console.log("=== reproduction + regression: the pop-in must settle to EXACTLY what the hand-off path alone would draw ===\n");

    const HOLD_INDEX = 4; // centre cell - rank 0, simplest baseline
    const FINAL_INDEX = 4; // both scenarios end with the same cell as candidate

    // ---- scenario A: THE ANIMATING PATH. Open normally, then hold
    // perfectly still and tick forward in small, realistic steps through
    // the whole pop-in (palette_advance_animation() -> palette_render_
    // animating(), throttled, the code under suspicion) until well past
    // settled, with the candidate never changing from HOLD_INDEX. --------
    const devA = await loadDevice();
    devA.tick(0);
    devA.appSwitch(APP_DRAW);
    devA.tick(1000);
    devA.drainLog();
    const openedAtA = await openPaletteOn(devA, HOLD_INDEX, 1000);
    let tA = openedAtA;
    const { cx: holdCx, cy: holdCy } = paletteCellCentre(HOLD_INDEX);
    const SETTLE_STEP_MS = 5; // fine-grained, so the throttle (16ms) is the only thing pacing renders, not the tick step
    const SETTLE_WINDOW_MS = PALETTE_ANIM_TOTAL_MS + 100; // comfortably past every cell's own true settle point
    let animFrameCount = 0;
    for (let elapsed = 0; elapsed < SETTLE_WINDOW_MS; elapsed += SETTLE_STEP_MS) {
        tA += SETTLE_STEP_MS;
        devA.touch(true, holdCx, holdCy); // held perfectly still throughout
        devA.tick(tA);
        for (const l of devA.drainLog()) if (l.includes("palette: frame")) animFrameCount++;
    }
    check("the animation actually rendered more than one frame under this scenario (sanity)", animFrameCount > 1, `${animFrameCount} frames`);
    const fbAnimated = devA.fbSnapshot();

    // ---- scenario B: THE REFERENCE, via the hand-off path alone. Open on
    // the SAME cell at the SAME wall-clock instant (a fresh device replaying
    // the identical open sequence, so paletteAnimStartMs lines up
    // identically), then immediately jump nowMs far past the "still
    // animating" window in a single tick BEFORE any further touch sample -
    // palette_advance_animation()'s very next call already sees stillAnimating
    // = false, so palette_render_animating() never runs again from this
    // point on. Every OTHER cell is then visited once (each becomes the
    // candidate in turn, driving palette_render_handoff() to whiten it to
    // its own provable maximum extent and redraw it at its exact final
    // scale), and the tour finishes back on HOLD_INDEX so both scenarios
    // land on the identical final candidate. -----------------------------
    const devB = await loadDevice();
    devB.tick(0);
    devB.appSwitch(APP_DRAW);
    devB.tick(1000);
    devB.drainLog();
    const openedAtB = await openPaletteOn(devB, HOLD_INDEX, 1000);
    let tB = openedAtB;

    // The single big jump: no touch() call in between, so st->paletteTouchX/Y
    // (and therefore the candidate palette_advance_animation() computes) is
    // still HOLD_INDEX's own position here - candidate == paletteLastCandidate,
    // so this tick renders nothing (nothing has changed yet); its only job is
    // to push elapsed time past the "still animating" threshold.
    tB += SETTLE_WINDOW_MS + 1000;
    devB.touch(true, holdCx, holdCy);
    devB.tick(tB);
    devB.drainLog();

    let handoffFrames = 0;
    // Visits every cell other than HOLD_INDEX exactly once, then returns to
    // FINAL_INDEX (== HOLD_INDEX here) so the last transition still fires a
    // real hand-off (leaving FINAL_INDEX grown, the second-to-last cell not).
    //
    // 20ms between stops, not 15: palette_render_handoff() is now paced by
    // the same PALETTE_RENDER_MIN_MS=16ms floor palette_render_animating()
    // already enforced on itself (see palette_advance_animation()'s own
    // comment in sketch.c for the mechanism this closes - decision 0001's
    // still-open "per-row DMA re-arm race", and feature-sketch-palette-
    // edge-column.ts's own scenario D for the red-then-green proof). A
    // tour stepping faster than that floor would have some of its stops
    // coalesced into the next one that clears it - correct throttling
    // behaviour, but it would silently turn this tour into fewer than nine
    // real hand-offs and break the "every visited cell gets its own frame"
    // premise this reference is built on. 20ms clears the floor with room
    // to spare while staying far under any real touch report interval
    // (~16.67ms at the controller's own 60Hz).
    const tourOrder = [0, 1, 2, 3, 5, 6, 7, 8, FINAL_INDEX];
    for (const idx of tourOrder) {
        const { cx, cy } = paletteCellCentre(idx);
        tB += 20;
        devB.touch(true, cx, cy);
        devB.tick(tB);
        for (const l of devB.drainLog()) if (l.includes("palette: frame kind=settled")) handoffFrames++;
    }
    check(`the hand-off tour actually rendered a frame for every cell visited (>= ${tourOrder.length - 1})`,
        handoffFrames >= tourOrder.length - 1, `${handoffFrames} settled frames over ${tourOrder.length} stops`);
    const fbReference = devB.fbSnapshot();

    // ---- THE ASSERTION. Byte for byte, whatever their own cost or code
    // path, the two must agree: the animating path is not allowed to leave
    // ANYTHING - a sliver of ghost ink from an overshoot frame, a stray
    // pixel at a cell's own edge - that the hand-off path would not also
    // have drawn there. ---------------------------------------------------
    let same = fbAnimated.length === fbReference.length;
    let firstDiff = -1, diffCount = 0;
    if (same) {
        for (let i = 0; i < fbAnimated.length; i++) {
            if (fbAnimated[i] !== fbReference[i]) {
                if (firstDiff < 0) firstDiff = i;
                diffCount++;
                same = false;
            }
        }
    }
    if (!same && firstDiff >= 0) {
        const px = Math.floor((firstDiff / 2) % PANEL_W);
        const py = Math.floor(firstDiff / 2 / PANEL_W);
        console.log(`    first differing byte offset=${firstDiff} (pixel (${px},${py})), ${diffCount} byte(s) differ in total`);
    }
    check("the settled pop-in is byte-identical to the hand-off path's own reference (no animating-path residue)",
        same, same ? "0 byte(s) differ" : `${diffCount} byte(s) differ`);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
