// repro-sketch-palette-animation-starved: headless reproduction of the
// owner's reports on real hardware, 2026-08-14, across two rounds of the
// same underlying bug shape.
//
//   THIRD ROUND, the day after the watchdog-reset fix landed and worked
//   ("ça marche !"): "L'animation est super super saccadée. Concretement
//   j'ai qu'une frame intermediaire avant que ça fasse les carrés en gros."
//   One intermediate frame across the whole ~280ms pop-in.
//
//   FOURTH ROUND, after the third round's own fix (palette_advance_
//   animation(), driving the pop-in from wall-clock time every tick) landed
//   and was flashed: "L'animation s'arrete a moitie, puis si je fais passer
//   mon doigt sur un rectangle, le rectangle finit de grossir." The
//   animation freezes partway, and only a discrete touch event (moving onto
//   a new cell) finishes growing that ONE cell - exactly what a render
//   trigger that is still secretly gated on something discrete looks like
//   from outside, even though the underlying state was, in fact, advancing
//   every tick as intended (see sketch.c's own palette_advance_animation()
//   and palette_render_animating() comments for the full account of both
//   causes and both fixes).
//
// WHY THIS FILE'S OWN FRAME-COUNT ASSERTION DID NOT CATCH THE FOURTH ROUND,
// AND WHY IT NOW DOES SOMETHING DIFFERENT. The original version of this
// file only ever counted "palette: frame" LOG LINES and asserted there were
// several. That line is written from inside the render function only once
// whiten+draw actually ran, which sounds like exactly the right thing to
// count - and in the fourth round's actual build, it WAS right: the buggy
// version's shared gate meant almost no lines got logged at all during a
// starved run, so a frame-count-only version of this file would in fact
// have failed against that build too. It is still the wrong thing to build
// a standing regression test around, for a reason independent of whether
// THIS particular bug happened to trip it: a logged line proves the
// firmware BELIEVES it drew something, never that the panel's own pixels
// changed. A future regression with a different shape - a scale calculation
// that silently saturates, a stale nowMs, a candidate that never updates -
// could log a perfectly healthy-looking frame count while drawing the same
// picture over and over, and a line-count assertion would wave it through.
// THE FIX BELOW: snapshot the actual framebuffer (emu_fb(), the same
// mechanism feature-sketch-palette.ts's own residue checks use) around
// every tick, and assert the PIXELS THEMSELVES changed at every logged
// frame and across multiple genuinely distinct visible states - what the
// owner can see on the panel, not what the firmware's own log claims.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/repro-sketch-palette-animation-starved.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const APP_DRAW = 1; // g_apps[] = { chrono, sketch("draw"), timer, four }
const PANEL_W = 368;
const PANEL_H = 448;

const CONFIRM_MS = 40;
const LONG_PRESS_MS = 550;
const PALETTE_POP_MS = 240;
const PALETTE_STAGGER_MS = 20;
const PALETTE_RENDER_MIN_MS = 33;
const PALETTE_ANIM_TOTAL_MS = PALETTE_POP_MS + 2 * PALETTE_STAGGER_MS; // 280

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

function parseFrameLine(line: string): { kind: string; whitenPx: number; coverageEvals: number } | null {
    const m = line.match(/palette: frame kind=(\w+) whitenPx=(\d+) coverageEvals=(\d+)/);
    if (!m) return null;
    return { kind: m[1]!, whitenPx: Number(m[2]), coverageEvals: Number(m[3]) };
}

// Every cell drawn onto white (0xFFFF, little-endian bytes FF FF) darkens
// at least one byte away from that (MIN-composited ink, never lightened -
// decision 0009 / draw_capsule's own header comment), so counting bytes
// that are not 0xFF is a cheap, geometry-free proxy for "how much ink is on
// screen right now" - used below only to prove several genuinely DIFFERENT
// states occurred, not to assert any particular direction or rate of
// growth (the pop-in's own overshoot, PALETTE_POP_PEAK_SCALE, can make a
// cell briefly larger than its final size and then recede a little as it
// settles, which is correct, expected behaviour, not a regression).
function inkPixelCount(fb: Uint8Array): number {
    let count = 0;
    for (let i = 0; i < fb.length; i++) if (fb[i] !== 0xff) count++;
    return count;
}

function fbEqual(a: Uint8Array, b: Uint8Array): boolean {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}

async function main() {
    console.log("=== reproduction + regression: the pop-in animation must not starve without touch samples, and every frame it logs must actually be VISIBLE on the panel ===\n");

    const dev = await loadDevice();
    dev.tick(0);
    dev.appSwitch(APP_DRAW);
    dev.tick(1000);
    dev.drainLog();

    // ---- open the palette the ordinary way: a genuine long press, real
    // touch samples, a realistic ~15ms report cadence. -------------------
    let t = 1000;
    const holdX = 184, holdY = 223; // the centre cell (post gfx.h PANEL_BEZEL_MARGIN_PX inset)
    const HOLD_STEP_MS = 15;
    let opened = false;
    for (let held = 0; held < CONFIRM_MS + LONG_PRESS_MS + 100 && !opened; held += HOLD_STEP_MS) {
        t += HOLD_STEP_MS;
        dev.touch(true, holdX, holdY);
        dev.tick(t);
        if (dev.drainLog().some((l) => l.includes("palette: open"))) opened = true;
    }
    check("the palette opened via a genuine long press", opened);

    // ---- THE scenario: from here, no more touch() calls AT ALL - not one
    // more sample, not even a repeat of the same position - only tick()s,
    // spanning the whole pop-in animation and a margin past it. A real
    // device's own main loop keeps calling tick() regardless of whether
    // core1 queued anything; this is that, with the touch queue left
    // completely empty (see this file's own header comment on why that is
    // the emulator's honest equivalent of "the controller produced nothing
    // new"). Every tick is snapshotted, so every "palette: frame" log line
    // can be checked against what it actually drew, not just that it
    // fired. -----------------------------------------------------------
    const TICK_STEP_MS = 10; // a plausible main-loop cadence, unrelated to any report rate
    const WINDOW_MS = PALETTE_ANIM_TOTAL_MS + 80;
    const frames: { kind: string; whitenPx: number; coverageEvals: number }[] = [];
    const framePixelDeltas: number[] = []; // count of changed bytes, one per logged "frame" line
    const inkTrace: number[] = []; // inkPixelCount() right after each logged frame, in order
    let prevFb = dev.fbSnapshot();
    let framesThatChangedNothing = 0;

    for (let elapsed = 0; elapsed < WINDOW_MS; elapsed += TICK_STEP_MS) {
        t += TICK_STEP_MS;
        dev.tick(t);
        const log = dev.drainLog();
        let loggedFrame = false;
        for (const l of log) {
            const f = parseFrameLine(l);
            if (f) { frames.push(f); loggedFrame = true; }
        }
        if (loggedFrame) {
            const nowFb = dev.fbSnapshot();
            let delta = 0;
            for (let i = 0; i < nowFb.length; i++) if (nowFb[i] !== prevFb[i]) delta++;
            framePixelDeltas.push(delta);
            inkTrace.push(inkPixelCount(nowFb));
            if (delta === 0) framesThatChangedNothing++;
            prevFb = nowFb;
        }
    }

    const ticksInWindow = Math.ceil(WINDOW_MS / TICK_STEP_MS);
    console.log(`    ${ticksInWindow} tick()s over ${WINDOW_MS}ms, ZERO touch samples after the one that opened it -> ${frames.length} "palette: frame" lines`);
    console.log(`    per-frame pixel-byte deltas: [${framePixelDeltas.join(", ")}]`);
    console.log(`    ink byte count at each frame: [${inkTrace.join(", ")}]`);

    // THE THIRD ROUND'S OWN REGRESSION: the report was "one intermediate
    // frame". A minimum comfortably above that, and comfortably below what
    // the throttle alone would allow if every tick rendered (which would
    // itself be the watchdog bug's own shape again), pins the animation to
    // actually animating without pinning it to an exact frame count that a
    // legitimate future retune of PALETTE_POP_MS or PALETTE_RENDER_MIN_MS
    // would then break for no real reason.
    const expectedFrames = Math.ceil(PALETTE_ANIM_TOTAL_MS / PALETTE_RENDER_MIN_MS); // ~9
    const minFrames = 5; // well above the reported "1", well below expectedFrames
    const maxFrames = ticksInWindow; // sanity: can never exceed one render per tick
    check(`the animation actually advances across multiple frames with zero touch samples (>=${minFrames}, ~${expectedFrames} expected)`,
        frames.length >= minFrames, `${frames.length} frames`);
    check(`frame count still stays within what the throttle allows (<=${maxFrames})`,
        frames.length <= maxFrames, `${frames.length} frames over ${ticksInWindow} ticks`);

    // THE FOURTH ROUND'S OWN REGRESSION, the point of this file's rewrite:
    // every single logged "palette: frame" line must correspond to pixels
    // that ACTUALLY CHANGED on the panel. A build with the fourth round's
    // bug (or one shaped like it) could, in principle, still log a
    // healthy-looking frame count while drawing the same frozen picture
    // over and over (the owner's own report, "the animation stops
    // halfway") - this is the assertion that catches that shape of bug even
    // when the log-line count itself still looks normal.
    check("every logged animation frame actually changed pixels on the panel (not a no-op that merely announced itself)",
        framesThatChangedNothing === 0, `${framesThatChangedNothing} of ${framePixelDeltas.length} logged frames changed nothing`);

    // THE OWNER'S OWN ACCEPTANCE CRITERION, in plain words: "the rectangles
    // should arrive the way they leave" - several genuinely different
    // in-between states, not a freeze followed by one jump straight to the
    // finished picture. Proxied here by how many DISTINCT ink levels the
    // animation passes through - deliberately not asserted to be monotonic
    // (the pop-in's own overshoot can make it dip briefly as a cell settles
    // back from past its final size, which is correct behaviour, not a
    // regression), only that there are genuinely several of them.
    const distinctInkLevels = new Set(inkTrace).size;
    check(`the visible picture passes through multiple distinct states, not a freeze-then-jump (>=${minFrames} distinct ink levels expected)`,
        distinctInkLevels >= minFrames, `${distinctInkLevels} distinct ink levels across ${inkTrace.length} frames`);

    // ---- and once settled, continuing to tick with still no touch samples
    // must produce ZERO further frames AND an unchanged framebuffer - "the
    // right number of renders per second is zero, not thirty" once nothing
    // is changing, the same invariant the watchdog fix's own settled-path
    // scoping established, now checked at the pixel level too. ------------
    const IDLE_TICKS = 50;
    let idleFrames = 0;
    const beforeIdle = dev.fbSnapshot();
    for (let i = 0; i < IDLE_TICKS; i++) {
        t += TICK_STEP_MS;
        dev.tick(t);
        for (const l of dev.drainLog()) if (parseFrameLine(l)) idleFrames++;
    }
    const afterIdle = dev.fbSnapshot();
    console.log(`    ${IDLE_TICKS} more idle tick()s, settled, still zero touch samples -> ${idleFrames} more frames`);
    check("once settled, idling with no touch samples produces zero further frames", idleFrames === 0, `${idleFrames} frames`);
    check("once settled, the framebuffer itself is unchanged too (not just unlogged)", fbEqual(beforeIdle, afterIdle));

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
