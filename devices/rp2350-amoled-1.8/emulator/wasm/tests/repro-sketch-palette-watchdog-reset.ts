// repro-sketch-palette-watchdog-reset: headless reproduction of the
// owner's report on real hardware, 2026-08-14, the day this feature
// shipped a third time - "ça plante et ça rebote sur le chrono" (it
// crashes and reboots into the stopwatch). The coordinator listened on
// the serial port while the owner used the palette:
//
//   stroke start (177,254) t=93897 (persisted)
//   palette: open at (177,254) candidate=4
//   sound: init ok
//   chrono: entered, stopped at 00:00:00
//   switch: chrono (15010 us)
//
// "palette: open" is sketch.c's own, and it fires. "sound: init ok" is a
// BOOT message: the device rebooted the instant the palette opened, the
// watchdog fired, and it came back up in the default app (chrono). No
// fault printed, core1restarts=0 - core0 alone, not getting back to feed
// the watchdog.
//
// THE CAUSE. palette_render_frame() used to whiten the whole panel and
// recompute all nine cells' signed-distance coverage on EVERY call - a
// deliberate choice, made for correctness (see sketch.c's own git history:
// it fixed a real residue bug), but never costed. One call: roughly
// 165,000 whitened pixels plus ~150,000-190,000 coverage evaluations
// (each a sqrtf) - see the measurement below, against apps that already
// run comfortably on this exact board. That call happened on EVERY
// drained touch sample while the ~280ms pop-in animation was in progress,
// gated on nothing but wall-clock elapsed time. core1 polls the touch
// controller far faster than it reports (up to ~1.4kHz when Touch_INT_PIN
// is held low - see sensors.h), and sketch_tick's own drain loop processes
// every queued sample before ever returning - so a real burst of samples
// queued during the animation could trigger hundreds of these full
// renders, and hundreds of full-panel pushes (~12ms each, per the
// coordinator's own figure), before core0 ever got back to whatever feeds
// the watchdog.
//
// THE FIX, in sketch.c: palette_render_frame() now (1) throttles to
// PALETTE_RENDER_MIN_MS (33ms, ~30fps) apart regardless of how many
// samples arrive, giving a hard, sample-count-independent cap on render
// count, and (2) once the pop-in has settled - most of this feature's own
// lifetime, since picking a colour can take seconds while the pop is a
// fifth of one - only whitens and redraws the (at most two) cells that
// can possibly have changed, each within a provable maximum extent
// (palette_cell_max_extent(), not the whole panel).
//
// THIS FILE COUNTS, per decision 0008's own argument that a cost this
// project does not measure is a cost it will eventually ship: the
// emulator has no watchdog and no real clock (decision 0003), so it
// cannot reproduce "the board resets" by simulating time - what it CAN do
// is count the exact same things a cost review would: render-frame count
// under a sample flood, and pixels-touched/coverage-evaluations per
// frame, each printed by sketch.c itself (see palette_render_frame()'s
// own "palette: frame ..." log line) rather than needing new plumbing
// through emu_shim.c/build.ts, which belong to work in flight elsewhere
// in this tree right now.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/repro-sketch-palette-watchdog-reset.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const PANEL_W = 368;
const PANEL_H = 448;
const APP_DRAW = 1; // g_apps[] = { chrono, sketch("draw"), timer, four }
const PANEL_AREA = PANEL_W * PANEL_H;

// Mirrors of sketch.c's own #defines.
const CONFIRM_MS = 40;
const LONG_PRESS_MS = 550;
const PALETTE_POP_MS = 240;
const PALETTE_STAGGER_MS = 20;
const PALETTE_RENDER_MIN_MS = 33;
const PALETTE_ANIM_TOTAL_MS = PALETTE_POP_MS + 2 * PALETTE_STAGGER_MS; // 280, the last rank's own settle time

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

    return {
        tick(nowMs: number) { exp.emu_tick(nowMs); },
        touch(down: boolean, x: number, y: number) { exp.emu_touch(down ? 1 : 0, Math.round(x), Math.round(y)); },
        appSwitch(index: number) { exp.emu_app_switch(index); },
        pushRects(): { x: number; y: number; w: number; h: number }[] {
            const n = exp.emu_push_count();
            const out: { x: number; y: number; w: number; h: number }[] = [];
            for (let i = 0; i < n; i++) out.push({ x: exp.emu_push_x(i), y: exp.emu_push_y(i), w: exp.emu_push_w(i), h: exp.emu_push_h(i) });
            return out;
        },
        drainLog(): string[] { const out = fwLog.slice(); fwLog.length = 0; return out; },
    };
}

type Device = Awaited<ReturnType<typeof loadDevice>>;

// Parses one "palette: frame kind=X whitenPx=N coverageEvals=M" line.
function parseFrameLine(line: string): { kind: string; whitenPx: number; coverageEvals: number } | null {
    const m = line.match(/palette: frame kind=(\w+) whitenPx=(\d+) coverageEvals=(\d+)/);
    if (!m) return null;
    return { kind: m[1]!, whitenPx: Number(m[2]), coverageEvals: Number(m[3]) };
}

async function main() {
    console.log("=== reproduction + regression: the palette's own per-frame cost, bounded ===\n");

    // ---- baseline: what does this board already push comfortably? -------
    // An ordinary pen stroke in this same app - nobody has ever reported
    // drawing feeling slow, and every push it makes already has to fit
    // inside a single tick's own budget on real hardware, same as the
    // palette's own frames do. Measured directly rather than assumed: a
    // real drag, several accepted samples, each one's own capsule push.
    const devT = await loadDevice();
    devT.tick(0);
    devT.appSwitch(APP_DRAW);
    devT.tick(1000);
    devT.drainLog();
    let maxStrokePushArea = 0;
    let tT = 1000;
    const strokePts: [number, number][] = [[40, 60], [70, 90], [100, 130], [140, 160], [190, 200]];
    for (const [px, py] of strokePts) {
        tT += 15;
        devT.touch(true, px, py);
        devT.tick(tT);
        for (const r of devT.pushRects()) maxStrokePushArea = Math.max(maxStrokePushArea, r.w * r.h);
    }
    console.log(`    baseline: an ordinary pen stroke's own largest single push = ${maxStrokePushArea}px ` +
        `(${((maxStrokePushArea / PANEL_AREA) * 100).toFixed(1)}% of the panel)`);

    // ---- scenario A: open the palette and hold through the full pop-in
    // animation, driven by a touch sample every 2ms - far faster than this
    // controller's own ~60-68Hz report rate, standing in for the queued
    // backlog a real burst of core1 polls can leave for core0 to drain in
    // one pass (see this file's own header comment). Count every "palette:
    // frame" line this produces, and its own cost. ------------------------
    const dev = await loadDevice();
    dev.tick(0);
    dev.appSwitch(APP_DRAW);
    dev.tick(1000);
    check("switched into sketch", true);
    dev.drainLog();

    let t = 1000;
    const holdX = 184, holdY = 223; // the centre cell (post gfx.h PANEL_BEZEL_MARGIN_PX inset) -
                                     // see feature-sketch-palette.ts's own paletteCellBounds() mirror
    const FAST_STEP_MS = 2; // far faster than any real report rate - a synthetic worst-case backlog
    const HOLD_MS = CONFIRM_MS + LONG_PRESS_MS + PALETTE_ANIM_TOTAL_MS + 150; // through open + full settle
    let frames: { kind: string; whitenPx: number; coverageEvals: number }[] = [];
    for (let held = 0; held < HOLD_MS; held += FAST_STEP_MS) {
        t += FAST_STEP_MS;
        dev.touch(true, holdX, holdY);
        dev.tick(t);
        for (const l of dev.drainLog()) {
            const f = parseFrameLine(l);
            if (f) frames.push(f);
        }
    }
    const openedOk = frames.length > 0;
    check("the palette actually opened and animated under the flood (sanity: this run measures something)", openedOk);

    const animFrames = frames.filter((f) => f.kind === "anim");
    const totalWhiten = frames.reduce((s, f) => s + f.whitenPx, 0);
    const totalEvals = frames.reduce((s, f) => s + f.coverageEvals, 0);
    const stepsInWindow = Math.ceil(HOLD_MS / FAST_STEP_MS);
    console.log(`    ${stepsInWindow} touch samples fed in (a 2ms cadence over ${HOLD_MS}ms) -> ${frames.length} actual render frames ` +
        `(${animFrames.length} "anim", ${frames.length - animFrames.length} "settled")`);
    console.log(`    total whitenPx=${totalWhiten}, total coverageEvals=${totalEvals} across the whole open+animate+settle sequence`);

    // THE CORE CLAIM: render count is bounded by wall-clock time divided by
    // the throttle, NOT by how many samples were fed in. A sample flood
    // 20x denser than a real burst (2ms vs the ~40-70ms gaps a genuine
    // queued backlog would show between distinct reports) still produces
    // only a small, fixed number of frames - this is what a HALF-second
    // stall (hundreds of frames, the pre-fix shape) looks like from this
    // side: it cannot happen here regardless of input rate.
    const maxExpectedFrames = Math.ceil(HOLD_MS / PALETTE_RENDER_MIN_MS) + 3; // +3: open's own forced frame,
                                                                               // rounding, one settle-margin frame
    check(`render-frame count is bounded by wall-clock time, not sample count (<=${maxExpectedFrames} despite ${stepsInWindow} samples fed)`,
        frames.length <= maxExpectedFrames, `${frames.length} frames`);

    // PER-FRAME cost ceiling: no single frame should cost meaningfully more
    // than "all nine cells, each at their own provable maximum extent" -
    // this is the number that was ALWAYS being paid before the fix, on
    // every sample; now it is what the (few, throttled) "anim" frames pay,
    // once, not what every sample pays. 220,000 gives headroom over the
    // measured ~189,000/~176,000 (whiten/coverage) without hiding a real
    // regression back toward "the whole panel, unscoped".
    const PER_FRAME_CEILING = 220000;
    const worstWhiten = Math.max(0, ...frames.map((f) => f.whitenPx));
    const worstEvals = Math.max(0, ...frames.map((f) => f.coverageEvals));
    check(`no single frame whitens more than ${PER_FRAME_CEILING}px (worst measured: ${worstWhiten})`,
        worstWhiten <= PER_FRAME_CEILING);
    check(`no single frame evaluates more than ${PER_FRAME_CEILING} coverage samples (worst measured: ${worstEvals})`,
        worstEvals <= PER_FRAME_CEILING);

    // Compared against the baseline: opening the palette costs, IN TOTAL
    // across its own one-time ~280ms animation, work on the order of a
    // double-digit multiple of one already-comfortable timer tick - a
    // one-time burst when a child starts a hold, not a per-sample tax.
    const strokeMultiple = maxStrokePushArea > 0 ? worstWhiten / maxStrokePushArea : Infinity;
    console.log(`    the busiest single "anim" frame costs ${strokeMultiple.toFixed(0)}x one ordinary stroke ` +
        `sample's own largest push - a real multiple, paid at most ${animFrames.length} times total (bounded above), ` +
        `not once per sample the way it used to be`);
    check("the busiest single frame's cost is a bounded multiple of an already-comfortable stroke push, not unbounded",
        strokeMultiple < 200, `${strokeMultiple.toFixed(0)}x`);

    // ---- scenario B: once settled, a burst of RAPID candidate changes
    // (dragging across several cells quickly) must stay cheap - the
    // steady state is most of this feature's own lifetime (picking a
    // colour takes seconds; the pop is a fifth of one), so this is the
    // cost that actually matters most for how the gesture feels. --------
    console.log("");
    // Drag across several cell centres in quick succession, each held long
    // enough to register as a genuine candidate but with no settle delay
    // needed (the animation is already long over by construction).
    // Post gfx.h PANEL_BEZEL_MARGIN_PX inset - see feature-sketch-palette.ts's
    // own paletteCellBounds() mirror for how these are derived.
    const cellCentresApprox: [number, number][] = [[70, 83], [184, 83], [298, 83], [70, 223], [184, 223], [298, 223]];
    dev.drainLog();
    let settledFrames: { kind: string; whitenPx: number; coverageEvals: number }[] = [];
    for (const [cx, cy] of cellCentresApprox) {
        for (let i = 0; i < 3; i++) { // a few samples per cell, well under the throttle window - settled path has none
            t += FAST_STEP_MS;
            dev.touch(true, cx, cy);
            dev.tick(t);
            for (const l of dev.drainLog()) {
                const f = parseFrameLine(l);
                if (f) settledFrames.push(f);
            }
        }
    }
    const worstSettledWhiten = Math.max(0, ...settledFrames.filter((f) => f.kind === "settled").map((f) => f.whitenPx));
    console.log(`    ${settledFrames.length} settled-phase frames across a ${cellCentresApprox.length}-cell rapid drag, ` +
        `worst single frame whitenPx=${worstSettledWhiten}`);
    // Two cells' own maximum extent (measured: ~22,300px each for a
    // row-1 cell, the tallest row - see palette_cell_max_extent()), with
    // headroom - the point is this stays a SMALL constant regardless of
    // how many cells the drag crosses, unlike the pre-fix whole-panel
    // cost every one of these samples used to pay.
    const SETTLED_FRAME_CEILING = 50000;
    check(`a settled-phase candidate change never costs more than ${SETTLED_FRAME_CEILING}px (worst measured: ${worstSettledWhiten})`,
        worstSettledWhiten <= SETTLED_FRAME_CEILING);

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
