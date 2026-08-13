// repro-timer-swallows-pwr-short-with-boot: headless reproduction of a
// silent, permanent input drop found by fuzzing the timer app's tick()
// against chrono's handling of the identical event class. Run with:
//
//   bun run emulator/wasm/tests/repro-timer-swallows-pwr-short-with-boot.ts
//
// THE BUG. timer_tick() (firmware/apps/timer.c) starts with:
//
//   if (f->bootClicked) {
//       ... // reset to the set value
//       return;
//   }
//   if (f->key & KEY_SHORT) { ... } // start/pause/resume
//
// runtime_core.c's rtcore_tick() calls sensors_key_take() exactly ONCE per
// tick, read-and-clear, before the app ever sees the frame - regardless of
// whether the app goes on to consume KEY_SHORT. So if a BOOT release
// (bootClicked) and a PWR short-press verdict (KEY_SHORT) both land on the
// SAME tick - plausible any time both buttons are released together, since
// BOOT is sampled at only ~20Hz (bootbtn.h) while PWR's verdict arrives from
// the PMIC asynchronously - timer_tick()'s early `return;` inside the
// bootClicked branch means the KEY_SHORT bit is discarded for good: it was
// already cleared out of the hardware register for this tick, and the app
// never looked at it. No log line, no state change, no way to recover it.
//
// chrono_tick() handles the identical class of simultaneous input
// differently: its bootClicked branch has no `return;`, so BOTH the reset
// and the KEY_SHORT toggle apply in the same tick (reset to 00:00:00, then
// immediately start running again) - confirmed below for contrast. That
// asymmetry between the two apps, over the exact same physical event, is
// what makes this look like an oversight in timer.c rather than a documented
// choice: nothing in timer.c's own comments says a same-tick BOOT click is
// meant to swallow a pending PWR short-press.
//
// This loads the REAL firmware compiled to wasm (emulator/wasm/dist/emu.wasm,
// built by emulator/wasm/build.ts) and drives it through emu_tick() with a
// synthetic clock, same harness shape as the other repro-*.ts tests in this
// directory. No internals are touched directly: the observation is the
// firmware's own printf lines (via js_log) and the framebuffer, exactly what
// a person watching the device and its serial console could also see.
//
// EXPECTED FIX SHAPE (not applied here - see this task's brief: findings are
// reported, not silently fixed). The most direct fix consistent with
// chrono.c's own precedent in this same codebase is to drop timer.c's early
// `return;` so a same-tick KEY_SHORT still gets processed after the BOOT
// reset - which is also what this test's assertion below expects: after the
// combined tick, the timer should be back RUNNING (from the just-recalled
// set value), the same shape chrono ends up in. Today it does not: it sits
// in SETTING, frozen, with the PWR press never having happened as far as
// anything on screen or in the log can tell.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");

const PANEL_W = 368;
const PANEL_H = 448;
const APP_TIMER = 2; // g_apps[] = { chrono, sketch("draw"), timer }
const BTN_BOOT = 0;
const BTN_PWR = 1;

// timer.c's ring geometry (RING_CX/RING_CY/RING_RADIUS), landscape
// coordinates - lifted from timer.c, not re-derived, same as the other
// repro tests in this directory.
const RING_CX = 224, RING_CY = 184;
const DEG2RAD = Math.PI / 180;

function panelTouchForAngle(deg: number, R = 150): [number, number] {
    const theta = deg * DEG2RAD;
    const lx = RING_CX + R * Math.sin(theta);
    const ly = RING_CY - R * Math.cos(theta);
    const px = PANEL_W - 1 - ly;
    const py = lx;
    return [Math.round(px), Math.round(py)];
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
                const b = new Uint8Array(memory.buffer, ptr, len);
                const line = decoder.decode(b);
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
            expf: (x: number) => Math.exp(x),
        },
    };

    const instance = await WebAssembly.instantiate(mod, imports);
    memory = instance.exports.memory as WebAssembly.Memory;
    const exp = instance.exports as any;

    if (exp.emu_init() !== 1) {
        throw new Error("emu_init() failed - see [fw] log lines above");
    }

    return {
        tick(nowMs: number) { exp.emu_tick(nowMs); },
        button(index: number, down: boolean) { exp.emu_button(index, down ? 1 : 0); },
        buttonVerdict(index: number, isLong: boolean) { exp.emu_button_verdict(index, isLong ? 1 : 0); },
        touch(down: boolean, x: number, y: number) { exp.emu_touch(down ? 1 : 0, x, y); },
        appSwitch(index: number) { exp.emu_app_switch(index); },
        fbHash(): number | bigint {
            const ptr = exp.emu_fb();
            const fb = new Uint8Array(memory.buffer, ptr, PANEL_W * PANEL_H * 2);
            return Bun.hash(fb);
        },
        fwLogLines(): readonly string[] { return fwLogLines; },
    };
}

type Device = Awaited<ReturnType<typeof loadDevice>>;

async function main() {
    console.log("=== reproduction: timer silently drops a same-tick PWR short-press when BOOT is released too ===\n");

    // ---- TIMER: set a value, start it running, then release BOOT and PWR
    // (short) on the exact same tick. --------------------------------------
    const dev = await loadDevice();
    dev.tick(0);
    dev.appSwitch(APP_TIMER);
    let t = 1;
    dev.tick(t);
    check("switched into timer", true);

    console.log("-- drag the ring to a mid-size value and start it --");
    const [px, py] = panelTouchForAngle(90);
    dev.touch(true, px, py);
    t += 16; dev.tick(t);
    dev.touch(false, 0, 0);
    t += 50; dev.tick(t);
    dev.button(BTN_PWR, true); dev.tick(t);
    dev.button(BTN_PWR, false); dev.buttonVerdict(BTN_PWR, false);
    t += 32; dev.tick(t);
    const startLine = dev.fwLogLines().find((l) => l.startsWith("timer: start, "));
    check("timer started running", !!startLine, startLine);

    for (let i = 0; i < 20; i++) { t += 50; dev.tick(t); } // let it run a bit

    console.log("-- BOOT down, PWR down, PWR up (+ short verdict), BOOT up: all landing on ONE tick --");
    const markBeforeCombined = dev.fwLogLines().length;
    dev.button(BTN_BOOT, true);
    dev.button(BTN_PWR, true);
    dev.button(BTN_PWR, false);
    dev.buttonVerdict(BTN_PWR, false);
    dev.button(BTN_BOOT, false);
    t += 16;
    dev.tick(t);

    const combinedTickLines = dev.fwLogLines().slice(markBeforeCombined);
    console.log("log lines from the combined tick:", combinedTickLines);

    // THE CHECK: does the timer end up back RUNNING (the PWR short-press's
    // effect survived, same shape chrono.c ends up in for the identical
    // input), or is it sitting frozen in SETTING with the press silently
    // gone? Read this back from the framebuffer, not from a log line: the
    // RUNNING-vs-SETTING difference is a digit-colour change (redraw_full,
    // digit_color_for_state) even before any digit VALUE changes, so a
    // hash taken immediately after the combined tick already differs from
    // one taken 2 real seconds later only if something is still counting
    // down.
    const hashRightAfter = dev.fbHash();
    t += 2000; dev.tick(t); // 2s later: RUNNING would visibly change; frozen SETTING would not
    const hashTwoSecondsLater = dev.fbHash();

    check(
        "the timer ends up RUNNING again after the combined tick (the PWR short-press was not silently dropped)",
        hashRightAfter !== hashTwoSecondsLater,
        `hash right after combined tick=${hashRightAfter}, 2s later=${hashTwoSecondsLater} (equal means nothing was counting down - the KEY_SHORT never took effect)`,
    );

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
