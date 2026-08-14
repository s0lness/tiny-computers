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
// update_ring_to() still has to get right, now PER BAND and with the added
// risk of one band's sweep clobbering a DIFFERENT band's cap (see that
// function's own header comment). The original version of this file
// mirrored timer.c's now-superseded three-tier ticks table (5s/30s/1m) and
// a single ring's geometry (RING_CX/CY/RADIUS); the first coil rewrite
// mirrored six 10-minute-lap bands; a later pass mirrored two 30-minute-lap
// bands, twice as thick; this file's own mirrors below are updated to match
// once more.
//
// LATEST PASS: "un seul anneau" - a single ring, not two visibly separate
// bands. The owner's own words: "on va faire un seul anneau qui s'enroule
// sur lui-même et garde... la largeur de cet anneau [est] la largeur totale
// des deux trucs aujourd'hui, inclus l'outline au milieu" - and, after an
// intermediate reading (each pass thickening the ring) was tried and
// corrected before it was ever built: "je ne suis pas sûr que tu doives
// augmenter la largeur au deuxième passage... ça doit rester la même
// largeur à chaque fois." What actually shipped (see timer.c's header,
// "CORRECTED 2026-08-14 (STILL LATER THE SAME DAY)"): the SAME mechanism
// this file has always mirrored - two annuli, wound inward, with an
// unpainted band between them - just retuned so the two annuli are wider
// (14px each, equal) and the unpainted band between them is much thinner
// (4px, a "thin outline" rather than the previous pass's fat 8px gap),
// while the ring's own total span (32px) and outer/inner radii (173/141)
// stay exactly what they were. Because the mechanism itself never changed,
// this file's own scanning logic (findStrayBlackPixels/findGapResidue)
// needed no rewrite either - only the geometry MIRROR constants below move,
// same as every earlier pass. A NEW scenario (D, at the end of this file) is
// added for this pass specifically: the previous scenarios all guard
// "nothing leaks", but nothing so far has proven that a one-lap coil and a
// two-lap coil actually look DIFFERENT to begin with, which is the entire
// point of two equal-width turns rather than one - see that scenario's own
// header for what it measures and why (a radial ink profile, not a single
// thickness number, since a thickness comparison across turns of EQUAL
// width would trivially show nothing).
//
// What did NOT change across any of these passes: the actual regression
// being guarded (no dense cluster of leftover black pixels after the
// arc/coil moves), the "worst 5-degree bucket" metric that told the
// reported bug (42) apart from pre-existing rounding noise (2), and the two
// original scenarios (a real multi-sample drag up-then-down, and a smooth
// RUNNING countdown with no drag at all).
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
// RING_THICK_PX (the total 32px span) is UNCHANGED across this pass - only
// how it is divided between "turn" and "outline" moves. BAND_GAP_PX is
// timer.c's own named knob for the outline's weight (see that file's
// header); BAND_THICK_PX is DERIVED from it exactly the way timer.c derives
// it, not an independent number that could drift out of sync.
const RING_THICK_PX = 32;
const BAND_GAP_PX = 4;                                    // THE KNOB - SUPERSEDED FROM 8
const BAND_THICK_PX = (RING_THICK_PX - BAND_GAP_PX) / LAPS_MAX; // 14, DERIVED - SUPERSEDED FROM 12
const BAND_STRIDE_PX = BAND_THICK_PX + BAND_GAP_PX; // 18, SUPERSEDED FROM 20
const RING_OUTER_R = 173;  // unchanged - see timer.c's "Ring geometry: the coil, CURRENT"

function bandOuterR(b: number): number { return RING_OUTER_R - b * BAND_STRIDE_PX; }
function bandInnerR(b: number): number { return bandOuterR(b) - BAND_THICK_PX; }

// timer.c's flat, uniform tick step (2026-08-14 on) - see TICK_STEP_S/
// TICKS_PER_LAP/MAX_TICKS in timer.c.
const TICK_STEP_S = 5;
const TICKS_PER_LAP = 180;   // 15:00 / 5s - SUPERSEDED FROM 360 (real-hardware pass)
const MAX_TICKS = TICKS_PER_LAP * LAPS_MAX; // 360, i.e. 30:00 - SUPERSEDED FROM 720/60:00
const TIMER_MAX_SECONDS = MAX_TICKS * TICK_STEP_S; // 1800 - SUPERSEDED FROM 3600

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
        const HYSTERESIS = 0.15; // timer.c's DRAG_COMMIT_HYSTERESIS_TICKS - SUPERSEDED FROM 0.3 (real-hardware pass, halved with TICKS_PER_LAP)
        if (diff >= 0.5 + HYSTERESIS || diff <= -(0.5 + HYSTERESIS)) {
            this.ticks = Math.round(this.accum);
        }
    }
}

// Generous angular tolerance for the caps' own intentional rounded bulge
// past the arc's exact edge (see timer.c's CAP_SWEEP_MARGIN_DEG derivation,
// ~4.5deg as of the single-ring merge pass, SUPERSEDED FROM ~3.5deg, itself
// SUPERSEDED FROM ~1.5deg): rounded up further here since this is a
// black-box pixel scan, not the firmware's own math. Still comfortably
// above the firmware's own 4.5deg margin, so no change needed beyond the
// comment.
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

// Scans STRICTLY the white gap between the two bands - radius (bandOuterR(1),
// bandInnerR(0)), i.e. EXCLUDING both bands' own +-1px membership tolerance
// (unlike findStrayBlackPixels above, which is an ANGULAR tolerance check
// per band and folds gap pixels into its own band=-1 bucket alongside a
// generous CAP_BULGE_TOLERANCE_DEG) - added 2026-08-14 (real-hardware pass)
// for the owner's second report, "sur le minuteur j'ai des pixels qui stray
// autour de l'anneau": a handful of permanently-black pixels landing in the
// gap between bands, caused by a rounded cap's own rasterisation overshoot
// (shapes_fill_half_width_table rounding a corner pixel up, stacked with
// cap_center()'s own lroundf() snap) reaching about 1px past its band's true
// radius - see timer.c's header, "BUG 2", for the full mechanism and the
// draw_cap_row_clipped() fix. This check is RADIUS-only and angle-agnostic
// (checks EVERY angle around the whole gap, not just where a cap happened to
// sweep in one specific scenario) and gates on an ABSOLUTE ZERO, not a
// bucketed threshold like MAX_STRAY_PER_BUCKET: the gap must never contain
// ink, full stop - there is no "acceptable noise" class for it the way there
// is for a cap's own angular bulge past its arc's exact edge.
function findGapResidue(fb: Uint8Array): { lx: number; ly: number; r: number }[] {
    const found: { lx: number; ly: number; r: number }[] = [];
    const gapOuter = bandInnerR(0) - 1;   // just inside band 0's own tolerance
    const gapInner = bandOuterR(LAPS_MAX - 1) + 1; // just outside the innermost band's own tolerance
    for (let ly = RING_CY - gapOuter - 1; ly <= RING_CY + gapOuter + 1; ly++) {
        for (let lx = RING_CX - gapOuter - 1; lx <= RING_CX + gapOuter + 1; lx++) {
            const dx = lx - RING_CX, dy = ly - RING_CY;
            const r = Math.sqrt(dx * dx + dy * dy);
            if (r <= gapInner || r >= gapOuter) continue;
            if (isBlackAtLand(fb, lx, ly)) found.push({ lx, ly, r });
        }
    }
    return found;
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
        console.log("-- drag 1: 0 -> 1260deg (past the 2-lap/360-tick ceiling, so it also exercises clamping) over 180 samples (a real drag, not one touch) --");
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

    // ---- scenario C: gap residue - a clean drag (no dropouts, no lifts)
    // sweeping caps through a wide range of angles must never leave a black
    // pixel in the white gap between the two bands - see findGapResidue()'s
    // own header comment for the mechanism this reproduces. Deliberately a
    // CLEAN drag (a plain mouse-style touch stream, same as dragTo() used
    // everywhere else in this file): the owner's report reproduced this way
    // directly, with no dropouts or touch imperfection involved at all,
    // unlike the wrap-to-zero bug this file's sibling test
    // (repro-timer-coil.ts's scenario 6) covers. -------------------------
    {
        const dev = await loadDevice();
        dev.tick(0);
        dev.appSwitch(APP_TIMER);
        dev.tick(1000);

        console.log("\n-- clean drag through almost two full laps, sweeping every cap angle at least once --");
        const sim = new DragSim();
        // 10 -> 700deg: crosses the lap boundary once and sweeps close to a
        // full second lap besides, the same wide angular coverage that
        // found the original defect (a cap near 117deg left a permanently
        // black pixel at radius 165.9 against this file's PREVIOUS 6px-band
        // geometry's 163-167 gap).
        dragTo(dev, sim, 10, 700, 1100, 400);

        const fb = dev.fbBytes();
        const gapResidue = findGapResidue(fb);
        console.log(`    gap residue pixels=${gapResidue.length}${gapResidue.length > 0 ? " " + JSON.stringify(gapResidue.slice(0, 8)) : ""}`);
        check(
            "a clean drag through the coil leaves ZERO black pixels in the gap between bands",
            gapResidue.length === 0,
            `${gapResidue.length} pixel(s) found${gapResidue.length > 0 ? ", e.g. " + JSON.stringify(gapResidue[0]) : ""}`,
        );
    }

    // ---- scenario D: one lap and two laps must be DISTINGUISHABLE in the
    // framebuffer - the entire point of "two equal-width turns nested
    // inward, separated by a thin outline" rather than one wide band. THE
    // MEASUREMENT CHANGED FROM AN EARLIER READING OF THIS TASK: a first
    // draft would have measured overall painted THICKNESS to tell one lap
    // apart from two, which was correct for a rejected design (the second
    // lap thickening the ring) but is meaningless for what actually
    // shipped, where every turn is the SAME width regardless of lap count -
    // see timer.c's header, "WHAT ACTUALLY GETS BUILT". What genuinely
    // differs is the RADIAL PROFILE: scanning straight down from the coil's
    // own centre (landscape angle 180deg, 6 o'clock, dx=0 - chosen because
    // neither cap, the fixed start cap at 0deg or a moving tip sitting well
    // clear of 180deg in both scenarios below, reaches it, so every sample
    // here comes from the plain per-row bar painter, not cap rasterisation),
    // a coil that has wound exactly one lap paints ONE black run (band 0's
    // own turn) with band 1's own turn still bare grey track; a coil that
    // has wound well into its second lap paints TWO separate black runs
    // (both turns), with the thin outline between them staying non-black -
    // proving the outline genuinely separates two turns rather than
    // rounding noise blurring one wide band into looking like two. -------
    {
        // (RING_CX, RING_CY + r) is exactly angleDeg=180 under timer.c's own
        // phi_deg_at() convention (0 at 12 o'clock, clockwise): sin(180deg)
        // is 0 and cos(180deg) is -1, so this reduces to the plain vertical
        // line, mirrored generally here in case a future pass wants a
        // different angle.
        function radialProfile(fb: Uint8Array, angleDeg: number, rFrom: number, rTo: number): { r: number; black: boolean }[] {
            const theta = angleDeg * DEG2RAD;
            const out: { r: number; black: boolean }[] = [];
            for (let r = rFrom; r <= rTo; r++) {
                const lx = RING_CX + Math.round(r * Math.sin(theta));
                const ly = RING_CY - Math.round(r * Math.cos(theta));
                out.push({ r, black: isBlackAtLand(fb, lx, ly) });
            }
            return out;
        }

        // Contiguous runs of black===true, as [rStart, rEnd] inclusive - the
        // shape a two-turn ring's ink should take at any angle both turns
        // have reached: zero, one or two runs, never a single run spanning
        // the whole 32px (which would mean the outline had vanished).
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

        const outerR = bandOuterR(0);
        const innerR = bandInnerR(LAPS_MAX - 1);
        const SCAN_R_FROM = innerR - 3;
        const SCAN_R_TO = outerR + 3;
        const ANGLE = 180; // straight down - see this scenario's own header

        // ONE LAP: fully wind band 0 (lap 1) and stop comfortably short of
        // band 1 (lap 2) reaching 180deg - any ticks in
        // [TICKS_PER_LAP, TICKS_PER_LAP + TICKS_PER_LAP/2) keeps fillDeg[1]
        // under 180 (see fillDegsForTicks), so the exact commit point
        // (subject to drag hysteresis) does not matter, only that it lands
        // in that generous window.
        const oneLap = await loadDevice();
        oneLap.tick(0);
        oneLap.appSwitch(APP_TIMER);
        oneLap.tick(1000);
        const oneSim = new DragSim();
        dragTo(oneLap, oneSim, 0, 370, 1100, 200); // one lap (360deg) + a small margin
        console.log(`\n-- scenario D, one-lap coil: ${oneSim.ticks} ticks (${(oneSim.ticks * TICK_STEP_S / 60).toFixed(2)} min) --`);
        check(
            "one-lap drag actually wound past one full lap, short of band 1 reaching 180deg",
            oneSim.ticks >= TICKS_PER_LAP && oneSim.ticks < TICKS_PER_LAP + TICKS_PER_LAP / 2,
            `ticks=${oneSim.ticks}`,
        );
        const oneRuns = blackRuns(radialProfile(oneLap.fbBytes(), ANGLE, SCAN_R_FROM, SCAN_R_TO));
        console.log(`    radial profile runs (r=${SCAN_R_FROM}..${SCAN_R_TO} at angle ${ANGLE}deg): ${JSON.stringify(oneRuns)}`);
        check(
            "one lap paints EXACTLY ONE black run at this angle (band 0's own turn only)",
            oneRuns.length === 1,
            `runs=${JSON.stringify(oneRuns)}`,
        );
        if (oneRuns.length === 1) {
            const mid = (oneRuns[0].rStart + oneRuns[0].rEnd) / 2;
            check(
                "and that run sits inside band 0's own radius range (the outer turn), not band 1's",
                mid >= bandInnerR(0) - 2 && mid <= bandOuterR(0) + 2,
                `run midpoint r=${mid}, band0=[${bandInnerR(0)},${bandOuterR(0)}]`,
            );
        }

        // TWO LAPS: wind well into the second lap, comfortably past the
        // point where band 1's own fillDeg reaches 180deg (needs
        // within-lap-2 ticks > TICKS_PER_LAP/2 = 90).
        const twoLap = await loadDevice();
        twoLap.tick(0);
        twoLap.appSwitch(APP_TIMER);
        twoLap.tick(1000);
        const twoSim = new DragSim();
        dragTo(twoLap, twoSim, 0, 600, 1100, 300); // ~1.67 laps
        console.log(`-- scenario D, two-lap coil: ${twoSim.ticks} ticks (${(twoSim.ticks * TICK_STEP_S / 60).toFixed(2)} min) --`);
        check(
            "two-lap drag actually wound past band 1's own 180deg point",
            twoSim.ticks > TICKS_PER_LAP + TICKS_PER_LAP / 2,
            `ticks=${twoSim.ticks}, need > ${TICKS_PER_LAP + TICKS_PER_LAP / 2}`,
        );
        const twoRuns = blackRuns(radialProfile(twoLap.fbBytes(), ANGLE, SCAN_R_FROM, SCAN_R_TO));
        console.log(`    radial profile runs (r=${SCAN_R_FROM}..${SCAN_R_TO} at angle ${ANGLE}deg): ${JSON.stringify(twoRuns)}`);
        check(
            "two laps paint TWO separate black runs at this angle (both turns, outline still visible between them)",
            twoRuns.length === 2,
            `runs=${JSON.stringify(twoRuns)}`,
        );
        if (twoRuns.length === 2) {
            const [inner, outer] = twoRuns[0].rStart < twoRuns[1].rStart ? [twoRuns[0], twoRuns[1]] : [twoRuns[1], twoRuns[0]];
            const innerMid = (inner.rStart + inner.rEnd) / 2;
            const outerMid = (outer.rStart + outer.rEnd) / 2;
            check(
                "the inner run sits in band 1's own range and the outer run in band 0's own range",
                innerMid >= bandInnerR(1) - 2 && innerMid <= bandOuterR(1) + 2 &&
                    outerMid >= bandInnerR(0) - 2 && outerMid <= bandOuterR(0) + 2,
                `inner mid=${innerMid} band1=[${bandInnerR(1)},${bandOuterR(1)}], outer mid=${outerMid} band0=[${bandInnerR(0)},${bandOuterR(0)}]`,
            );
        }

        // Sanity check the same conclusion holds at the cheap whole-frame
        // level too, not just at one sampled angle: a one-lap and a
        // two-lap coil must not hash identically.
        const oneHash = Bun.hash(oneLap.fbBytes());
        const twoHash = Bun.hash(twoLap.fbBytes());
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
