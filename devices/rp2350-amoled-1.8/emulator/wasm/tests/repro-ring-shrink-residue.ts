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
// Root cause of the ORIGINAL bug: an incremental repaint computing its own
// row range from exact old/new angles with no allowance for something
// drawn a little past the exact edge on purpose (then: a rounded cap's own
// bulge) not being swept again once the arc has moved past it. That defect
// CLASS, not that specific cause, is what this file has guarded ever since
// - see timer.c's own header for two more times it recurred as the coil's
// drawing changed shape.
//
// UPDATED 2026-08-14 FOR THE COIL REDESIGN, SEVERAL TIMES THE SAME DAY.
// Earlier passes of this file mirrored: the original single ring; six
// 10-minute-lap bands; two 30-minute-lap bands twice as thick; two EQUAL-
// width turns with a thin outline; an overlapping annulus with a halo and
// rounded caps. See timer.c's header, "CORRECTED 2026-08-14 (A FOURTH TIME,
// THE SAME DAY)", for the latest correction: the owner had the actual
// build in his hands and rejected the halo (width changing as it winds)
// and the rounded caps ("gros points") both. What replaced them: ONE
// constant-width ring for both laps, no inset, no caps at all, and exactly
// ONE cue - a thin white cut mark across the ring's full radial width at
// lap 2's own current leading edge. This file's own mirrors below are
// rewritten again for that: expectedAt()/scanViolations() (added for the
// halo pass) still work here almost unchanged in SHAPE (an independent
// reference model compared pixel-by-pixel against the real framebuffer),
// just against a much simpler expected picture - solid ink or track,
// interrupted only by the marker where lap 2 has one. Scenario D changes
// again too: there is no halo along the overlap to measure any more, so it
// now asserts the marker's own existence, position and (critically, since
// a moving marker is exactly the shape that leaves residue - see this
// file's own history above) that it leaves no trace behind as it advances.
//
// What did NOT change across any of these passes: the actual regression
// being guarded (no dense cluster of leftover ink after the arc/coil
// moves), the "worst 5-degree bucket" metric that told the original
// reported bug (42) apart from pre-existing rounding noise (2), and the two
// original scenarios (a real multi-sample drag up-then-down, and a smooth
// RUNNING countdown with no drag at all).
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
// "Ring geometry: the coil, CURRENT (a fourth time: constant width, one cut
// mark)" section for the derivation of every number here.
const RING_CX = 224, RING_CY = 184;
const DEG2RAD = Math.PI / 180;

const LAPS_MAX = 2;
const RING_OUTER_R = 173;
const RING_INNER_R = 141;
const RING_THICK_PX = RING_OUTER_R - RING_INNER_R; // 32, unchanged for the fourth time today

// timer.c's own cut-mark knob - see that file's "THE CUT MARKER" section
// for the full derivation (8px of arc length at the ring's own midline
// radius, 157, chosen because a single LOCALISED mark needs to clear a
// higher legibility bar than the circumferential bands this file's earlier
// passes validated at 4px).
const MARKER_HALF_DEG = 1.5;

// timer.c's flat, uniform tick step (2026-08-14 on) - see TICK_STEP_S/
// TICKS_PER_LAP/MAX_TICKS in timer.c.
const TICK_STEP_S = 5;
const TICKS_PER_LAP = 180;   // 15:00 / 5s
const MAX_TICKS = TICKS_PER_LAP * LAPS_MAX; // 360, i.e. 30:00
const TIMER_MAX_SECONDS = MAX_TICKS * TICK_STEP_S; // 1800

// Mirror of timer.c's compute_band_fill_degs(): each lap's own fraction of
// TICKS_PER_LAP, from a continuous total-ticks value. Unchanged mechanics -
// this is exactly as true for the marker-only coil as for every earlier
// layout, since fillDeg[0]/fillDeg[1] describe WHAT the coil means, not how
// it is painted.
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
const ANGLE_TOL_DEG = 6;
const RADIUS_TOL_PX = 2;

// The worst single 5-degree bucket's violation count that the PRE-EXISTING,
// direction-independent rounding noise alone can produce. Measured
// empirically below (kept as a named constant, same discipline every
// earlier version of this file used) rather than demanded to be exactly
// zero, since a handful of single-pixel roundings in the half-width table
// are a known, separate, non-regressing source of noise.
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
 * - mirrors timer.c's paint_band_row() (lap 1's own ink/track, the only ink
 * layer left) and paint_marker_row() (the cut mark, painted on top wherever
 * lap 2 has one) EXACTLY in CONTINUOUS geometry (true radius/angle from a
 * float centre), not the firmware's own per-row half-width TABLE, which
 * rounds to the nearest pixel per row (shapes_fill_half_width_table,
 * lroundf) - a genuine circle at the row level, not the pixel-corner level.
 * RADIUS_TOL_PX/ANGLE_TOL_DEG mark pixels close enough to a genuine zone
 * boundary (a radius equal to RING_INNER_R/RING_OUTER_R, or an angle equal
 * to 0/360/fillDeg0/the marker's own two edges) that the table-based
 * renderer could legitimately land either side of the true continuous line
 * - those pixels come back `ambiguous` and the scan below skips them.
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

    const nearRadiusBoundary = Math.abs(r - RING_INNER_R) <= RADIUS_TOL_PX || Math.abs(r - RING_OUTER_R) <= RADIUS_TOL_PX;

    let deg = Math.atan2(dx, -dy) * (180 / Math.PI);
    if (deg < 0) deg += 360;

    const markerLo = Math.max(0, fillDeg1 - MARKER_HALF_DEG);
    const markerHi = Math.min(360, fillDeg1 + MARKER_HALF_DEG);
    const nearAngleBoundary = [0, 360, fillDeg0, markerLo, markerHi].some((b) => angularDist(deg, b) <= ANGLE_TOL_DEG);

    if (nearRadiusBoundary || nearAngleBoundary) return { inRing: true, ambiguous: true };
    if (r < RING_INNER_R || r > RING_OUTER_R) return { inRing: false, ambiguous: false };

    const hasMarker = fillDeg1 > 0 && deg >= markerLo && deg < markerHi;
    if (hasMarker) return { inRing: true, ambiguous: false, expected: "white" };

    const coveredByLap1 = deg < fillDeg0;
    return { inRing: true, ambiguous: false, expected: coveredByLap1 ? "black" : "grey" };
}

type Violation = { lx: number; ly: number; deg: number; expected: string; actual: string };

// Scans the coil's own bounding square (radius RING_OUTER_R+3, comfortably
// past the ring's own outer edge - the digit block's own radius tops out
// well inside RING_INNER_R, see timer.c's digit-clearance derivation, so it
// is excluded by expectedAt()'s own `inRing` check by construction) and
// flags every pixel whose ACTUAL colour disagrees with expectedAt()'s
// prediction for the GIVEN fillDeg[0]/fillDeg[1]. Has to be state-aware
// (the marker is state-dependent, not a fixed radius band) - see this
// file's header.
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
            out.push({ lx, ly, deg, expected: e.expected!, actual });
        }
    }
    return out;
}

// Worst single 5-degree bucket among the violations - the metric this test
// actually gates on (see header comment on why a dense cluster, not a bare
// non-zero count, is what distinguishes a real regression from pre-existing
// rounding noise).
function worstBucket(violations: Violation[]): { key: number; count: number } {
    const buckets = new Map<number, number>();
    for (const v of violations) {
        const b = Math.floor(v.deg / 5) * 5;
        buckets.set(b, (buckets.get(b) ?? 0) + 1);
    }
    let worst = { key: -1, count: 0 };
    for (const [key, c] of buckets) if (c > worst.count) worst = { key, count: c };
    return worst;
}

// Finds every WHITE pixel within the ring's own CORE radial span (inset
// from the outer/inner edge by RADIUS_TOL_PX) - used by scenario D to find
// the cut marker directly. Deliberately excludes the outer/inner edge
// itself: that is scanViolations()'s own job (ordinary table-rounding noise
// there is a separate, already-covered concern), and the marker spans the
// ring's FULL radial width by construction, so a real marker shows up
// across virtually this entire core span regardless of the small edge
// exclusion.
function findWhiteInRing(fb: Uint8Array): { lx: number; ly: number; deg: number; r: number }[] {
    const found: { lx: number; ly: number; deg: number; r: number }[] = [];
    const scan = RING_OUTER_R + 1;
    const coreOuter = RING_OUTER_R - RADIUS_TOL_PX;
    const coreInner = RING_INNER_R + RADIUS_TOL_PX;
    for (let ly = RING_CY - scan; ly <= RING_CY + scan; ly++) {
        for (let lx = RING_CX - scan; lx <= RING_CX + scan; lx++) {
            const dx = lx - RING_CX, dy = ly - RING_CY;
            const r = Math.sqrt(dx * dx + dy * dy);
            if (r < coreInner || r > coreOuter) continue;
            if (classify(fb, lx, ly) !== "white") continue;
            let deg = Math.atan2(dx, -dy) * (180 / Math.PI);
            if (deg < 0) deg += 360;
            found.push({ lx, ly, deg, r });
        }
    }
    return found;
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

    // ---- scenario C: a clean drag (no dropouts, no lifts) sweeping through
    // a wide range of angles - including well into lap 2, so the cut marker
    // sweeps through many positions - must leave no residue anywhere in the
    // ring, checked with the SAME comparator as scenarios A/B. This is the
    // direct descendant of this file's old gap-residue check (added for the
    // owner's second report on the two-band coil, "sur le minuteur j'ai des
    // pixels qui stray autour de l'anneau"): that check scanned one FIXED
    // radius band forever, because the two-band coil's own gap never moved.
    // Neither the halo pass's margin nor this pass's marker sit at a fixed
    // radius any more, so this scenario goes through scanViolations() like
    // A/B rather than a bespoke fixed-band scan. Deliberately a CLEAN drag
    // (a plain mouse-style touch stream, same as dragTo() used everywhere
    // else in this file): the owner's original report reproduced this way
    // directly, with no dropouts or touch imperfection involved at all. ---
    {
        const dev = await loadDevice();
        dev.tick(0);
        dev.appSwitch(APP_TIMER);
        dev.tick(1000);

        console.log("\n-- clean drag through almost two full laps, sweeping the marker through many positions --");
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
            "a clean drag sweeping the marker through many angles leaves no dense residue cluster",
            worst.count <= MAX_VIOLATIONS_PER_BUCKET,
            `worst bucket=${worst.key} count=${worst.count}, threshold=${MAX_VIOLATIONS_PER_BUCKET}, total=${violations.length}`,
        );
    }

    // ---- scenario D: distinguishability, rewritten a third time for the
    // marker-only design (this file's header, "CORRECTED... A FOURTH
    // TIME"). There is no halo along the overlap to measure any more - lap
    // 1's ink and lap 2's ink are the SAME black, everywhere, except at lap
    // 2's own leading edge. So this asserts exactly what the task names:
    // on a one-lap coil (fillDeg[1] == 0) there is no white band anywhere
    // in the ring; on a two-lap coil the ring is fully inked except for one
    // thin radial white band at the head, that band sits at fillDeg[1]'s
    // own angle, spans the ring's FULL radial width, and - the part this
    // dial has gotten wrong twice before (see this file's own header) -
    // leaves NO TRACE behind at its old position as it advances, checked
    // through both a long continuous drag (many small incremental updates)
    // and a subsequent large JUMP (a fresh touch-down landing far from the
    // marker's last position, the same shape of update the ORIGINAL bug in
    // this file needed). ---------------------------------------------------
    {
        // ONE LAP, MID-WIND: fillDeg[1] is 0 throughout lap 1, whether or
        // not lap 1 itself has finished winding - no marker should exist at
        // any point during lap 1. Checked mid-lap (90 ticks, half of
        // TICKS_PER_LAP) rather than only at completion, since "no marker
        // during lap 1" is a claim about fillDeg[1], not about how much of
        // lap 1 has wound.
        const midLap1 = await loadDevice();
        midLap1.tick(0);
        midLap1.appSwitch(APP_TIMER);
        midLap1.tick(1000);
        const midSim = new DragSim();
        dragTo(midLap1, midSim, 0, 180, 1100, 100); // half a lap
        console.log(`\n-- scenario D, mid-lap-1: ${midSim.ticks} ticks (${(midSim.ticks * TICK_STEP_S / 60).toFixed(2)} min) --`);
        check(
            "mid-lap-1 drag actually stayed within lap 1",
            midSim.ticks > 0 && midSim.ticks < TICKS_PER_LAP,
            `ticks=${midSim.ticks}`,
        );
        const midWhites = findWhiteInRing(midLap1.fbBytes());
        check(
            "no white band anywhere in the ring during lap 1 (fillDeg[1] == 0, no marker needed)",
            midWhites.length === 0,
            `found ${midWhites.length} white pixel(s)${midWhites.length > 0 ? ", e.g. " + JSON.stringify(midWhites[0]) : ""}`,
        );

        // TWO LAPS, VIA A LONG CONTINUOUS DRAG: 0 -> 600deg in one
        // unbroken touch context (many small incremental updates, the same
        // shape RUNNING's own per-frame shrink and a real winding finger
        // both take) - if the marker ever left a trace behind an earlier
        // position along the way, it would still be sitting in the final
        // frame as a stray white pixel far from the FINAL fillDeg[1].
        const twoLap = await loadDevice();
        twoLap.tick(0);
        twoLap.appSwitch(APP_TIMER);
        twoLap.tick(1000);
        const twoSim = new DragSim();
        let t2 = dragTo(twoLap, twoSim, 0, 600, 1100, 300); // ~1.67 laps, many samples
        const [fillDeg0, fillDeg1] = fillDegsForTicks(twoSim.ticks);
        console.log(`-- scenario D, two-lap coil after a long continuous drag: ${twoSim.ticks} ticks (${(twoSim.ticks * TICK_STEP_S / 60).toFixed(2)} min), fillDeg1=${fillDeg1.toFixed(1)} --`);
        check(
            "two-lap drag actually wound past lap 2's own start",
            twoSim.ticks > TICKS_PER_LAP,
            `ticks=${twoSim.ticks}`,
        );

        const twoFb1 = twoLap.fbBytes();
        const whites1 = findWhiteInRing(twoFb1);
        console.log(`    white pixels found: ${whites1.length}`);
        check(
            "a two-lap coil DOES have a white band somewhere in the ring",
            whites1.length > 0,
            `found ${whites1.length}`,
        );
        const offBand1 = whites1.filter((w) => angularDist(w.deg, fillDeg1) > MARKER_HALF_DEG + ANGLE_TOL_DEG);
        check(
            "every white pixel found sits within the marker's own angular window at fillDeg1 - nothing left behind by the long drag that got it here",
            offBand1.length === 0,
            `${offBand1.length} pixel(s) outside the marker's window, e.g. ${JSON.stringify(offBand1[0])}`,
        );
        if (whites1.length > 0) {
            // The head's mark used to be a straight radial cut spanning the
            // ring's full width, and this assertion checked exactly that.
            // It is now an arc that FADES OUT where it meets the ring's two
            // borders, because the owner read the old full-weight version as
            // "un demi-cercle dans le coil" rather than a closure of it.
            //
            // So the property worth pinning is no longer the span, it is the
            // taper: the mark must live around the ring's midline and must
            // NOT reach either border, since reaching them is precisely what
            // made it read as a second border rather than an end.
            const radii = whites1.map((w) => w.r);
            const midR = (RING_OUTER_R + RING_INNER_R) / 2;
            const worst = Math.max(...radii.map((r) => Math.abs(r - midR)));
            const halfBand = RING_THICK_PX / 2;
            check(
                "the head's mark tapers away before the ring's borders, instead of butting into them",
                worst < halfBand - 1,
                `furthest bright pixel is ${worst.toFixed(1)}px from the midline, border is at ${halfBand}px`,
            );
        }

        // A LARGE JUMP FROM HERE: a fresh touch-down (point_touch(), not a
        // continuing drag) landing far from the marker's current position -
        // the same shape of update (an incremental repaint driven by a big
        // angle change, not a smooth walk through every intermediate angle)
        // that produced the ORIGINAL bug this file exists to catch. Still
        // inside lap 2 (a tap preserves the current lap - see timer.c's
        // point_touch() comment) so the marker should simply relocate.
        const [px, py] = panelTouchForAngle(90);
        twoLap.touch(true, px, py);
        t2 += 16;
        twoLap.tick(t2);
        twoLap.touch(false, 0, 0);
        t2 += 50;
        twoLap.tick(t2);

        const twoFb2 = twoLap.fbBytes();
        const whites2 = findWhiteInRing(twoFb2);
        // point_touch()'s own contract (timer.c): a tap preserves whichever
        // lap is currently wound and sets the SUB-LAP position to the
        // tapped angle - sub_lap_ticks_for_angle(90) at TICKS_PER_LAP=180 is
        // round(90/360*180)=45. Mirrored here from twoSim.ticks (not
        // hardcoded) so this stays correct however the preceding drag
        // actually landed.
        const lapAtTap = Math.floor(twoSim.ticks / TICKS_PER_LAP);
        const newTotalAfterTap = Math.min(MAX_TICKS, lapAtTap * TICKS_PER_LAP + 45);
        const expectedFillDeg1After = fillDegsForTicks(newTotalAfterTap)[1];
        console.log(`    after a large jump to angle 90deg: white pixels found=${whites2.length}, expected marker near ${expectedFillDeg1After.toFixed(1)}deg`);
        const offBand2 = whites2.filter((w) => angularDist(w.deg, expectedFillDeg1After) > MARKER_HALF_DEG + ANGLE_TOL_DEG);
        check(
            "after a large jump, the marker relocates cleanly - nothing left behind at the old position",
            whites2.length > 0 && offBand2.length === 0,
            `found ${whites2.length}, ${offBand2.length} outside the new window, e.g. ${JSON.stringify(offBand2[0])}`,
        );

        // Sanity check the same conclusion holds at the cheap whole-frame
        // level too: the mid-lap-1 coil (no marker) and the two-lap coil
        // (a marker) must not hash identically.
        const midHash = Bun.hash(midLap1.fbBytes());
        const twoHash = Bun.hash(twoFb2);
        check(
            "a mid-lap-1 coil and a two-lap coil produce different framebuffers overall",
            midHash !== twoHash,
            `midHash=${midHash} twoHash=${twoHash}`,
        );
    }

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
