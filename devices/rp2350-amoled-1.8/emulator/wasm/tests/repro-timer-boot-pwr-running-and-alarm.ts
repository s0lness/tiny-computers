// repro-timer-boot-pwr-running-and-alarm: regression coverage for the two
// states the fix to timer.c's bootClicked-swallows-KEY_SHORT bug (see
// docs/findings-app-fuzzing.md section 1, and the primary reproduction
// repro-timer-swallows-pwr-short-with-boot.ts) was most likely to leave
// incoherent: RUNNING and ALARM. Run with:
//
//   bun run emulator/wasm/tests/repro-timer-boot-pwr-running-and-alarm.ts
//
// THE FIX BEING GUARDED. timer_tick() (firmware/apps/timer.c) used to open
// with `if (f->bootClicked) { ...; return; }`, which discarded a same-tick
// PWR short-press (KEY_SHORT) for good, because runtime_core.c's
// sensors_key_take() is read-and-clear once per tick regardless of whether
// the app consumes what it returns. The fix drops that unconditional
// return, following chrono.c's own precedent for the identical situation:
// BOOT applies first, then a same-tick KEY_SHORT acts on the state BOOT's
// handling just produced.
//
// TWO SCENARIOS HERE, both driving the real combined-tick BOOT+PWR chord
// (BOOT down, PWR down, PWR up + short verdict, BOOT up, then one tick):
//
//   1. RUNNING: BOOT resets a running countdown back to SETTING at the set
//      value (unaffected by the fix - this alone was already correct), and
//      the fix means the pending KEY_SHORT is no longer dropped: it starts
//      the countdown running again from that same value in the SAME tick.
//      This is the scenario the primary repro already exercises end to end
//      (hash-based); this file re-asserts it directly off the firmware's
//      own log lines instead, as a second, independent check.
//
//   2. ALARM ringing: this path is NOT touched by the fix at all.
//      timer_tick() checks `if (s->state == TS_ALARM)` and calls
//      handle_alarm() before it ever reaches the bootClicked branch, and
//      handle_alarm()'s own "any input" test already treats bootClicked and
//      a nonzero key as equivalent members of one OR - so a combined BOOT+
//      PWR chord during the alarm was always going to collapse into exactly
//      ONE dismissal, not two, and not a dropped one. Asserted here as a
//      recorded, deliberate "checked and stayed coherent", not left to
//      chance just because the code path is adjacent to what changed.
//
// This loads the REAL firmware compiled to wasm (emulator/wasm/dist/emu.wasm,
// built by emulator/wasm/build.ts) and drives it through emu_tick() with a
// synthetic clock, same harness shape as the other repro-*.ts tests in this
// directory.
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

// Fires the real combined chord: BOOT down, PWR down, PWR up + short
// verdict, BOOT up, all landing on the SAME tick - the exact sequence the
// bug report says is reachable in practice because BOOT is polled at only
// ~20Hz (bootbtn.h), so releasing both buttons together is enough to land
// their effects on one tick.
function fireCombinedChord(dev: Device, t: number): string[] {
    const before = dev.fwLogLines().length;
    dev.button(BTN_BOOT, true);
    dev.button(BTN_PWR, true);
    dev.button(BTN_PWR, false);
    dev.buttonVerdict(BTN_PWR, false);
    dev.button(BTN_BOOT, false);
    dev.tick(t);
    return dev.fwLogLines().slice(before);
}

type Device = Awaited<ReturnType<typeof loadDevice>>;

async function testRunning() {
    console.log("\n=== 1. combined BOOT+PWR while the countdown is RUNNING ===\n");

    const dev = await loadDevice();
    dev.tick(0);
    dev.appSwitch(APP_TIMER);
    let t = 1;
    dev.tick(t);

    console.log("-- drag the ring to a mid-size value and start it --");
    const [px, py] = panelTouchForAngle(90);
    dev.touch(true, px, py);
    t += 16; dev.tick(t);
    dev.touch(false, 0, 0);
    t += 50; dev.tick(t);
    dev.button(BTN_PWR, true); dev.tick(t);
    dev.button(BTN_PWR, false); dev.buttonVerdict(BTN_PWR, false);
    t += 32; dev.tick(t);
    check("timer started running", dev.fwLogLines().some((l) => l.startsWith("timer: start, ")));

    for (let i = 0; i < 20; i++) { t += 50; dev.tick(t); } // let it run a bit, genuinely RUNNING

    console.log("-- fire the combined chord while RUNNING --");
    t += 16;
    const lines = fireCombinedChord(dev, t);
    console.log("log lines from the combined tick:", lines);

    check(
        "BOOT's reset fired in the combined tick",
        lines.some((l) => l.startsWith("timer: BOOT reset to ")),
        `lines=${JSON.stringify(lines)}`,
    );
    check(
        "the PWR short-press was NOT dropped: a start followed it in the SAME tick",
        lines.some((l) => l.startsWith("timer: start, ")),
        `lines=${JSON.stringify(lines)}`,
    );
    check(
        "both events applied IN ORDER (reset before start), not the other way round",
        lines.findIndex((l) => l.startsWith("timer: BOOT reset to ")) <
        lines.findIndex((l) => l.startsWith("timer: start, ")),
    );

    // Confirm it is genuinely counting down afterwards, not just that the
    // right log lines were printed - same double-check style as the
    // primary repro (a log line alone would not catch a redraw that never
    // actually reflects the new state).
    const hashRightAfter = dev.fbHash();
    t += 2000; dev.tick(t);
    const hashTwoSecondsLater = dev.fbHash();
    check(
        "the framebuffer is actually still changing 2s later (really RUNNING, not frozen)",
        hashRightAfter !== hashTwoSecondsLater,
    );
}

async function testAlarm() {
    console.log("\n=== 2. combined BOOT+PWR while the ALARM is ringing ===\n");

    const dev = await loadDevice();
    dev.tick(0);
    dev.appSwitch(APP_TIMER);
    let t = 1;
    dev.tick(t);

    console.log("-- drag the ring to the smallest settable value (one 5s tick) and start it --");
    // timer.c's coil is a flat step everywhere: a tap at 1 degree always
    // rounds to sub-lap tick 1 (lroundf(1/360*TICKS_PER_LAP) = 1 at both
    // 360 and the current 180), i.e. TICK_STEP_S = 5 seconds - the fastest
    // reachable route to ALARM, unaffected by the real-hardware pass's
    // TICKS_PER_LAP halving. Updated from the old tiered ring's angle=2
    // (which landed on tick slot 0 there); at the coil's finer, flat
    // resolution angle=2 now rounds to tick 2 (10s) instead, so this uses
    // angle=1 to land on exactly one tick.
    const [px, py] = panelTouchForAngle(1);
    dev.touch(true, px, py);
    t += 16; dev.tick(t);
    dev.touch(false, 0, 0);
    t += 50; dev.tick(t);
    dev.button(BTN_PWR, true); dev.tick(t);
    dev.button(BTN_PWR, false); dev.buttonVerdict(BTN_PWR, false);
    t += 32; dev.tick(t);
    const startLine = dev.fwLogLines().find((l) => l.startsWith("timer: start, "));
    check("timer started running at 00:05", startLine === "timer: start, 00:05\r\n", startLine);

    console.log("-- let it run out --");
    let ringingLine: string | undefined;
    for (let i = 0; i < 200 && !ringingLine; i++) {
        t += 50;
        dev.tick(t);
        ringingLine = dev.fwLogLines().find((l) => l === "timer: ringing\r\n");
    }
    check("the alarm started ringing", !!ringingLine);

    // Let a couple of flash cycles play out so this is a genuine mid-ring
    // dismissal, not the very first tick of TS_ALARM.
    for (let i = 0; i < 10; i++) { t += 50; dev.tick(t); }

    console.log("-- fire the combined BOOT+PWR chord while ringing --");
    t += 16;
    const lines = fireCombinedChord(dev, t);
    console.log("log lines from the combined tick:", lines);

    const dismissLines = lines.filter((l) => l.startsWith("timer: alarm dismissed"));
    check(
        "the chord produced EXACTLY ONE dismissal (not two, not zero)",
        dismissLines.length === 1,
        `dismissLines=${JSON.stringify(dismissLines)}, all lines=${JSON.stringify(lines)}`,
    );
    check(
        "the dismissal reports BOOT can recall the value that just alarmed (00:05)",
        dismissLines[0]?.includes("BOOT recalls 00:05") ?? false,
        dismissLines[0],
    );

    // Coherence check: after dismissal the timer must be a static SETTING
    // screen at 00:00, not still counting anything down - two ticks apart
    // should hash identically.
    const hashRightAfter = dev.fbHash();
    t += 2000; dev.tick(t);
    const hashTwoSecondsLater = dev.fbHash();
    check(
        "after the combined dismissal the screen is static (clean SETTING at 00:00, nothing left running)",
        hashRightAfter === hashTwoSecondsLater,
    );

    // And BOOT alone, from here, should recall 00:05 in one press - the
    // "again is one press" contract the dismissal path exists to preserve,
    // now checked past a combined-chord dismissal specifically rather than
    // only past a single-input one.
    t += 16;
    dev.button(BTN_BOOT, true);
    dev.button(BTN_BOOT, false);
    t += 16; dev.tick(t);
    const recallLine = dev.fwLogLines().find((l) => l.startsWith("timer: BOOT recalled "));
    check(
        "BOOT afterwards still recalls 00:05 in one press",
        recallLine === "timer: BOOT recalled 00:05\r\n",
        recallLine,
    );
}

async function main() {
    console.log("=== regression: timer's BOOT+PWR sequencing fix, mirrored across RUNNING and ALARM ===");
    await testRunning();
    await testAlarm();
    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
