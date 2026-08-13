// repro-arena-not-zeroed: headless reproduction of the bug found while
// testing the timer app: leaving it and coming back showed the PREVIOUS
// paused countdown instead of a fresh SETTING screen. Run with:
//
//   bun run emulator/wasm/tests/repro-arena-not-zeroed.ts
//
// Root cause (see runtime_core.c's app_alloc()): do_switch() rewound the
// arena's bump pointer (g_arenaUsed = 0) on every switch but never cleared a
// byte. app.h documents app_alloc() as "Allocates from the arena, zeroed",
// and chrono.c, timer.c and sketch.c all lean on that in their enter()
// comments ("APP_STATE zeroes", "lastDigit[i] is already 0"). So the bytes
// an app's APP_STATE() got back were really whatever the previous
// occupant(s) of that arena offset left behind, not a fresh zeroed struct.
//
// This loads the REAL firmware compiled to wasm (emulator/wasm/dist/emu.wasm,
// built by emulator/wasm/build.ts) and drives it through emu_tick() with a
// synthetic clock, same shape as repro-switch-input.ts. No internals are
// touched directly and no pointer is ever peeked: every assertion is on what
// a person at the device could also observe - emu_app_current() and the
// framebuffer, hashed with Bun.hash() rather than inspected pixel by pixel.
//
// The test:
//   1. switches into the timer for the first time (its state is guaranteed
//      zero here regardless of the bug: it is the module's BSS, untouched by
//      anything before it) and records that render as the "fresh" hash;
//   2. dirties the timer thoroughly (drags the ring to set a time, starts
//      it, lets it run down a few seconds, pauses it - state, setTicks,
//      remainingSeconds, lastLit and lastDigitSeconds all now differ from a
//      fresh SETTING screen);
//   3. switches to chrono and dirties IT too (starts the stopwatch and lets
//      it run), so the arena's first bytes are overwritten by a
//      differently-shaped struct before the timer ever gets them back -
//      exactly the "worse and more confusing between apps of different
//      shapes" case the bug report called out;
//   4. switches back to the timer and compares its render to the "fresh"
//      hash from step 1.
//
// Before the fix: the step-4 render carries the paused timer's leftover
// digits/ring (or, worse, a mix of stale bytes if chrono's differently-sized
// state didn't fully overwrite them) instead of a blank SETTING screen, so
// the hashes differ and the test FAILS. After the fix: app_alloc() zeroes
// what it hands out, so entering the timer is byte-for-byte the same render
// whether it is the first time or the fifth, and the test PASSES.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

// Panel geometry and app-table facts, lifted from gfx.h / runtime_core.c /
// timer.c rather than re-derived: this test drives the real module, it does
// not reimplement it.
const PANEL_W = 368;
const PANEL_H = 448;
const APP_CHRONO = 0;
const APP_TIMER = 2; // g_apps[] = { chrono, sketch("draw"), timer }
const BTN_PWR = 1;

// timer.c's ring geometry (RING_CX/RING_CY/RING_RADIUS), landscape
// coordinates. A point straight out to the ring's right (3 o'clock) is
// enough to make ring_tick_for_touch() choose a non-zero tick count; the
// exact count does not matter to this test, only that SOMETHING gets set.
const RING_CX = 224, RING_CY = 184, RING_RADIUS = 165;
const RING_TOUCH_LX = RING_CX + RING_RADIUS; // 389
const RING_TOUCH_LY = RING_CY;               // 184

// timer.c's ring_tick_for_touch() inverts panel_to_land: lx = touchPanelY,
// ly = PANEL_W - 1 - touchPanelX. Solved here for the panel point that maps
// to the landscape point above: touchPanelY = lx, touchPanelX = PANEL_W-1-ly.
const RING_TOUCH_PX = PANEL_W - 1 - RING_TOUCH_LY; // 183
const RING_TOUCH_PY = RING_TOUCH_LX;               // 389

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

    const imports = {
        env: {
            js_log(ptr: number, len: number) {
                const bytes = new Uint8Array(memory.buffer, ptr, len);
                console.log(`    [fw] ${decoder.decode(bytes)}`);
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
        button(index: number, down: boolean) {
            exp.emu_button(index, down ? 1 : 0);
        },
        buttonVerdict(index: number, isLong: boolean) {
            exp.emu_button_verdict(index, isLong ? 1 : 0);
        },
        touch(down: boolean, x: number, y: number) {
            exp.emu_touch(down ? 1 : 0, x, y);
        },
        appSwitch(index: number) {
            exp.emu_app_switch(index);
        },
        appCurrent(): number {
            return exp.emu_app_current();
        },
        fbHash(): number | bigint {
            const ptr = exp.emu_fb();
            const fb = new Uint8Array(memory.buffer, ptr, PANEL_W * PANEL_H * 2);
            return Bun.hash(fb);
        },
    };
}

type Device = Awaited<ReturnType<typeof loadDevice>>;

// A plain PWR short click: press, release, short verdict - each edge its own
// tick, so a single sensors_key_take() read never sees more than one bit.
function pwrShortClick(dev: Device, tPressMs: number) {
    dev.button(BTN_PWR, true);
    dev.tick(tPressMs);
    dev.button(BTN_PWR, false);
    dev.buttonVerdict(BTN_PWR, false);
    dev.tick(tPressMs + 50);
}

async function main() {
    console.log("=== reproduction + regression: arena not zeroed on app switch ===\n");

    const dev = await loadDevice();

    console.log("-- boot --");
    dev.tick(0);
    check("boots into chrono", dev.appCurrent() === APP_CHRONO, `app_current()=${dev.appCurrent()}`);

    // ---- first-ever entry into the timer: guaranteed fresh (module BSS is
    // zero at load, regardless of the bug), so this is the "what a clean
    // SETTING screen actually looks like" reference. ------------------------
    console.log("-- first switch into timer (the reference 'fresh' render) --");
    dev.appSwitch(APP_TIMER);
    dev.tick(1000);
    check("switched into timer", dev.appCurrent() === APP_TIMER, `app_current()=${dev.appCurrent()}`);
    const freshHash = dev.fbHash();
    console.log(`    fresh timer render hash = ${freshHash}`);

    // ---- dirty the timer thoroughly: set a time, start it, let it run down
    // a few seconds, pause it. State, setTicks, remainingSeconds, lastLit and
    // lastDigitSeconds all now differ from the fresh SETTING screen above. --
    console.log(`-- dirty timer: drag ring at panel (${RING_TOUCH_PX}, ${RING_TOUCH_PY}) --`);
    dev.touch(true, RING_TOUCH_PX, RING_TOUCH_PY);
    dev.tick(1100);
    dev.touch(false, 0, 0);
    dev.tick(1150);
    console.log("-- start it, let it run, pause it --");
    pwrShortClick(dev, 1500); // SETTING -> RUNNING
    dev.tick(3600);           // a couple of seconds of countdown pass
    pwrShortClick(dev, 3700); // RUNNING -> PAUSED
    const dirtyHash = dev.fbHash();
    check(
        "dirtying the timer actually changed its render",
        dirtyHash !== freshHash,
        `dirty=${dirtyHash} fresh=${freshHash}`,
    );

    // ---- switch to chrono and dirty IT too, so the arena's first bytes get
    // overwritten by a differently-shaped struct before the timer ever gets
    // them back - the "worse between apps of different shapes" case. -------
    console.log("-- switch to chrono and dirty it too (start the stopwatch) --");
    dev.appSwitch(APP_CHRONO);
    dev.tick(4000);
    check("switched to chrono", dev.appCurrent() === APP_CHRONO, `app_current()=${dev.appCurrent()}`);
    pwrShortClick(dev, 4100); // start the stopwatch
    dev.tick(5200);           // let a second-plus of elapsed time show

    // ---- back to the timer: THE regression check. -------------------------
    console.log("-- switch back to timer --");
    dev.appSwitch(APP_TIMER);
    dev.tick(6000);
    check("switched back into timer", dev.appCurrent() === APP_TIMER, `app_current()=${dev.appCurrent()}`);
    const reentryHash = dev.fbHash();
    console.log(`    reentry timer render hash = ${reentryHash}`);
    check(
        "timer re-entry renders identically to a fresh SETTING screen (app_alloc() actually zeroes)",
        reentryHash === freshHash,
        `reentry=${reentryHash} fresh=${freshHash}`,
    );

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
