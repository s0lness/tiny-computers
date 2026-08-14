// repro-switch-input: headless reproduction of the hardware bug reported by
// the owner (see runtime_core.c's touch_resolver_reset() and its comment for
// the fix, and the "BOOT+PWR long-press chord" comment for the gesture this
// drives). Run with:
//
//   bun run emulator/wasm/tests/repro-switch-input.ts
//
// This loads the REAL firmware compiled to wasm (emulator/wasm/dist/emu.wasm,
// built by emulator/wasm/build.ts) and drives it through emu_tick() with a
// synthetic clock, never wall time - see emu_abi.h's comment on why nowMs is
// the only time source a reproducible test can trust.
//
// It performs the actual sequence the owner described: boot, open the menu
// through the real gesture (BOOT held, PWR held past its long-press
// threshold - not a shortcut into app_switch_to()), tap a tile, and exercise
// whatever app that lands on. No internals are touched directly; every
// assertion is on what a person at the device could also observe: the
// framebuffer changing (hashed with Bun.hash(), not inspected pixel by
// pixel) and emu_app_current().
//
// History: this file originally drove the long-double-press gesture the
// owner actually hit on hardware (a short PWR press, then a second PWR
// press held long, within 500ms of the first) and that version is what
// first reproduced the bug below - see this file's git history for the
// exact trace and PASS/FAIL output before and after the fix. The gesture
// itself was replaced (both buttons held, PWR long) once the double-press's
// own timing window turned out to be a second, independent bug (see
// runtime_core.c); this version drives the chord that replaced it.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

// Panel geometry and app-table facts, lifted from gfx.h / runtime_core.c /
// emu_shim.c rather than re-derived: this test drives the real module, it
// does not reimplement it.
const PANEL_W = 368;
const PANEL_H = 448;
const APP_INDEX_MENU = -1;
const APP_CHRONO = 0;
const APP_DRAW = 1;
const BTN_BOOT = 0;
const BTN_PWR = 1;
const KEY_LONG_MS = 1500; // emu_device()'s declared longPressMs for PWR

// menu.c's ACTUAL touch layout (menu_rows/menu_row_span/cell_rect_land): a
// GRID of cells, MENU_CELL_H=112 tall, at most MENU_COLS_MAX=4 across, packed
// to the top of the glass, with each row's cells contiguous and its last one
// absorbing whatever LAND_W does not divide evenly - so every landscape point
// above the grid's own bottom edge belongs to exactly one cell, and
// everything below it is the cancel band. Recomputed here (there is no export
// for it) so a touch can be aimed at a specific app, the same way a finger
// would be aimed at a spot on the glass.
//
// THIS FILE HAS NOW MODELLED A DEAD LAYOUT TWICE, which is worth the space.
//
// The first time (TILE_MARGIN=16, TILE_GAP=16, TILE_H=220, bordered tiles
// centred vertically) it kept passing after menu.c had moved on, because with
// three apps the centre of an imaginary bordered tile still landed inside the
// real full-width column. The fourth app narrowed the columns from 149px to
// 112px and the stale model started aiming into the NEIGHBOURING column: a
// latent wrong test that a layout change exposed, not a regression in menu.c.
//
// The second time was 2026-08-15, when the columns became this grid because a
// sixth app would have taken each column under a child's fingertip
// (docs/decisions/0013). The full-height-column model would have kept passing
// here too, for exactly the same reason as the first time: it aimed at
// ly=LAND_H/2=184, which at four apps in one row of 112 is now in the CANCEL
// BAND, so every tap in this file would have become a cancel and the test
// would have failed loudly rather than quietly. That is luck, not design. So:
// rewritten from menu.c's current geometry, and aimed at a cell CENTRE, which
// is the one point that stays correct under any future reshuffle of the same
// idea.
//
// appCount is read from emu_device()'s own "apps" array rather than written
// down here, so the next app to be added cannot re-stale it.
function cellCenterPanelXY(appIndex: number, appCount: number): [number, number] {
    const LAND_W = PANEL_H, LAND_H = PANEL_W;
    const MENU_CELL_H = 112;
    const MENU_COLS_MAX = 4;
    const rowsMax = Math.floor(LAND_H / MENU_CELL_H);
    const rows = Math.min(Math.max(Math.ceil(appCount / MENU_COLS_MAX), 1), rowsMax);
    const base = Math.floor(appCount / rows);
    const extra = appCount % rows;

    let first = 0, cols = 0, r = 0;
    for (r = 0; r < rows; r++) {
        first = r * base + Math.min(r, extra);
        cols = base + (r < extra ? 1 : 0);
        if (appIndex < first + cols) break;
    }
    const c = appIndex - first;
    const w = Math.floor(LAND_W / cols);
    const bx = c * w;
    const bw = c === cols - 1 ? LAND_W - bx : w;
    const lx = bx + Math.floor(bw / 2);
    const ly = r * MENU_CELL_H + Math.floor(MENU_CELL_H / 2);
    // menu.c's panel_to_land() inverted: lx=py, ly=PANEL_W-1-px -> px =
    // PANEL_W-1-ly, py = lx.
    return [PANEL_W - 1 - ly, lx];
}

// THE MENU LAUNCHES ON RELEASE NOW, not on touch-down, so every "tap" in
// this file is a press-hold-release. Rewritten rather than loosened when
// menu.c changed (the same way repro-ring-shrink-residue.ts was rewritten
// when the timer stopped being a dial): the bug this file reproduces is
// about a stale wasDown swallowing the PRESS, and that bug is still
// reproducible - it just takes the gesture the menu actually has.
//
// The hold has to satisfy menu.c's arming rule (MENU_ARM_SAMPLES contact
// samples spanning MENU_ARM_MS at MENU_ARM_RATE_HZ), THEN outlast
// MENU_HOVER_CONFIRM_MS with the same cell reported before that cell is
// actually lit (2026-08-15: a jitter episode on this panel throws the
// reported centroid 80-250px away for up to three reports, which on a grid
// of 112px cells is a whole cell in any direction, so the halo does not move
// until a position has held), and the release has to outlast
// MENU_RELEASE_GRACE_MS of silence before it is believed. All three numbers
// are menu.c's; the arithmetic behind the first and last is in apps/four.c.
const MENU_RELEASE_GRACE_MS = 300;
const MENU_HOVER_CONFIRM_MS = 150;

// Presses at (px, py), holds it there, and returns the clock. Leaves the
// finger DOWN - callers that want a launch call menuRelease(), and the one
// caller that wants the finger to stay down through the switch does not.
function menuHold(dev: Device, px: number, py: number, tMs: number): number {
    let t = tMs;
    // Long enough to arm AND then to confirm the hover: 8 samples used to be
    // enough when arming was the only gate, and is 120ms, which is under
    // MENU_ARM_MS + MENU_HOVER_CONFIRM_MS. Derived rather than counted, so
    // that moving either of menu.c's constants moves this too.
    const HOLD_MS = 60 + MENU_HOVER_CONFIRM_MS + 100;
    for (let held = 0; held < HOLD_MS; held += 15) {
        dev.touch(true, px, py);
        dev.tick(t);
        t += 15;
    }
    return t;
}

function menuRelease(dev: Device, tMs: number): number {
    let t = tMs;
    for (let waited = 0; waited < MENU_RELEASE_GRACE_MS + 150; waited += 15) {
        dev.touch(false, 0, 0);
        dev.tick(t);
        t += 15;
    }
    return t;
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

    const imports = {
        env: {
            js_log(ptr: number, len: number) {
                const bytes = new Uint8Array(memory.buffer, ptr, len);
                console.log(`    [fw] ${decoder.decode(bytes)}`);
            },
            // All eight take/return f32; wasm truncates at the call
            // boundary, JS Math is float64 underneath either way (see
            // emu_abi.h's note on this being a known, accepted numerical
            // divergence from the board's single-precision FPU).
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
        appCurrent(): number {
            return exp.emu_app_current();
        },
        fbHash(): number | bigint {
            const ptr = exp.emu_fb();
            const fb = new Uint8Array(memory.buffer, ptr, PANEL_W * PANEL_H * 2);
            return Bun.hash(fb);
        },
        // How many apps this firmware actually declares, from emu_device()'s
        // own JSON (emu_abi.h's "apps" key) rather than a number written down
        // in this file - see cellCenterPanelXY's comment for what a hardcoded
        // one cost.
        appCount(): number {
            const ptr = exp.emu_device();
            const bytes = new Uint8Array(memory.buffer, ptr);
            let end = 0;
            while (bytes[end] !== 0) end++;
            const json = JSON.parse(decoder.decode(bytes.subarray(0, end)));
            return Array.isArray(json.apps) ? json.apps.length : 0;
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

// The real gesture, per runtime_core.c's "BOOT+PWR long-press chord" comment:
// BOOT held down, PWR pressed and held past the PMIC's 1.5s threshold, both
// still held at the instant the long-press verdict lands. Released in the
// natural order (PWR first, then BOOT) afterward.
function bootPwrChord(dev: Device, label: string, t0: number) {
    console.log(`-- ${label} (BOOT+PWR chord, starting t=${t0}) --`);
    dev.button(BTN_BOOT, true);
    dev.tick(t0);
    dev.button(BTN_PWR, true);
    dev.tick(t0 + 10);
    dev.buttonVerdict(BTN_PWR, true);
    dev.tick(t0 + 10 + KEY_LONG_MS);
    dev.button(BTN_PWR, false);
    dev.tick(t0 + 10 + KEY_LONG_MS + 50);
    dev.button(BTN_BOOT, false);
    dev.tick(t0 + 10 + KEY_LONG_MS + 100);
}

async function main() {
    console.log("=== reproduction + regression: switch-carried touch state, and the BOOT+PWR chord ===\n");

    const dev = await loadDevice();

    console.log("-- boot --");
    dev.tick(0);
    const bootApp = dev.appCurrent();
    check("boots into chrono", bootApp === APP_CHRONO, `app_current()=${bootApp}`);

    // ---- a PWR-only long press must NOT open the menu (the chord requires
    // BOOT too) - sanity check that this really is a chord, not a relabelled
    // single long press.
    console.log("-- PWR long press alone (BOOT not held): must NOT open the menu --");
    dev.button(BTN_PWR, true);
    dev.tick(1000);
    dev.buttonVerdict(BTN_PWR, true);
    dev.tick(1000 + KEY_LONG_MS);
    dev.button(BTN_PWR, false);
    dev.tick(1000 + KEY_LONG_MS + 50);
    const afterPwrAlone = dev.appCurrent();
    check("PWR alone does not open the menu", afterPwrAlone === APP_CHRONO, `app_current()=${afterPwrAlone}`);

    // ---- open the menu, the real way -----------------------------------
    bootPwrChord(dev, "gesture #1: open menu from chrono", 3000);
    const afterOpen1 = dev.appCurrent();
    check("menu opens from chrono", afterOpen1 === APP_INDEX_MENU, `app_current()=${afterOpen1}`);

    // ---- tap the draw tile, launching sketch -----------------------------
    const appCount = dev.appCount(); // from emu_device()'s own "apps" array
    const [drawX, drawY] = cellCenterPanelXY(APP_DRAW, appCount);
    console.log(`-- tap the "draw" cell at panel (${drawX}, ${drawY}) --`);
    let tGest = menuHold(dev, drawX, drawY, 5000);
    check("holding on the draw tile does NOT launch it yet - release is the verb now",
        dev.appCurrent() === APP_INDEX_MENU, `app_current()=${dev.appCurrent()}`);
    tGest = menuRelease(dev, tGest);
    const afterTapDraw = dev.appCurrent();
    check("releasing over the draw tile launches sketch", afterTapDraw === APP_DRAW, `app_current()=${afterTapDraw}`);

    // The tap continues into a drag: the finger never left the glass. This
    // is the exact moment the suspected mechanism targets: does the newly
    // entered app's first tick see a stale "already down" and miss the
    // press edge it needs to start a stroke?
    // A stroke in the app that was just entered. This used to continue the
    // very touch that launched it (the finger never left the glass), which
    // was the sharpest form of the question this file exists to ask: does
    // the newly entered app's first tick see a stale "already down" and miss
    // the press edge it needs? Release-to-launch means the finger is by
    // definition UP at the moment of the switch, so the sharp form is gone -
    // but the question is not, and the answer still has to be yes: a press
    // arriving immediately after a switch must start a stroke.
    const preDrawHash = dev.fbHash();
    console.log("-- press inside the app that was just entered, and drag --");
    let tDraw = tGest;
    for (const [dx, dy] of [[0, 0], [8, 8], [20, 20], [34, 34]] as const) {
        dev.touch(true, drawX + dx, drawY + dy);
        dev.tick(tDraw);
        tDraw += 20;
    }
    dev.touch(false, 0, 0);
    dev.tick(tDraw + 400); // past LIFT_DEBOUNCE_MS, lift registers
    const postDrawHash = dev.fbHash();
    check(
        "sketch actually drew something (framebuffer changed)",
        preDrawHash !== postDrawHash,
        `pre=${preDrawHash} post=${postDrawHash}`,
    );

    // ---- back to the menu, from inside sketch ----------------------------
    bootPwrChord(dev, "gesture #2: open menu from sketch", 6000);
    const afterOpen2 = dev.appCurrent();
    check("menu opens again from sketch", afterOpen2 === APP_INDEX_MENU, `app_current()=${afterOpen2}`);

    // ---- tap the chrono tile -----------------------------------------------
    // THE critical check: this is exactly the tap the suspected mechanism
    // predicts will be swallowed (a stale wasDown carried from the still-down
    // finger that launched sketch, never cleared while sketch - a raw-touch
    // consumer - was running). FAILS before the fix, PASSES after.
    const [chronoX, chronoY] = cellCenterPanelXY(APP_CHRONO, appCount);
    console.log(`-- tap the "chrono" cell at panel (${chronoX}, ${chronoY}) --`);
    const tChrono = menuRelease(dev, menuHold(dev, chronoX, chronoY, 8000));
    (function keepClockMoving() { dev.tick(tChrono); })();
    const afterTapChrono = dev.appCurrent();
    check("tapping the chrono tile launches chrono", afterTapChrono === APP_CHRONO, `app_current()=${afterTapChrono}`);

    // ---- exercise the stopwatch: PWR-short should start it ---------------
    const preRunHash = dev.fbHash();
    console.log("-- PWR short-press: start the stopwatch --");
    pwrShortClick(dev, 8500);
    dev.tick(9550); // let a second of running time pass, so a digit changes
    const postRunHash = dev.fbHash();
    check(
        "chrono responds to PWR-short (framebuffer changed as it runs)",
        preRunHash !== postRunHash,
        `pre=${preRunHash} post=${postRunHash}`,
    );

    // ---- the reported terminal symptom: does the menu still open? --------
    bootPwrChord(dev, "gesture #3: open menu from chrono, again", 10000);
    const afterOpen3 = dev.appCurrent();
    check(
        "menu still opens on a third chord (the reported terminal symptom)",
        afterOpen3 === APP_INDEX_MENU,
        `app_current()=${afterOpen3}`,
    );

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
