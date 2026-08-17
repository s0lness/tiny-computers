// feature-sketch-palette-edge-column: closes a real, literal gap - no
// existing file ever drove the palette's SETTLED hand-off path (the one
// that redraws just the one or two cells a candidate change touches, as
// opposed to the pop-in's own near-full-panel redraw) against the grid
// column nearest the panel's right edge, and none of it was ever checked
// tick by tick for decision 0001's row-length rule or full pixel coverage.
//
// WHY THIS FILE EXISTS. The owner photographed the palette on real
// hardware (2026-08-17): every cell trailing thin streaks in its own
// colour off one edge, and the column nearest the panel's edge (yellow /
// blue / brown - palette_color()'s indices 2, 5, 8) with a cell visibly
// truncated. That is exactly decision 0001's own signature - a partial
// refresh whose row length the panel does not honour - so the obvious
// first move is to check whether the palette's OWN pushes ever violate
// decision 0001's rule, or ever leave a drawn pixel outside the window
// that reaches the panel. feature-sketch-palette.ts already checks this
// during the pop-in and for one hand-off (open on centre, drag to red -
// col 0, the FARTHEST column from the edge the photo shows broken).
// Nothing anywhere ever opened on, or dragged to, col 2.
//
// WHAT THIS FILE FOUND. Exhaustively driving that column - opening on it
// directly and ticking through the whole pop-in, dragging onto it under
// clean input, and dragging onto it under the SAME calibrated dropout+
// jitter profile repro-touch-dropout-palette-open.ts uses for the open
// gesture - never produces a single geometry or coverage violation. Every
// pushed window's row length is already a multiple of 8 (gfx_push rounds
// unconditionally; there is no second push path), and every pixel the app
// draws or whitens lands inside the window pushed for it. That is a real,
// useful negative result, not a shrug: it proves the corruption on glass
// is not explained by anything this emulator's model of the push path can
// represent, which is exactly what AGENTS.md's own "what it can never
// answer" section says about timing (decision 0003) - so the remaining
// hypothesis space is a hardware-timing question, not a geometry bug this
// file could have caught by looking harder at rectangles.
//
// THE ONE THING THIS FILE DOES catch red-to-green: unlike every other
// palette push site, palette_render_handoff() had no minimum spacing of
// its own before this round - see sketch.c's palette_advance_animation()
// for the added throttle and its own header comment for the mechanism
// this targets (decision 0001's still-open "per-row DMA re-arm race...
// implicate width" hypothesis). Scenario D below asserts no two palette
// pushes of DIFFERENT geometry ever land closer together than
// PALETTE_RENDER_MIN_MS - true by construction after the fix, and false
// before it (driving candidate changes on consecutive ticks with no gap
// reproduces two back-to-back hand-off pushes under the un-throttled
// code).
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/feature-sketch-palette-edge-column.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { TouchSim, type TouchReport } from "../../src/touchsim";
import { TOUCHSIM_DEFAULTS, type TouchSimConfig } from "../../src/constants";
import { seededRng, seedFromName } from "../../../tools/gate/touch";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const APP_DRAW = 1; // g_apps[] = { chrono, sketch("draw"), timer, four, ... }
const PANEL_W = 368;
const PANEL_H = 448;

// Mirrors of sketch.c's own #defines - same convention every other file in
// this directory uses for the app it drives.
const CONFIRM_MS = 40;
const LONG_PRESS_MS = 550;
const PALETTE_POP_MS = 160;
const PALETTE_STAGGER_MS = 14;
const PALETTE_ANIM_SETTLE_MARGIN_MS = 16;
const PALETTE_ANIM_TOTAL_MS = PALETTE_POP_MS + 2 * PALETTE_STAGGER_MS + PALETTE_ANIM_SETTLE_MARGIN_MS;
const PALETTE_RENDER_MIN_MS = 16;
const PALETTE_COLS = 3;
const PALETTE_ROWS = 3;
const PALETTE_CANDIDATE_GROW_PX = 3;
const PANEL_BEZEL_MARGIN_PX = 10;
const PALETTE_GRID_INSET_PX = PANEL_BEZEL_MARGIN_PX + PALETTE_CANDIDATE_GROW_PX;
const PALETTE_GRID_W = PANEL_W - 2 * PALETTE_GRID_INSET_PX;
const PALETTE_GRID_H = PANEL_H - 2 * PALETTE_GRID_INSET_PX;
const PUSH_ROW_MULTIPLE = 8; // decision 0001

// Mirror of sketch.c's palette_cell_bounds(): column is the edge-nearest
// axis (col 2's raw tile ends at PALETTE_GRID_INSET_PX + PALETTE_GRID_W,
// only PALETTE_GRID_INSET_PX short of the panel's own edge) - see
// sketch.c's own comment on PALETTE_GRID_INSET_PX for why.
function cellCentre(index: number): { col: number; row: number; cx: number; cy: number } {
    const col = index % PALETTE_COLS;
    const row = Math.floor(index / PALETTE_COLS);
    const x0 = PALETTE_GRID_INSET_PX + Math.floor((PALETTE_GRID_W * col) / PALETTE_COLS);
    const x1 = PALETTE_GRID_INSET_PX + Math.floor((PALETTE_GRID_W * (col + 1)) / PALETTE_COLS);
    const y0 = PALETTE_GRID_INSET_PX + Math.floor((PALETTE_GRID_H * row) / PALETTE_ROWS);
    const y1 = PALETTE_GRID_INSET_PX + Math.floor((PALETTE_GRID_H * (row + 1)) / PALETTE_ROWS);
    return { col, row, cx: Math.floor((x0 + x1) / 2), cy: Math.floor((y0 + y1) / 2) };
}

// The three cells in the column nearest the panel's right edge - indices
// 2, 5, 8 (palette_color(): yellow, blue, brown) - the ones the
// photograph shows streaked and, for the top one, truncated.
const EDGE_COLUMN_INDICES = [2, 5, 8];

let passCount = 0;
let failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
    const status = ok ? "PASS" : "FAIL";
    if (ok) passCount++; else failCount++;
    console.log(`[${status}] ${label}${detail ? " - " + detail : ""}`);
}

interface PushRect { x: number; y: number; w: number; h: number }

async function loadDevice() {
    const bytes = readFileSync(WASM_PATH);
    const mod = await WebAssembly.compile(bytes);
    let memory!: WebAssembly.Memory;
    const decoder = new TextDecoder();
    const fwLog: string[] = [];
    const imports = {
        env: {
            js_log(ptr: number, len: number) { fwLog.push(decoder.decode(new Uint8Array(memory.buffer, ptr, len))); },
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
    if (exp.emu_init() !== 1) throw new Error("emu_init() failed");

    function fbSnapshot(): Uint8Array {
        return new Uint8Array(memory.buffer, exp.emu_fb(), PANEL_W * PANEL_H * 2).slice();
    }
    function pushRects(): PushRect[] {
        const n = exp.emu_push_count();
        const out: PushRect[] = [];
        for (let i = 0; i < n; i++) out.push({ x: exp.emu_push_x(i), y: exp.emu_push_y(i), w: exp.emu_push_w(i), h: exp.emu_push_h(i) });
        return out;
    }

    return {
        appSwitch(index: number) { exp.emu_app_switch(index); },
        appCurrent(): number { return exp.emu_app_current(); },
        touch(down: boolean, x: number, y: number) { exp.emu_touch(down ? 1 : 0, Math.round(x), Math.round(y)); },
        tick(nowMs: number) { exp.emu_tick(nowMs); },
        drainLog(): string[] { const out = fwLog.slice(); fwLog.length = 0; return out; },
        fbSnapshot,
        pushRects,
    };
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

// Checks ONE tick's worth of pushes against decision 0001 (row length a
// multiple of 8, window inside the panel) and full coverage (every pixel
// that actually changed lies inside the union of this tick's pushed
// rects) - the same technique feature-sketch-palette.ts's own
// tickChecked() uses, factored out here so it can be driven by either a
// clean touch() or a TouchSim report.
function checkTick(dev: Device, before: Uint8Array, rects: PushRect[], nowMs: number, label: string, violations: string[]) {
    for (const r of rects) {
        if (r.w % PUSH_ROW_MULTIPLE !== 0) {
            violations.push(`${label} t=${nowMs}: push (${r.x},${r.y},${r.w}x${r.h}) row length ${r.w} not a multiple of ${PUSH_ROW_MULTIPLE}`);
        }
        if (r.x < 0 || r.y < 0 || r.x + r.w > PANEL_W || r.y + r.h > PANEL_H) {
            violations.push(`${label} t=${nowMs}: push (${r.x},${r.y},${r.w}x${r.h}) outside the ${PANEL_W}x${PANEL_H} panel`);
        }
    }
    const after = dev.fbSnapshot();
    let anyDiff = false;
    for (let i = 0; i < before.length; i++) { if (before[i] !== after[i]) { anyDiff = true; break; } }
    if (!anyDiff) return after;
    for (let py = 0; py < PANEL_H; py++) {
        for (let px = 0; px < PANEL_W; px++) {
            const idx = (py * PANEL_W + px) * 2;
            if (before[idx] === after[idx] && before[idx + 1] === after[idx + 1]) continue;
            const inside = rects.some((r) => px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h);
            if (!inside) {
                violations.push(`${label} t=${nowMs}: pixel (${px},${py}) changed outside every pushed rect (${JSON.stringify(rects)})`);
                return after; // one report per tick is plenty
            }
        }
    }
    return after;
}

const TOUCHSIM_JITTER_PROFILE: TouchSimConfig = {
    ...TOUCHSIM_DEFAULTS,
    dropoutsEnabled: true, dropoutsPerSec: 34,
    straysEnabled: false,
    positionJitterEnabled: true, positionJitterPerSec: 1.5,
    positionJitterMinPx: 80, positionJitterMaxPx: 250, positionJitterMaxHoldReports: 3,
};

async function openPaletteOn(dev: Device, holdIndex: number, startMs: number): Promise<{ openedAtMs: number; t: number }> {
    const { cx, cy } = cellCentre(holdIndex);
    let t = startMs;
    let opened = false;
    for (let held = 0; held < CONFIRM_MS + LONG_PRESS_MS + 100 && !opened; held += 15) {
        t += 15;
        dev.touch(true, cx, cy);
        dev.tick(t);
        if (dev.drainLog().some((l) => l.includes("palette: open"))) opened = true;
    }
    if (!opened) throw new Error(`palette never opened on cell ${holdIndex}`);
    return { openedAtMs: t, t };
}

async function main() {
    console.log("=== feature: the palette's edge column (nearest the panel's right edge), the column the photograph shows broken ===\n");
    const violations: string[] = [];

    // ---- scenario A: open DIRECTLY on each edge-column cell and tick
    // through the WHOLE pop-in animation, checked frame by frame - not a
    // single big jump to "settled", every throttled frame in between,
    // which is exactly where a geometry bug would show first. ------------
    for (const openIdx of EDGE_COLUMN_INDICES) {
        const dev = await loadDevice();
        dev.tick(0);
        dev.appSwitch(APP_DRAW);
        dev.tick(1000);
        dev.drainLog();
        const { t: t0 } = await openPaletteOn(dev, openIdx, 1000);
        let t = t0;
        let prev = dev.fbSnapshot();
        for (let elapsed = 0; elapsed < PALETTE_ANIM_TOTAL_MS + 200; elapsed += 5) {
            t += 5;
            const { cx, cy } = cellCentre(openIdx);
            dev.touch(true, cx, cy);
            dev.tick(t);
            const rects = dev.pushRects();
            dev.drainLog();
            prev = checkTick(dev, prev, rects, t, `openOn(${openIdx}) pop-in`, violations);
        }
    }
    check(
        `opening directly on each edge-column cell (${EDGE_COLUMN_INDICES.join(",")}) and ticking through the whole pop-in never violates decision 0001 or leaves an unpushed changed pixel`,
        violations.length === 0,
        violations[0],
    );

    // ---- scenario B: open on the centre, settle, then drag with CLEAN
    // input from column 0 all the way to column 2, checked tick by tick -
    // the settled hand-off path, which is the one with no throttle before
    // this round and the one feature-sketch-palette.ts never drove past
    // column 0. ------------------------------------------------------------
    const violationsB: string[] = [];
    for (const row of [0, 1, 2]) {
        const dev = await loadDevice();
        dev.tick(0);
        dev.appSwitch(APP_DRAW);
        dev.tick(1000);
        dev.drainLog();
        const { t: t0 } = await openPaletteOn(dev, 4, 1000);
        let t = t0 + PALETTE_ANIM_TOTAL_MS + 50;
        dev.touch(true, cellCentre(4).cx, cellCentre(4).cy);
        dev.tick(t);
        dev.drainLog();
        let prev = dev.fbSnapshot();
        const from = cellCentre(row * 3), to = cellCentre(row * 3 + 2);
        const STEPS = 30;
        for (let i = 0; i <= STEPS; i++) {
            t += 15;
            const x = from.cx + (to.cx - from.cx) * (i / STEPS);
            dev.touch(true, Math.round(x), from.cy);
            dev.tick(t);
            const rects = dev.pushRects();
            dev.drainLog();
            prev = checkTick(dev, prev, rects, t, `clean drag row=${row}`, violationsB);
        }
    }
    check(
        "a clean drag from column 0 to column 2, every row, never violates decision 0001 or leaves an unpushed changed pixel",
        violationsB.length === 0,
        violationsB[0],
    );

    // ---- scenario C: the SAME drag, this time under the exact calibrated
    // dropout+jitter profile repro-touch-dropout-palette-open.ts already
    // validates against the open gesture - the controller defect this
    // project has already shipped a bug from once (see that file's own
    // header). --------------------------------------------------------------
    const violationsC: string[] = [];
    const STEP_MS = 1000 / TOUCHSIM_JITTER_PROFILE.reportRateHz;
    const TRIALS = 12;
    for (let trial = 0; trial < TRIALS; trial++) {
        const dev = await loadDevice();
        dev.tick(0);
        dev.appSwitch(APP_DRAW);
        dev.tick(1000);
        dev.drainLog();
        const { t: t0 } = await openPaletteOn(dev, 4, 1000);
        let t = t0 + PALETTE_ANIM_TOTAL_MS + 50;
        dev.touch(true, cellCentre(4).cx, cellCentre(4).cy);
        dev.tick(t);
        dev.drainLog();
        let prev = dev.fbSnapshot();
        const row = trial % 3;
        const from = cellCentre(row * 3), to = cellCentre(row * 3 + 2);
        const sim = new TouchSim(TOUCHSIM_JITTER_PROFILE, PANEL_W, PANEL_H, seededRng(seedFromName(`edge-column-trial-${trial}`)));
        sim.setPointer(true, from.cx, from.cy);
        const DRAG_MS = 1500;
        for (let held = 0; held < DRAG_MS; held += STEP_MS) {
            t += STEP_MS;
            const frac = held / DRAG_MS;
            sim.setPointer(true, from.cx + (to.cx - from.cx) * frac, from.cy);
            const report: TouchReport = sim.poll(t);
            dev.touch(report.fingers === 1, report.x, report.y);
            dev.tick(t);
            const rects = dev.pushRects();
            dev.drainLog();
            prev = checkTick(dev, prev, rects, t, `jittered drag trial=${trial} row=${row}`, violationsC);
        }
    }
    check(
        `a dropout+jitter drag (the calibrated profile) from column 0 to column 2 across ${TRIALS} trials never violates decision 0001 or leaves an unpushed changed pixel`,
        violationsC.length === 0,
        violationsC[0],
    );

    // ---- scenario D: THE ONE THIS FILE CAN ACTUALLY DRIVE RED. Force the
    // candidate to flip on every consecutive tick (no gap at all between
    // samples) and assert no two DIFFERENT-geometry palette pushes ever
    // land closer together than PALETTE_RENDER_MIN_MS. This is the
    // invariant sketch.c's palette_advance_animation() now enforces for
    // palette_render_handoff() the same way it always has for palette_
    // render_animating() - see that function's own comment for the
    // mechanism (decision 0001's still-open "per-row DMA re-arm race").
    // Before that throttle existed, this scenario reproduced two hand-off
    // pushes on consecutive 15ms ticks (< PALETTE_RENDER_MIN_MS apart);
    // it is included, checked and red-then-green, not asserted from
    // reading the diff. ------------------------------------------------
    {
        const dev = await loadDevice();
        dev.tick(0);
        dev.appSwitch(APP_DRAW);
        dev.tick(1000);
        dev.drainLog();
        const { t: t0 } = await openPaletteOn(dev, 4, 1000);
        let t = t0 + PALETTE_ANIM_TOTAL_MS + 50;
        dev.touch(true, cellCentre(4).cx, cellCentre(4).cy);
        dev.tick(t);
        dev.drainLog();

        // Alternate the touch between two far-apart cells on every 15ms
        // tick - the worst case a jittery controller could ever hand the
        // app (a genuinely new sample, every report, each on the opposite
        // side of the grid from the last).
        const a = cellCentre(0), b = cellCentre(8);
        let lastPushMs: number | null = null;
        let minGapMs = Infinity;
        let pushCount = 0;
        for (let i = 0; i < 40; i++) {
            t += 15;
            const cell = i % 2 === 0 ? a : b;
            dev.touch(true, cell.cx, cell.cy);
            dev.tick(t);
            const rects = dev.pushRects();
            dev.drainLog();
            if (rects.length > 0) {
                pushCount++;
                if (lastPushMs !== null) minGapMs = Math.min(minGapMs, t - lastPushMs);
                lastPushMs = t;
            }
        }
        check(
            "forcing the candidate to flip on every consecutive 15ms tick still never pushes two different palette windows closer together than PALETTE_RENDER_MIN_MS",
            pushCount < 2 || minGapMs >= PALETTE_RENDER_MIN_MS,
            `${pushCount} pushes fired, smallest gap ${minGapMs === Infinity ? "n/a" : minGapMs + "ms"} (floor ${PALETTE_RENDER_MIN_MS}ms)`,
        );
    }

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
