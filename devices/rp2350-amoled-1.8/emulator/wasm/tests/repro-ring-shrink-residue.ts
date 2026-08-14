// repro-ring-shrink-residue: headless reproduction of the timer ring's
// "black dashes" bug, reported by the owner from a camera pointed at the
// real device: after dragging the ring up to a big value and then a second
// drag snapped it down to a much smaller one, part of the light grey track
// on the right side of the ring stayed broken into black horizontal dashes,
// roughly between 1:30 and 4 o'clock. A framebuffer dump over USB showed
// the identical dashes, which rules out panel timing/tearing: the panel is
// faithfully displaying an image that is already wrong in memory. Run with:
//
//   bun run emulator/wasm/tests/repro-ring-shrink-residue.ts
//
// Root cause (see firmware/apps/timer.c's CAP_SWEEP_MARGIN_DEG comment):
// NOT the incremental repaint's row-range bound being too small for a big
// single jump - ring_sweep_row_range's own bound is exact for any single
// old-angle/new-angle pair, jump size does not matter (checked by brute
// force against every pixel the true angle range touches). The bug is that
// the arc's moving end cap is a real disc (drawn a few degrees wide on
// BOTH sides of its exact angle, on purpose, for the rounded-end look), and
// update_ring_to's very next call computes its row range from the EXACT
// old/new angles with no allowance for the previous call's cap having
// physically painted a little past the "old" edge. Once the arc has moved
// on, nothing ever sweeps that sliver again: for a shrinking arc, every
// later call's angle range only gets smaller, so a sliver left just past
// an old edge is stuck for good.
//
// UPDATED 2026-08-14 FOR THE COIL REDESIGN, SEVERAL TIMES THE SAME DAY. This
// test is kept and extended, not replaced: the underlying defect class (a
// rounded cap's own overshoot getting stranded by an incremental repaint
// that does not account for it) is exactly what the coil's
// update_ring_to() still has to get right. Earlier passes of this file
// mirrored: the original single ring; six 10-minute-lap bands; two
// 30-minute-lap bands twice as thick; then two EQUAL-width turns separated
// by a thin outline (BAND_GAP_PX). All of those put the two laps SIDE BY
// SIDE, radially - see timer.c's header, "CORRECTED 2026-08-14 (A THIRD
// TIME, THE SAME DAY)", for why that was wrong twice in a row: the owner
// sent a picture (Apple Fitness's own activity ring past 100%) that settled
// it as ONE annulus, both laps at the SAME radius, lap 2 painted ON TOP of
// lap 1 and told apart by a HALO (a thin band of bare paper) rather than by
// position. This file's own mirrors below are rewritten, not just retuned,
// for that reason: there is no longer a fixed "gap" or "outline" radius to
// scan for residue in - the halo appears and disappears with lap 2's own
// fillDeg[1], so this file's old findStrayBlackPixels()/findGapResidue()
// pair (built around fixed, disjoint per-band radii) no longer describes
// anything real. They are replaced by one comparator, scanViolations(),
// that predicts the expected colour of every coil pixel from the coil's
// current fillDeg[0]/fillDeg[1] and flags any actual pixel that disagrees -
// see that function's own header for the full reasoning.
//
// What did NOT change across any of these passes: the actual regression
// being guarded (no dense cluster of leftover ink after the arc/coil
// moves), the "worst 5-degree bucket" metric that told the original
// reported bug (42) apart from pre-existing rounding noise (2), and the two
// original scenarios (a real multi-sample drag up-then-down, and a smooth
// RUNNING countdown with no drag at all). A fourth scenario, added for the
// two-equal-turns pass and kept (in updated form) for this one, proves a
// one-lap and a two-lap coil are actually DISTINGUISHABLE in the
// framebuffer - see that scenario's own header for what changed about how
// it measures that.
//
// This loads the REAL firmware compiled to wasm (emulator/wasm/dist/emu.wasm,
// built by emulator/wasm/build.ts) and drives it through emu_tick() with a
// synthetic clock, same shape as the other repro-*.ts tests. No internals
// are touched directly: every assertion is on the framebuffer itself,
// scanned pixel by pixel - a person at the device could see the exact same
// thing by looking at the coil.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

const PANEL_W = 368;
const PANEL_H = 448;
const APP_TIMER = 2; // g_apps[] = { chrono, sketch("draw"), timer }
const BTN_PWR = 1;

// timer.c's coil geometry, landscape coordinates - lifted, not re-derived,
// same convention every repro test in this directory uses. See timer.c's
// "Ring geometry: the coil, CURRENT (overlapping annulus)" section for the
// derivation of every number here.
const RING_CX = 224, RING_CY = 184;
const DEG2RAD = Math.PI / 180;

const LAPS_MAX = 2;
const RING_OUTER_R = 173;
const RING_INNER_R = 141;
const RING_THICK_PX = RING_OUTER_R - RING_INNER_R;      // 32, unchanged for the third time today
const RING_HALF_THICK_PX = RING_THICK_PX / 2;            // 16 - lap 1's own cap/halo-disc radius
const RING_CENTERLINE_R = (RING_OUTER_R + RING_INNER_R) / 2; // 157 - both laps' caps sit here now
const HALO_PX = 4;                                        // THE KNOB - halo width per edge, see timer.c's header
const INSET_OUTER_R = RING_OUTER_R - HALO_PX;             // 169
const INSET_INNER_R = RING_INNER_R + HALO_PX;             // 145
const INSET_HALF_THICK_PX = RING_HALF_THICK_PX - HALO_PX; // 12 - lap 2's own ink-disc radius

// timer.c's flat, uniform tick step (2026-08-14 on) - see TICK_STEP_S/
// TICKS_PER_LAP/MAX_TICKS in timer.c.
const TICK_STEP_S = 5;
const TICKS_PER_LAP = 180;   // 15:00 / 5s
const MAX_TICKS = TICKS_PER_LAP * LAPS_MAX; // 360, i.e. 30:00
const TIMER_MAX_SECONDS = MAX_TICKS * TICK_STEP_S; // 1800

// Mirror of timer.c's compute_band_fill_degs(): each lap's own fraction of
// TICKS_PER_LAP, from a continuous total-ticks value. Unchanged mechanics -
// this is exactly as true for the overlapping-annulus coil as for every
// earlier layout, since fillDeg[0]/fillDeg[1] describe WHAT the coil means,
// not how it is painted.
function fillDegsForTicks(ticks: number): number[] {
    const t = Math.max(0, Math.min(MAX_TICKS, ticks));
    const out: number[] = [];
    for (let b = 0; b < LAPS_MAX; b++) {
        const within = t - b * TICKS_PER_LAP;
        if (within <= 0) out.push(0);
        else if (within >= TICKS_PER_LAP) out.push(360);
        else out.push((within / TICKS_PER_LAP) * 360);
    }
    return out;
}

function fillDegsForRemainingSeconds(sec: number): number[] {
    return fillDegsForTicks(sec / TICK_STEP_S);
}

// Mirror of timer.c's point_touch()/drag_touch(): an independent
// re-implementation of the SAME algorithm (not a call into the firmware),
// used to predict what setTicks a given sequence of touch angles should
// land on - the coil's SETTING drag has no per-sample log line to read
// back (unlike a start/pause/resume transition), so this is the only way
// to know the expected value without re-deriving timer.c's own math a
// second, divergent way at the assertion site. Includes drag_touch()'s
// commit hysteresis (DRAG_COMMIT_HYSTERESIS_TICKS): without mirroring it
// here too, this simulator's predicted tick could disagree with the real
// firmware's by the hysteresis margin right after a drag stops near a
// commit boundary, which would show up as a false violation below.
class DragSim {
    ticks = 0;
    accum = 0;
    lastAngle = 0;

    point(angleDeg: number) {
        const lap = Math.floor(this.ticks / TICKS_PER_LAP);
        let sub = Math.round((angleDeg / 360) * TICKS_PER_LAP);
        if (sub >= TICKS_PER_LAP) sub -= TICKS_PER_LAP;
        if (sub < 0) sub = 0;
        let total = lap * TICKS_PER_LAP + sub;
        total = Math.max(0, Math.min(MAX_TICKS, total));
        this.ticks = total;
        this.accum = total;
        this.lastAngle = angleDeg;
    }

    drag(angleDeg: number) {
        let delta = angleDeg - this.lastAngle;
        if (delta > 180) delta -= 360;
        else if (delta < -180) delta += 360;
        this.lastAngle = angleDeg;
        this.accum += (delta / 360) * TICKS_PER_LAP;
        this.accum = Math.max(0, Math.min(MAX_TICKS, this.accum));
        const diff = this.accum - this.ticks;
        const HYSTERESIS = 0.15; // timer.c's DRAG_COMMIT_HYSTERESIS_TICKS
        if (diff >= 0.5 + HYSTERESIS || diff <= -(0.5 + HYSTERESIS)) {
            this.ticks = Math.round(this.accum);
        }
    }
}

// Generous tolerances for the table-based renderer's own rounding noise -
// see expectedAt()'s own header for exactly what these absorb.
// ANGLE_TOL_DEG covers both a cap's own intentional rounded bulge past the
// arc's exact edge (timer.c's CAP_SWEEP_MARGIN_DEG is 9.0deg as of this
// pass - see that constant's own derivation - so 6deg here is comfortably
// in the same order, not independently re-derived) and ordinary per-row
// half-width table rounding near a boundary.
const ANGLE_TOL_DEG = 6;
const RADIUS_TOL_PX = 2;

// The worst single (zone, 5-degree) bucket's violation count that the
// PRE-EXISTING, direction-independent cap/ring rounding noise alone can
// produce. Measured empirically below (kept as a named constant, same
// discipline every earlier version of this file used) rather than demanded
// to be exactly zero, since a handful of single-pixel roundings between a
// cap's own half-width table and the ring's own outer-radius table are a
// known, separate, non-regressing source of noise.
const MAX_VIOLATIONS_PER_BUCKET = 10;

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
    const fwLogLines: string[] = [];

    const imports = {
        env: {
            js_log(ptr: number, len: number) {
                const bytes = new Uint8Array(memory.buffer, ptr, len);
                const line = decoder.decode(bytes);
                fwLogLines.push(line);
                console.log(`    [fw] ${line}`);
            },
            sinf: (x: number) => Math.sin(x),
            cosf: (x: number) => Math.cos(x),
            atan2f: (y: number, x: number) => Math.atan2(y, x),
            sqrtf: (x: number) => Math.sqrt(x),
            fabsf: (x: number) => Math.abs(x),
            floorf: (x: number) => Math.floor(x),
            fmodf: (x: number, y: number) => x % y,
            powf: (x: number, y: number) => Math.pow(x, y),
            expf: (x: number) => Math.exp(x), // sound_synth.c's decay envelope; see emu_abi.h
        },
    };

    const instance = await WebAssembly.instantiate(mod, imports);
    memory = instance.exports.memory as WebAssembly.Memory;
    const exp = instance.exports as any;

    if (exp.emu_init() !== 1) {
        throw new Error("emu_init() failed - see [fw] log lines above");
    }

    return {
        tick(nowMs: number) {
            exp.emu_tick(nowMs);
        },
        touch(down: boolean, x: number, y: number) {
            exp.emu_touch(down ? 1 : 0, x, y);
        },
        button(index: number, down: boolean) {
            exp.emu_button(index, down ? 1 : 0);
        },
        buttonVerdict(index: number, isLong: boolean) {
            exp.emu_button_verdict(index, isLong ? 1 : 0);
        },
        appSwitch(index: number) {
            exp.emu_app_switch(index);
        },
        appCurrent(): number {
            return exp.emu_app_current();
        },
        fbBytes(): Uint8Array {
            const ptr = exp.emu_fb();
            return new Uint8Array(memory.buffer, ptr, PANEL_W * PANEL_H * 2).slice();
        },
        fwLogLines(): readonly string[] {
            return fwLogLines;
        },
    };
}

type Device = Awaited<ReturnType<typeof loadDevice>>;

// panel (px,py) that timer.c's raw_touch_angle_deg() will read back as
// landscape angle `deg` (0 at 12 o'clock, clockwise) - inverting
// gfx_land_rect's mapping, same as every other repro test in this
// directory. R is arbitrary (touch angle does not depend on radius); 150
// keeps it comfortably inside the coil's own outer bound.
function panelTouchForAngle(deg: number, R = 150): [number, number] {
    const theta = deg * DEG2RAD;
    const lx = RING_CX + R * Math.sin(theta);
    const ly = RING_CY - R * Math.cos(theta);
    const px = PANEL_W - 1 - ly;
    const py = lx;
    return [Math.round(px), Math.round(py)];
}

// landscape (lx,ly) for a point at the given angle/radius from the coil's
// own centre - the inverse of panelTouchForAngle's own geometry, but
// landing directly in LANDSCAPE space (no panel-coordinate round trip)
// since every pixel this file reads comes from classify()/isBlackAtLand(),
// which already expect landscape input.
function landAtAngleRadius(deg: number, r: number): [number, number] {
    const theta = deg * DEG2RAD;
    return [Math.round(RING_CX + r * Math.sin(theta)), Math.round(RING_CY - r * Math.cos(theta))];
}

// Classifies a landscape pixel as pure black (PX_BLACK, 0x0000), pure white
// (PX_WHITE, 0xFFFF) or "other" (the grey track colour, or - if something
// is genuinely wrong - some other value neither of the two flat inks this
// panel actually uses). This is deliberately a coarse 3-way bucket rather
// than trying to decode gray_to_px()'s own bit-packing in TypeScript: black
// and white are the only two colours anything in this file EXPECTS to see
// outside the track, so classifying everything else as "other" is enough
// to catch every violation class this file cares about without needing to
// reproduce gfx.h's own RGB565 packing here.
function classify(fb: Uint8Array, lx: number, ly: number): "black" | "white" | "other" {
    const px = PANEL_W - 1 - ly;
    const py = lx;
    const idx = (py * PANEL_W + px) * 2;
    const a = fb[idx], b = fb[idx + 1];
    if (a === 0 && b === 0) return "black";
    if (a === 0xff && b === 0xff) return "white";
    return "other";
}

function angularDist(a: number, b: number): number {
    const d = Math.abs(a - b) % 360;
    return d > 180 ? 360 - d : d;
}

/* ---------------------------------------------------------------------
 * expectedAt(): an independent reference model of what colour a given
 * landscape pixel SHOULD be, given the coil's current fillDeg[0]/fillDeg[1]
 * - mirrors timer.c's paint_band_row() (lap 1, full-width ink/track) and
 * paint_overlay_row() (lap 2, halo margins + inset ink) EXACTLY in
 * CONTINUOUS geometry (true radius/angle from a float centre), not the
 * firmware's own per-row half-width TABLE, which rounds to the nearest
 * pixel per row (shapes_fill_half_width_table, lroundf) - a genuine circle
 * at the row level, not the pixel-corner level, the same rounding this
 * file's own history (timer.c's cap-clipping fix) has already had to
 * account for once. RADIUS_TOL_PX/ANGLE_TOL_DEG mark pixels close enough to
 * a genuine zone boundary (a radius equal to RING_INNER_R/RING_OUTER_R/
 * INSET_INNER_R/INSET_OUTER_R, or an angle equal to 0/360/fillDeg0/
 * fillDeg1) that the table-based renderer could legitimately land either
 * side of the true continuous line - those pixels come back `ambiguous`
 * and the scan below skips them, exactly the same "generous tolerance
 * around a real, expected rounding edge" discipline every earlier version
 * of this file used, just computed from a model instead of read off a
 * fixed radius band.
 * ------------------------------------------------------------------- */
function expectedAt(
    lx: number,
    ly: number,
    fillDeg0: number,
    fillDeg1: number,
): { inRing: boolean; ambiguous: boolean; expected?: "black" | "white" | "grey" } {
    const dx = lx - RING_CX, dy = ly - RING_CY;
    const r = Math.sqrt(dx * dx + dy * dy);
    if (r < RING_INNER_R - RADIUS_TOL_PX || r > RING_OUTER_R + RADIUS_TOL_PX) {
        return { inRing: false, ambiguous: false };
    }

    const nearRadiusBoundary = [RING_INNER_R, RING_OUTER_R, INSET_INNER_R, INSET_OUTER_R].some(
        (b) => Math.abs(r - b) <= RADIUS_TOL_PX,
    );

    let deg = Math.atan2(dx, -dy) * (180 / Math.PI);
    if (deg < 0) deg += 360;
    const nearAngleBoundary = [0, 360, fillDeg0, fillDeg1].some((b) => angularDist(deg, b) <= ANGLE_TOL_DEG);

    if (nearRadiusBoundary || nearAngleBoundary) return { inRing: true, ambiguous: true };
    if (r < RING_INNER_R || r > RING_OUTER_R) return { inRing: false, ambiguous: false };

    const coveredByLap2 = deg < fillDeg1;
    const coveredByLap1 = deg < fillDeg0;
    const inInset = r >= INSET_INNER_R && r <= INSET_OUTER_R;

    if (coveredByLap2) return { inRing: true, ambiguous: false, expected: inInset ? "black" : "white" };
    if (coveredByLap1) return { inRing: true, ambiguous: false, expected: "black" };
    return { inRing: true, ambiguous: false, expected: "grey" };
}

type Violation = { lx: number; ly: number; deg: number; zone: string; expected: string; actual: string };

// Scans the coil's own bounding square (radius RING_OUTER_R+3, comfortably
// past the ring's own outer edge - the digit block's own radius tops out
// well inside RING_INNER_R, see timer.c's digit-clearance derivation, so it
// is excluded by expectedAt()'s own `inRing` check by construction, not by
// special-casing "black pixels near the centre are fine" here) and flags
// every pixel whose ACTUAL colour disagrees with expectedAt()'s prediction
// for the GIVEN fillDeg[0]/fillDeg[1] - the direct generalisation of the
// old findStrayBlackPixels()/findGapResidue() pair into one comparator that
// covers the halo margins, the inset ink, lap 1's own full-width ink and
// the plain grey track all at once. This has to be state-aware (unlike the
// old gap check, which was a fixed radius band checked unconditionally)
// because the halo itself is state-dependent now - see this file's header.
function scanViolations(fb: Uint8Array, fillDeg0: number, fillDeg1: number): Violation[] {
    const out: Violation[] = [];
    const scan = RING_OUTER_R + 3;
    for (let ly = RING_CY - scan; ly <= RING_CY + scan; ly++) {
        for (let lx = RING_CX - scan; lx <= RING_CX + scan; lx++) {
            const e = expectedAt(lx, ly, fillDeg0, fillDeg1);
            if (!e.inRing || e.ambiguous) continue;
            const actual = classify(fb, lx, ly);
            const matches =
                (e.expected === "black" && actual === "black") ||
                (e.expected === "white" && actual === "white") ||
                (e.expected === "grey" && actual === "other");
            if (matches) continue;
            const dx = lx - RING_CX, dy = ly - RING_CY;
            let deg = Math.atan2(dx, -dy) * (180 / Math.PI);
            if (deg < 0) deg += 360;
            const r = Math.sqrt(dx * dx + dy * dy);
            const zone = r >= INSET_INNER_R && r <= INSET_OUTER_R ? "ink" : "margin-or-track";
            out.push({ lx, ly, deg, zone, expected: e.expected!, actual });
        }
    }
    return out;
}

// Worst single (zone, 5-degree) bucket among the violations - the metric
// this test actually gates on (see header comment on why a dense cluster,
// not a bare non-zero count, is what distinguishes a real regression from
// pre-existing rounding noise).
function worstBucket(violations: Violation[]): { key: string; count: number } {
    const buckets = new Map<string, number>();
    for (const v of violations) {
        const b = Math.floor(v.deg / 5) * 5;
        const key = `${v.zone}:${b}`;
        buckets.set(key, (buckets.get(key) ?? 0) + 1);
    }
    let worst = { key: "n/a", count: 0 };
    for (const [key, c] of buckets) if (c > worst.count) worst = { key, count: c };
    return worst;
}

// Drags the coil's touch point smoothly from fromDeg to toDeg (an UNWRAPPED
// angle - may run past 360 or below 0 to represent several laps' worth of
// physical rotation, the same way a real finger just keeps turning the same
// circle) over `steps` samples at a 16ms frame interval (~60Hz), then
// lifts. Mirrors each sample into `sim` (a DragSim) in lockstep, so the
// caller can read back the expected setTicks afterwards without re-deriving
// it a second way.
function dragTo(dev: Device, sim: DragSim, fromDeg: number, toDeg: number, t0: number, steps: number): number {
    let t = t0;
    for (let i = 1; i <= steps; i++) {
        const deg = fromDeg + (toDeg - fromDeg) * (i / steps);
        const mod = ((deg % 360) + 360) % 360;
        const [px, py] = panelTouchForAngle(mod);
        dev.touch(true, px, py);
        if (i === 1) sim.point(mod); else sim.drag(mod);
        t += 16;
        dev.tick(t);
    }
    dev.touch(false, 0, 0);
    t += 50;
    dev.tick(t);
    return t;
}

function pwrShortClick(dev: Device, tPressMs: number) {
    dev.button(BTN_PWR, true);
    dev.tick(tPressMs);
    dev.button(BTN_PWR, false);
    dev.buttonVerdict(BTN_PWR, false);
    dev.tick(tPressMs + 50);
}

async function main() {
    console.log("=== reproduction + regression: coil residue after the arc shrinks ===\n");

    // ---- scenario A: the reported gesture, generalised to the coil - drag
    // up across several laps, then a second drag snaps it back down to a
    // small single-lap value, both with many realistic touch samples. ------
    {
        const dev = await loadDevice();
        dev.tick(0);
        dev.appSwitch(APP_TIMER);
        dev.tick(1000);
        check("switched into timer", dev.appCurrent() === APP_TIMER, `app_current()=${dev.appCurrent()}`);

        const sim = new DragSim();
        console.log("-- drag 1: 0 -> 1260deg (past the 2-lap/360-tick ceiling, so it also exercises clamping) over 180 samples (a real drag, not one touch) --");
        let t = dragTo(dev, sim, 0, 1260, 1000, 180);
        console.log("-- drag 2: a fresh touch-down, back down to 48deg (well under one lap) over 60 samples --");
        t = dragTo(dev, sim, 1260, 48, t + 200, 60);

        console.log(`    expected final ticks (mirrored point/drag sim) = ${sim.ticks} (${(sim.ticks * TICK_STEP_S / 60).toFixed(2)} min)`);
        const [fillDeg0, fillDeg1] = fillDegsForTicks(sim.ticks);

        const fb = dev.fbBytes();
        const violations = scanViolations(fb, fillDeg0, fillDeg1);
        const worst = worstBucket(violations);
        console.log(`    total violations=${violations.length}, worst bucket=${worst.key} count=${worst.count}`);
        check(
            "no dense cluster of residue in the coil after a multi-lap drag up-then-down",
            worst.count <= MAX_VIOLATIONS_PER_BUCKET,
            `worst bucket=${worst.key} count=${worst.count}, threshold=${MAX_VIOLATIONS_PER_BUCKET}, total=${violations.length}`,
        );
    }

    // ---- scenario B: no drag at all - a smooth RUNNING countdown leaves
    // the same kind of sliver, one per frame, without ever jumping, and
    // crosses at least one lap boundary as it unwinds. ---------------------
    {
        const dev = await loadDevice();
        dev.tick(0);
        dev.appSwitch(APP_TIMER);
        dev.tick(1000);

        console.log("\n-- drag to a value just past one lap boundary, start RUNNING, let 90s pass --");
        const sim = new DragSim();
        let t = dragTo(dev, sim, 0, 400, 1100, 30); // 400deg unwrapped, crosses the twelve o'clock branch cut once - exact resulting ticks read from sim.ticks below, not hand-computed (depends on hysteresis)
        pwrShortClick(dev, t + 50);
        t += 100;

        const startLine = dev.fwLogLines().find((l) => l.startsWith("timer: start, "));
        if (!startLine) throw new Error("did not see a 'timer: start, MM:SS' log line");
        const m = /timer: start, (\d+):(\d+)/.exec(startLine);
        if (!m) throw new Error(`could not parse start time from: ${startLine}`);
        const startSeconds = Number(m[1]) * 60 + Number(m[2]);
        console.log(`    (parsed start = ${startSeconds}s from "${startLine}")`);

        const endT = t + 90_000;
        while (t < endT) {
            t += 16;
            dev.tick(t);
        }

        const [fillDeg0, fillDeg1] = fillDegsForRemainingSeconds(startSeconds - 90);

        const fb = dev.fbBytes();
        const violations = scanViolations(fb, fillDeg0, fillDeg1);
        const worst = worstBucket(violations);
        console.log(`    total violations=${violations.length}, worst bucket=${worst.key} count=${worst.count}`);
        check(
            "no dense cluster of residue in the coil after a smooth RUNNING countdown across a lap boundary",
            worst.count <= MAX_VIOLATIONS_PER_BUCKET,
            `worst bucket=${worst.key} count=${worst.count}, threshold=${MAX_VIOLATIONS_PER_BUCKET}, total=${violations.length}`,
        );
    }

    // ---- scenario C: a clean drag (no dropouts, no lifts) sweeping caps
    // through a wide range of angles - including well into lap 2, so both
    // lap 1's own cap AND lap 2's own halo+ink cap sweep past many angles -
    // must leave no residue anywhere in the ring, checked with the SAME
    // comparator as scenarios A/B. This is the direct descendant of the
    // old findGapResidue() check (added for the owner's second report, "sur
    // le minuteur j'ai des pixels qui stray autour de l'anneau" - see
    // timer.c's header, "BUG 2", for the original mechanism this
    // reproduces: a rounded cap's own rasterisation overshoot landing a
    // pixel just past its own band's declared radius): that check scanned
    // one FIXED radius band forever, because the two-band coil's own gap
    // never moved. The overlapping annulus has no such fixed band any more
    // - the halo margin IS the thing this scenario has to prove clean, and
    // it only exists where lap 2 currently covers, so this scenario now
    // goes through scanViolations() like A/B rather than its own bespoke
    // gap scan. Deliberately a CLEAN drag (a plain mouse-style touch
    // stream, same as dragTo() used everywhere else in this file): the
    // owner's original report reproduced this way directly, with no
    // dropouts or touch imperfection involved at all. -------------------
    {
        const dev = await loadDevice();
        dev.tick(0);
        dev.appSwitch(APP_TIMER);
        dev.tick(1000);

        console.log("\n-- clean drag through almost two full laps, sweeping every cap angle at least once --");
        const sim = new DragSim();
        // 10 -> 700deg: crosses the lap boundary once and sweeps close to a
        // full second lap besides, the same wide angular coverage that
        // found the original defect.
        dragTo(dev, sim, 10, 700, 1100, 400);

        const [fillDeg0, fillDeg1] = fillDegsForTicks(sim.ticks);
        const fb = dev.fbBytes();
        const violations = scanViolations(fb, fillDeg0, fillDeg1);
        const worst = worstBucket(violations);
        console.log(`    total violations=${violations.length}, worst bucket=${worst.key} count=${worst.count}`);
        check(
            "a clean drag sweeping both laps' caps through many angles leaves no dense residue cluster",
            worst.count <= MAX_VIOLATIONS_PER_BUCKET,
            `worst bucket=${worst.key} count=${worst.count}, threshold=${MAX_VIOLATIONS_PER_BUCKET}, total=${violations.length}`,
        );
    }

    // ---- scenario D: one lap and two laps must be DISTINGUISHABLE in the
    // framebuffer - the entire point of the halo. THE MEASUREMENT CHANGED
    // AGAIN FROM THE PREVIOUS (equal-turns) PASS: that version told one lap
    // from two by RUN COUNT in a radial scan (one black run vs two, since
    // the turns sat side by side with an outline between them). The
    // overlapping annulus puts both laps at the SAME radius, so a radial
    // scan through a covered angle shows exactly ONE black run either way -
    // what differs is that run's own WIDTH and POSITION (a one-lap coil's
    // run spans close to the FULL ring, RING_INNER_R to RING_OUTER_R; a
    // two-lap coil's own run is HALO_PX narrower on each side, inset to
    // INSET_INNER_R..INSET_OUTER_R) - and, measured directly rather than
    // inferred from width alone, a SPECIFIC sample point at the halo's own
    // radius: solid black on a one-lap coil (lap 1's own ink, nothing has
    // narrowed it), pure white on a two-lap coil AT THE SAME ANGLE (the
    // halo itself, the white separation the task asks to prove exists) -
    // this is the direct measurement of "the halo produces a white
    // separation inside the ink that a single lap does not have". ---------
    {
        function radialProfile(fb: Uint8Array, angleDeg: number, rFrom: number, rTo: number): { r: number; black: boolean }[] {
            const out: { r: number; black: boolean }[] = [];
            for (let r = rFrom; r <= rTo; r++) {
                const [lx, ly] = landAtAngleRadius(angleDeg, r);
                out.push({ r, black: classify(fb, lx, ly) === "black" });
            }
            return out;
        }

        function blackRuns(profile: { r: number; black: boolean }[]): { rStart: number; rEnd: number }[] {
            const runs: { rStart: number; rEnd: number }[] = [];
            let open: { rStart: number; rEnd: number } | null = null;
            for (const p of profile) {
                if (p.black) {
                    if (!open) open = { rStart: p.r, rEnd: p.r };
                    else open.rEnd = p.r;
                } else if (open) {
                    runs.push(open);
                    open = null;
                }
            }
            if (open) runs.push(open);
            return runs;
        }

        const SCAN_R_FROM = RING_INNER_R - 3;
        const SCAN_R_TO = RING_OUTER_R + 3;
        const ANGLE = 180; // straight down (dx=0, dy>0) - clear of both caps in both scenarios below (the fixed
                            // start cap sits near 0deg/12 o'clock, and neither moving tip below lands near 180),
                            // so every sample here comes from the plain per-row bar/overlay painter, not cap
                            // rasterisation - a clean read of the two-layer structure itself.
        const HALO_SAMPLE_R = Math.round((INSET_OUTER_R + RING_OUTER_R) / 2); // ~171, inside the halo's own outer margin

        // ONE LAP: fully wind lap 1 and stop comfortably short of lap 2
        // reaching 180deg - any ticks in [TICKS_PER_LAP, TICKS_PER_LAP +
        // TICKS_PER_LAP/2) keeps fillDeg[1] under 180 (see
        // fillDegsForTicks), so the exact commit point (subject to drag
        // hysteresis) does not matter, only that it lands in that generous
        // window.
        const oneLap = await loadDevice();
        oneLap.tick(0);
        oneLap.appSwitch(APP_TIMER);
        oneLap.tick(1000);
        const oneSim = new DragSim();
        dragTo(oneLap, oneSim, 0, 370, 1100, 200); // one lap (360deg) + a small margin
        console.log(`\n-- scenario D, one-lap coil: ${oneSim.ticks} ticks (${(oneSim.ticks * TICK_STEP_S / 60).toFixed(2)} min) --`);
        check(
            "one-lap drag actually wound past one full lap, short of lap 2 reaching 180deg",
            oneSim.ticks >= TICKS_PER_LAP && oneSim.ticks < TICKS_PER_LAP + TICKS_PER_LAP / 2,
            `ticks=${oneSim.ticks}`,
        );
        const oneFb = oneLap.fbBytes();
        const oneRuns = blackRuns(radialProfile(oneFb, ANGLE, SCAN_R_FROM, SCAN_R_TO));
        console.log(`    radial profile runs (r=${SCAN_R_FROM}..${SCAN_R_TO} at angle ${ANGLE}deg): ${JSON.stringify(oneRuns)}`);
        check(
            "one lap paints ONE black run spanning close to the FULL ring width (no halo narrowing it)",
            oneRuns.length === 1 && (oneRuns[0].rEnd - oneRuns[0].rStart) >= RING_THICK_PX - 1 - 2 * RADIUS_TOL_PX,
            `runs=${JSON.stringify(oneRuns)}, full width=${RING_THICK_PX}`,
        );
        const oneHaloSample = classify(oneFb, ...landAtAngleRadius(ANGLE, HALO_SAMPLE_R));
        check(
            "at the halo's own radius, a single-lap coil (not double-covered here) shows solid ink, no halo yet",
            oneHaloSample === "black",
            `sample=${oneHaloSample} at r=${HALO_SAMPLE_R}`,
        );

        // TWO LAPS: wind well into the second lap, comfortably past the
        // point where lap 2's own fillDeg reaches 180deg (needs
        // within-lap-2 ticks > TICKS_PER_LAP/2 = 90).
        const twoLap = await loadDevice();
        twoLap.tick(0);
        twoLap.appSwitch(APP_TIMER);
        twoLap.tick(1000);
        const twoSim = new DragSim();
        dragTo(twoLap, twoSim, 0, 600, 1100, 300); // ~1.67 laps
        console.log(`-- scenario D, two-lap coil: ${twoSim.ticks} ticks (${(twoSim.ticks * TICK_STEP_S / 60).toFixed(2)} min) --`);
        check(
            "two-lap drag actually wound past lap 2's own 180deg point",
            twoSim.ticks > TICKS_PER_LAP + TICKS_PER_LAP / 2,
            `ticks=${twoSim.ticks}, need > ${TICKS_PER_LAP + TICKS_PER_LAP / 2}`,
        );
        const twoFb = twoLap.fbBytes();
        const twoRuns = blackRuns(radialProfile(twoFb, ANGLE, SCAN_R_FROM, SCAN_R_TO));
        console.log(`    radial profile runs (r=${SCAN_R_FROM}..${SCAN_R_TO} at angle ${ANGLE}deg): ${JSON.stringify(twoRuns)}`);
        check(
            "two laps paint ONE black run, INSET to the ink zone only - narrower than the one-lap coil's own run",
            twoRuns.length === 1 &&
                twoRuns[0].rStart >= INSET_INNER_R - RADIUS_TOL_PX && twoRuns[0].rStart <= INSET_INNER_R + RADIUS_TOL_PX &&
                twoRuns[0].rEnd >= INSET_OUTER_R - RADIUS_TOL_PX && twoRuns[0].rEnd <= INSET_OUTER_R + RADIUS_TOL_PX,
            `runs=${JSON.stringify(twoRuns)}, expected inset=[${INSET_INNER_R},${INSET_OUTER_R}]`,
        );
        const twoHaloSample = classify(twoFb, ...landAtAngleRadius(ANGLE, HALO_SAMPLE_R));
        check(
            "at the SAME halo-margin radius, a two-lap coil (double-covered here) shows WHITE - the halo itself, measured directly, not eyeballed",
            twoHaloSample === "white",
            `sample=${twoHaloSample} at r=${HALO_SAMPLE_R}`,
        );

        // Sanity check the same conclusion holds at the cheap whole-frame
        // level too, not just at one sampled angle/radius: a one-lap and a
        // two-lap coil must not hash identically.
        const oneHash = Bun.hash(oneFb);
        const twoHash = Bun.hash(twoFb);
        check(
            "a one-lap and a two-lap coil produce different framebuffers overall",
            oneHash !== twoHash,
            `oneHash=${oneHash} twoHash=${twoHash}`,
        );
    }

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
