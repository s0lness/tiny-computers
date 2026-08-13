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
// an old edge is stuck for good. This is not really about one big jump: a
// slow, smooth countdown leaves the exact same kind of sliver every single
// frame, just one at a time (see this file's step 3 below) - the reported
// drag only made a lot of them visible in one place at once, because a
// drag revisits many angles quickly on its way to a much smaller value,
// while the fix (padding the row-range's angle window by the cap's own
// angular footprint before turning it into rows) closes the gap in both
// cases the same way.
//
// This loads the REAL firmware compiled to wasm (emulator/wasm/dist/emu.wasm,
// built by emulator/wasm/build.ts) and drives it through emu_tick() with a
// synthetic clock, same shape as the other repro-*.ts tests. No internals
// are touched directly: every assertion is on the framebuffer itself,
// scanned for stray black pixels - a person at the device could see the
// exact same thing by looking at the ring.
//
// The test:
//   1. drags the ring up to a big value with a REALISTIC multi-sample drag
//      (60 touch samples over ~1s, not one instantaneous jump - a real
//      finger reports many points on its way around, and this is also what
//      the owner's report actually was: a drag, not a single touch).
//   2. immediately drags it back down to a small value, again with many
//      samples (the "ended up at 08:00" half of the gesture).
//   3. separately, starts the timer RUNNING from a big value and lets a
//      smooth, fine-grained countdown run for 90 simulated seconds - the
//      "leaves one sliver per frame" shape of the same bug, with no drag
//      involved at all.
//   4. scans the ring's annulus band in the framebuffer and asserts there
//      is no dense cluster of black pixels outside the arc's current
//      angular span (with a small margin for the caps' own intentional
//      rounded bulge).
//
// One note on the assertion's tolerance: a SEPARATE, much smaller defect
// exists in the cap rasterisation (independent lroundf roundings between
// the cap's own half-width table and the ring's own outer-radius table can
// disagree by a single pixel at the cap's edge, regardless of drag
// direction - confirmed present even in a flawless, monotonically
// INCREASING sweep, so it is not the reported bug and this change does not
// touch it). That leaves a thin, roughly uniform scatter of one or two
// stray pixels per 5-degree bucket across the whole ring on every run,
// fixed or not. The assertion below is written to catch the reported bug
// specifically - a DENSE cluster in one region - by bounding the single
// worst 5-degree bucket, not by demanding zero stray pixels anywhere: the
// worst bucket measured before this fix was 42 pixels; after, 2; the
// pre-existing scatter this is deliberately not chasing tops out at 2.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

const PANEL_W = 368;
const PANEL_H = 448;
const APP_TIMER = 2; // g_apps[] = { chrono, sketch("draw"), timer }
const BTN_PWR = 1;

// timer.c's ring geometry, landscape coordinates - lifted, not re-derived
// (RING_CX/RING_CY/RING_HALF_THICK).
const RING_CX = 224, RING_CY = 184;
const DEG2RAD = Math.PI / 180;

// Generous angular tolerance for the caps' own intentional rounded bulge
// past the arc's exact edge (see timer.c's CAP_SWEEP_MARGIN_DEG derivation,
// ~2.78deg): rounded up further here since this is a black-box pixel scan,
// not the firmware's own math.
const CAP_BULGE_TOLERANCE_DEG = 6;

// The worst single 5-degree bucket's stray-pixel count that the PRE-
// EXISTING, direction-independent cap/ring rounding noise alone can
// produce (measured: 2). The reported bug's worst bucket measured 42
// before the fix. This threshold sits well above the noise floor and well
// below the bug, so it fails on the bug and passes on the noise.
const MAX_STRAY_PER_BUCKET = 10;

// timer.c's tiered tick table (see that file's TICK_COUNT comment, "the new
// step table", 2026-08-13): 5s/step up to 2:00, 30s/step up to 10:00,
// 1m/step up to 60:00. Mirrored here, not imported, since this test drives
// the compiled wasm as a black box; kept in lockstep with timer.c's own
// FINE_TICKS/MID_TICKS/COARSE_TICKS by hand.
const FINE_TICKS = 24, FINE_STEP_S = 5, FINE_SPAN_S = FINE_TICKS * FINE_STEP_S;           // 120
const MID_TICKS = 16, MID_STEP_S = 30, MID_SPAN_S = MID_TICKS * MID_STEP_S;               // 480
const COARSE_TICKS = 50, COARSE_STEP_S = 60, COARSE_SPAN_S = COARSE_TICKS * COARSE_STEP_S; // 3000
const TIMER_TICK_COUNT = FINE_TICKS + MID_TICKS + COARSE_TICKS;                            // 90
const TIMER_MAX_SECONDS = FINE_SPAN_S + MID_SPAN_S + COARSE_SPAN_S;                        // 3600

// Mirror of timer.c's tick_index_for_seconds(): the continuous inverse of
// seconds_for_ticks() on the same three-tier scale. Since 2026-08-13
// timer.c's ring angle is driven by TICK INDEX, not by remaining seconds
// directly (a plain remainSec/TIMER_MAX_SECONDS*360 would visibly disagree
// with where a drag lands, see current_fill_deg()'s header comment in
// timer.c) - so this, not a flat proportion, is what this test must use to
// predict where the arc should be after N seconds of RUNNING countdown.
function tickIndexForSeconds(sec: number): number {
    if (sec <= 0) return 0;
    if (sec > TIMER_MAX_SECONDS) sec = TIMER_MAX_SECONDS;
    if (sec <= FINE_SPAN_S) return sec / FINE_STEP_S;
    if (sec <= FINE_SPAN_S + MID_SPAN_S) return FINE_TICKS + (sec - FINE_SPAN_S) / MID_STEP_S;
    return FINE_TICKS + MID_TICKS + (sec - FINE_SPAN_S - MID_SPAN_S) / COARSE_STEP_S;
}

function fillDegForRemainingSeconds(sec: number): number {
    return (tickIndexForSeconds(sec) / TIMER_TICK_COUNT) * 360;
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
        // Every "[fw] ..." line printed so far, in order - used below to read
        // back the exact MM:SS timer.c's own printf chose for a touch, rather
        // than re-deriving it from a hand-rolled copy of ring_tick_for_touch's
        // rounding path (px/py get rounded twice on the way in, see
        // panelTouchForAngle, so a second independent derivation could easily
        // land one tick off from the real one).
        fwLogLines(): readonly string[] {
            return fwLogLines;
        },
    };
}

type Device = Awaited<ReturnType<typeof loadDevice>>;

// panel (px,py) that timer.c's ring_tick_for_touch() will read back as
// landscape angle `deg` (0 at 12 o'clock, clockwise), at radius R landscape
// units out from the ring's centre - inverting gfx_land_rect's mapping the
// same way repro-arena-not-zeroed.ts's RING_TOUCH_PX/PY constants do, just
// parameterised over the angle instead of fixed at one point.
function panelTouchForAngle(deg: number, R = 150): [number, number] {
    const theta = deg * DEG2RAD;
    const lx = RING_CX + R * Math.sin(theta);
    const ly = RING_CY - R * Math.cos(theta);
    const px = PANEL_W - 1 - ly;
    const py = lx;
    return [Math.round(px), Math.round(py)];
}

// True if the framebuffer holds a pure-black pixel (raw 0x0000, the same
// bytes regardless of the panel's byte-swapped RGB565 order - PX_BLACK is a
// byte-palindrome, see gfx.h) at landscape (lx, ly).
function isBlackAtLand(fb: Uint8Array, lx: number, ly: number): boolean {
    const px = PANEL_W - 1 - ly;
    const py = lx;
    const idx = (py * PANEL_W + px) * 2;
    return fb[idx] === 0 && fb[idx + 1] === 0;
}

// Scans the ring's annulus band (radius 155..175, comfortably straddling
// timer.c's RING_INNER_R/RING_OUTER_R of 157/173) and returns every black
// pixel whose angle (clockwise from 12 o'clock, same convention as
// timer.c's fillDeg) falls outside [0, fillDeg] plus the cap bulge
// tolerance on both the moving tip and the fixed start cap - i.e. every
// black pixel that should not be there.
function findStrayBlackPixels(fb: Uint8Array, fillDeg: number): { lx: number; ly: number; deg: number }[] {
    const R_OUTER = 173;
    const stray: { lx: number; ly: number; deg: number }[] = [];
    for (let ly = RING_CY - R_OUTER; ly <= RING_CY + R_OUTER; ly++) {
        for (let lx = RING_CX - R_OUTER; lx <= RING_CX + R_OUTER; lx++) {
            const dx = lx - RING_CX, dy = ly - RING_CY;
            const r = Math.sqrt(dx * dx + dy * dy);
            if (r < 155 || r > 175) continue;
            if (!isBlackAtLand(fb, lx, ly)) continue;
            let deg = Math.atan2(dx, -dy) * (180 / Math.PI);
            if (deg < 0) deg += 360;
            const withinMovingArc = deg <= fillDeg + CAP_BULGE_TOLERANCE_DEG;
            const withinFixedStartCap = deg >= 360 - CAP_BULGE_TOLERANCE_DEG;
            if (!withinMovingArc && !withinFixedStartCap) stray.push({ lx, ly, deg });
        }
    }
    return stray;
}

// Worst single 5-degree bucket among the stray pixels - the metric this
// test actually gates on (see header comment on why a dense cluster, not a
// bare non-zero count, is what distinguishes the reported bug from the
// pre-existing rounding noise).
function worstBucket(stray: { deg: number }[]): { bucket: number; count: number } {
    const buckets = new Map<number, number>();
    for (const s of stray) {
        const b = Math.floor(s.deg / 5) * 5;
        buckets.set(b, (buckets.get(b) ?? 0) + 1);
    }
    let worst = { bucket: -1, count: 0 };
    for (const [b, c] of buckets) if (c > worst.count) worst = { bucket: b, count: c };
    return worst;
}

// Drags the ring's touch point smoothly from fromDeg to toDeg over `steps`
// samples at a 16ms frame interval (~60Hz, a realistic touch sample rate),
// then lifts. Returns the clock time after the lift.
function dragTo(dev: Device, fromDeg: number, toDeg: number, t0: number, steps: number): number {
    let t = t0;
    for (let i = 1; i <= steps; i++) {
        const deg = fromDeg + (toDeg - fromDeg) * (i / steps);
        const [px, py] = panelTouchForAngle(deg);
        dev.touch(true, px, py);
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
    console.log("=== reproduction + regression: ring track residue after the arc shrinks ===\n");

    // ---- scenario A: the reported gesture - drag up big, then a second
    // drag down small, both with many realistic touch samples. ------------
    {
        const dev = await loadDevice();
        dev.tick(0);
        dev.appSwitch(APP_TIMER);
        dev.tick(1000);
        check("switched into timer", dev.appCurrent() === APP_TIMER, `app_current()=${dev.appCurrent()}`);

        console.log("-- drag 1: 0 -> ~300deg over 60 samples (a real drag, not one touch) --");
        let t = dragTo(dev, 0, 300, 1000, 60);
        console.log("-- drag 2: a fresh touch-down, ~300 -> 48deg over 20 samples --");
        t = dragTo(dev, 300, 48, t + 200, 20);

        const fb = dev.fbBytes();
        const stray = findStrayBlackPixels(fb, 48);
        const worst = worstBucket(stray);
        console.log(`    total stray pixels=${stray.length}, worst 5deg bucket=${worst.bucket >= 0 ? worst.bucket + "deg" : "n/a"} count=${worst.count}`);
        check(
            "no dense cluster of black residue in the track after drag-up-then-down",
            worst.count <= MAX_STRAY_PER_BUCKET,
            `worst bucket count=${worst.count}, threshold=${MAX_STRAY_PER_BUCKET}, total stray=${stray.length}`,
        );
    }

    // ---- scenario B: no drag at all - a smooth RUNNING countdown leaves
    // the same kind of sliver, one per frame, without ever jumping. --------
    {
        const dev = await loadDevice();
        dev.tick(0);
        dev.appSwitch(APP_TIMER);
        dev.tick(1000);

        console.log("\n-- drag to a big value, start RUNNING, let 90s of countdown pass --");
        const [px, py] = panelTouchForAngle(300);
        dev.touch(true, px, py);
        dev.tick(1100);
        dev.touch(false, 0, 0);
        dev.tick(1150);
        pwrShortClick(dev, 1200);

        // Read back the exact value timer.c's own "timer: start, MM:SS" print
        // chose for this touch, rather than re-deriving the tick from the
        // touch angle a second time (see fwLogLines()'s comment on why that
        // would risk a one-tick rounding mismatch of its own).
        const startLine = dev.fwLogLines().find((l) => l.startsWith("timer: start, "));
        if (!startLine) throw new Error("did not see a 'timer: start, MM:SS' log line");
        const m = /timer: start, (\d+):(\d+)/.exec(startLine);
        if (!m) throw new Error(`could not parse start time from: ${startLine}`);
        const startSeconds = Number(m[1]) * 60 + Number(m[2]);
        console.log(`    (parsed start = ${startSeconds}s from "${startLine}")`);

        let t = 1300;
        const endT = t + 90_000;
        while (t < endT) {
            t += 16;
            dev.tick(t);
        }

        // fillDeg after 90s off the real start value above, on timer.c's
        // tiered tick-index scale (see fillDegForRemainingSeconds - NOT a
        // flat remainSec/3600*360 any more, since 2026-08-13).
        const currentFillDeg = fillDegForRemainingSeconds(startSeconds - 90);

        const fb = dev.fbBytes();
        const stray = findStrayBlackPixels(fb, currentFillDeg);
        const worst = worstBucket(stray);
        console.log(`    total stray pixels=${stray.length}, worst 5deg bucket=${worst.bucket >= 0 ? worst.bucket + "deg" : "n/a"} count=${worst.count}`);
        check(
            "no dense cluster of black residue in the track after a smooth RUNNING countdown",
            worst.count <= MAX_STRAY_PER_BUCKET,
            `worst bucket count=${worst.count}, threshold=${MAX_STRAY_PER_BUCKET}, total stray=${stray.length}`,
        );
    }

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
