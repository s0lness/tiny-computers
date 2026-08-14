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
// UPDATED 2026-08-14 FOR THE COIL REDESIGN, TWICE THE SAME DAY. This test
// is kept and extended, not replaced: the underlying defect class (a
// rounded cap's own overshoot getting stranded by an incremental repaint
// that does not account for it) is exactly what the coil's
// update_ring_to() still has to get right, now PER BAND and with the added
// risk of one band's sweep clobbering a DIFFERENT band's cap (see that
// function's own header comment). The original version of this file
// mirrored timer.c's now-superseded three-tier ticks table (5s/30s/1m) and
// a single ring's geometry (RING_CX/CY/RADIUS); the first coil rewrite
// mirrored six 10-minute-lap bands; the current firmware (see timer.c's
// header, "CORRECTED 2026-08-14 (LATER THE SAME DAY): 30 MINUTES A LAP,
// TWO LAPS") is two 30-minute-lap bands, twice as thick, and this file's
// own mirrors below are updated to match once more. What did NOT change
// across any of these: the actual regression being guarded (no dense
// cluster of leftover black pixels after the arc/coil moves), the "worst
// 5-degree bucket" metric that told the reported bug (42) apart from
// pre-existing rounding noise (2), and the two scenarios (a real multi-
// sample drag up-then-down, and a smooth RUNNING countdown with no drag at
// all).
//
// This loads the REAL firmware compiled to wasm (emulator/wasm/dist/emu.wasm,
// built by emulator/wasm/build.ts) and drives it through emu_tick() with a
// synthetic clock, same shape as the other repro-*.ts tests. No internals
// are touched directly: every assertion is on the framebuffer itself,
// scanned for stray black pixels - a person at the device could see the
// exact same thing by looking at the coil.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

const PANEL_W = 368;
const PANEL_H = 448;
const APP_TIMER = 2; // g_apps[] = { chrono, sketch("draw"), timer }
const BTN_PWR = 1;

// timer.c's coil geometry, landscape coordinates - lifted, not re-derived,
// same convention every repro test in this directory uses. See timer.c's
// "Ring geometry: the coil" section for the derivation of every number
// here.
const RING_CX = 224, RING_CY = 184;
const DEG2RAD = Math.PI / 180;

const LAPS_MAX = 2;
const BAND_THICK_PX = 6;
const BAND_GAP_PX = 4;
const BAND_STRIDE_PX = BAND_THICK_PX + BAND_GAP_PX; // 10
const RING_OUTER_R = 173;

function bandOuterR(b: number): number { return RING_OUTER_R - b * BAND_STRIDE_PX; }
function bandInnerR(b: number): number { return bandOuterR(b) - BAND_THICK_PX; }

// timer.c's flat, uniform tick step (2026-08-14 on) - see TICK_STEP_S/
// TICKS_PER_LAP/MAX_TICKS in timer.c.
const TICK_STEP_S = 5;
const TICKS_PER_LAP = 360;   // 30:00 / 5s
const MAX_TICKS = TICKS_PER_LAP * LAPS_MAX; // 720, i.e. 60:00
const TIMER_MAX_SECONDS = MAX_TICKS * TICK_STEP_S; // 3600

// Mirror of timer.c's compute_band_fill_degs(): each band's own fraction of
// TICKS_PER_LAP, from a continuous total-ticks value.
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
// commit hysteresis (DRAG_COMMIT_HYSTERESIS_TICKS, added 2026-08-14 for
// the wider-lap coil's finer per-tick resolution): without mirroring it
// here too, this simulator's predicted tick could disagree with the real
// firmware's by the hysteresis margin right after a drag stops near a
// commit boundary, which would show up as a false "stray pixel" below.
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
        const HYSTERESIS = 0.3; // timer.c's DRAG_COMMIT_HYSTERESIS_TICKS
        if (diff >= 0.5 + HYSTERESIS || diff <= -(0.5 + HYSTERESIS)) {
            this.ticks = Math.round(this.accum);
        }
    }
}

// Generous angular tolerance for the caps' own intentional rounded bulge
// past the arc's exact edge (see timer.c's CAP_SWEEP_MARGIN_DEG derivation,
// ~1.5deg): rounded up further here since this is a black-box pixel scan,
// not the firmware's own math.
const CAP_BULGE_TOLERANCE_DEG = 6;

// The worst single (band, 5-degree) bucket's stray-pixel count that the
// PRE-EXISTING, direction-independent cap/ring rounding noise alone can
// produce. Measured empirically below (kept as a named constant, same
// discipline the single ring's own version used) rather than demanded to
// be exactly zero, since a handful of single-pixel roundings between a
// cap's own half-width table and a band's own outer-radius table are a
// known, separate, non-regressing source of noise (see the single ring's
// retained comment on this in timer.c's history).
const MAX_STRAY_PER_BUCKET = 10;

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

function isBlackAtLand(fb: Uint8Array, lx: number, ly: number): boolean {
    const px = PANEL_W - 1 - ly;
    const py = lx;
    const idx = (py * PANEL_W + px) * 2;
    return fb[idx] === 0 && fb[idx + 1] === 0;
}

// Scans only the coil's own annulus TERRITORY - radius [RING_INNER_R-1,
// RING_OUTER_R+1] - and, for every black pixel there, works out which band
// (if any) it belongs to by radius; a black pixel in that territory but
// between two bands (a white gap) belongs to no band and is unconditionally
// stray. A black pixel that DOES belong to band b is stray unless its own
// clockwise-from-12 angle falls inside that band's own [0, fillDeg[b]] arc
// (plus the cap bulge tolerance on both the moving tip and the fixed start
// cap).
//
// Deliberately NOT a scan of the coil's whole bounding square, unlike a
// first draft of this file: RING_CY +/- RING_OUTER_R also contains the
// MM:SS digit block, which is legitimately solid black while RUNNING
// (digit_color_for_state) - the digits' own radius from the coil's centre
// tops out at the digit block's ~145.0px half-diagonal (see timer.c's
// digit-clearance derivation), safely inside RING_INNER_R-1 (149), so
// restricting the scan to the coil's own territory excludes them by
// construction rather than by special-casing "black pixels near the centre
// are fine" at every call site.
function findStrayBlackPixels(
    fb: Uint8Array,
    fillDeg: number[],
): { lx: number; ly: number; band: number; deg: number }[] {
    const stray: { lx: number; ly: number; band: number; deg: number }[] = [];
    const scanOuter = RING_OUTER_R + 1;
    const scanInner = bandInnerR(LAPS_MAX - 1) - 1;
    for (let ly = RING_CY - scanOuter; ly <= RING_CY + scanOuter; ly++) {
        for (let lx = RING_CX - scanOuter; lx <= RING_CX + scanOuter; lx++) {
            const dx = lx - RING_CX, dy = ly - RING_CY;
            const r = Math.sqrt(dx * dx + dy * dy);
            if (r < scanInner || r > scanOuter) continue;
            if (!isBlackAtLand(fb, lx, ly)) continue;

            let band = -1;
            for (let b = 0; b < LAPS_MAX; b++) {
                if (r >= bandInnerR(b) - 1 && r <= bandOuterR(b) + 1) { band = b; break; }
            }
            if (band < 0) {
                stray.push({ lx, ly, band: -1, deg: -1 });
                continue;
            }

            let deg = Math.atan2(dx, -dy) * (180 / Math.PI);
            if (deg < 0) deg += 360;
            const withinMovingArc = deg <= fillDeg[band] + CAP_BULGE_TOLERANCE_DEG;
            const withinFixedStartCap = deg >= 360 - CAP_BULGE_TOLERANCE_DEG;
            if (!withinMovingArc && !withinFixedStartCap) stray.push({ lx, ly, band, deg });
        }
    }
    return stray;
}

// Worst single (band, 5-degree) bucket among the stray pixels - the metric
// this test actually gates on (see header comment on why a dense cluster,
// not a bare non-zero count, is what distinguishes the reported bug from
// pre-existing rounding noise). A stray pixel with band==-1 (a gap/outside
// pixel) buckets into its own band=-1 group, angle bucket 0 - any such
// pixel is a hard failure class of its own (ink outside every band
// entirely), so lumping them together is fine: MAX_STRAY_PER_BUCKET still
// catches a handful of them, and the detail string below reports the raw
// total regardless.
function worstBucket(stray: { band: number; deg: number }[]): { key: string; count: number } {
    const buckets = new Map<string, number>();
    for (const s of stray) {
        const b = s.band < 0 ? -1 : Math.floor(s.deg / 5) * 5;
        const key = `${s.band}:${b}`;
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
    console.log("=== reproduction + regression: coil band residue after the arc shrinks ===\n");

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
        console.log("-- drag 1: 0 -> 1260deg (past the 2-lap/720-tick ceiling, so it also exercises clamping) over 180 samples (a real drag, not one touch) --");
        let t = dragTo(dev, sim, 0, 1260, 1000, 180);
        console.log("-- drag 2: a fresh touch-down, back down to 48deg (well under one lap) over 60 samples --");
        t = dragTo(dev, sim, 1260, 48, t + 200, 60);

        console.log(`    expected final ticks (mirrored point/drag sim) = ${sim.ticks} (${(sim.ticks * TICK_STEP_S / 60).toFixed(2)} min)`);
        const expectedFillDegs = fillDegsForTicks(sim.ticks);

        const fb = dev.fbBytes();
        const stray = findStrayBlackPixels(fb, expectedFillDegs);
        const worst = worstBucket(stray);
        console.log(`    total stray pixels=${stray.length}, worst bucket=${worst.key} count=${worst.count}`);
        check(
            "no dense cluster of black residue in the coil after a multi-lap drag up-then-down",
            worst.count <= MAX_STRAY_PER_BUCKET,
            `worst bucket=${worst.key} count=${worst.count}, threshold=${MAX_STRAY_PER_BUCKET}, total stray=${stray.length}`,
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

        console.log("\n-- drag to a value just past one lap boundary (~33 minutes), start RUNNING, let 90s pass --");
        const sim = new DragSim();
        let t = dragTo(dev, sim, 0, 400, 1100, 30); // 400deg = 400 ticks = 2000s = 33:20 (TICKS_PER_LAP=360)
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

        const expectedFillDegs = fillDegsForRemainingSeconds(startSeconds - 90);

        const fb = dev.fbBytes();
        const stray = findStrayBlackPixels(fb, expectedFillDegs);
        const worst = worstBucket(stray);
        console.log(`    total stray pixels=${stray.length}, worst bucket=${worst.key} count=${worst.count}`);
        check(
            "no dense cluster of black residue in the coil after a smooth RUNNING countdown across a lap boundary",
            worst.count <= MAX_STRAY_PER_BUCKET,
            `worst bucket=${worst.key} count=${worst.count}, threshold=${MAX_STRAY_PER_BUCKET}, total stray=${stray.length}`,
        );
    }

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
