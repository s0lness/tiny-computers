// repro-poweroff-gesture: headless verification of the PWR-held-alone
// power-off gesture (runtime_core.c's section of the same name) and,
// specifically, that the BOOT+PWR menu chord can never trigger it, however
// long it is held. Run with:
//
//   bun run emulator/wasm/build.ts        # THIS FILE DOES NOT BUILD ANYTHING
//   bun run emulator/wasm/tests/repro-poweroff-gesture.ts
//
// This loads the REAL firmware compiled to wasm (emulator/wasm/dist/emu.wasm,
// built by emulator/wasm/build.ts) and drives it through emu_tick() with a
// synthetic clock, same harness shape as the other tests in this directory.
//
// There is no i2c1 and no AXP2101 in this build (emu_shim.c's
// sensors_request_poweroff() is a documented no-op - nothing in wasm has a
// PMIC to command), so this test cannot observe a real power cut. What it
// CAN observe, and does, is the DECISION: runtime_core.c calls rt_log()
// exactly once, right before sensors_request_poweroff(), the instant it
// decides to power off ("poweroff: PWR held 3500ms alone..." - that
// firmware line formats PWR_HOLD_POWEROFF_MS in rather than spelling it,
// so it cannot go stale the way this file's mirrored constants can).
// js_log forwards
// every rt_log() call to this test's own callback, so "did firmware decide
// to power off" is checked by grepping the captured log lines, not by
// inspecting any internal state. Whether the AXP2101 actually obeys that
// decision on real silicon can only be checked by a human holding the real
// button - see this task's own verification notes and sensors.h's "THE
// HONESTY REQUIREMENT" comment on sensors_inject_key() for why that gap is
// real and is not what this test claims to close.
import { readFileSync } from "node:fs";
import { join } from "node:path";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const BTN_BOOT = 0;
const BTN_PWR = 1;
// Mirrors of runtime_core.c's constants. NOTHING CHECKS THAT THEY STILL
// MATCH: this file loads a PREBUILT dist/emu.wasm and never compiles the
// firmware, so changing PWR_HOLD_POWEROFF_MS in C and running this without
// rebuilding checks the PREVIOUS binary against the NEW numbers - which is
// exactly how the 5000 -> 3500 change first "failed", with two assertions
// reporting a threshold nobody had compiled yet (see AGENTS.md, "A one-shot
// build that is not checked will leave the PREVIOUS emu.wasm in place").
// Rebuild first, always.
//
// Every margin below is derived from these three, never spelled as a
// literal: when the threshold moved, the scenarios written around 4900 and
// 5100 silently changed meaning ("just before" became "well after"), and a
// test whose intent depends on arithmetic the reader has to redo is a test
// that will lie again on the next change.
const DIM_START_MS = 1500; // PWR_HOLD_DIM_START_MS, runtime_core.c
const POWEROFF_MS = 3500; // PWR_HOLD_POWEROFF_MS, runtime_core.c
const RECOVER_MS = 1500; // PWR_HOLD_RECOVER_MS, runtime_core.c
const HARD_CEILING_MS = POWEROFF_MS + RECOVER_MS; // PWR_HOLD_HARD_CEILING_MS
const POWEROFF_LOG_NEEDLE = "requesting power-off";

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
    const logLines: string[] = [];

    const imports = {
        env: {
            js_log(ptr: number, len: number) {
                const b = new Uint8Array(memory.buffer, ptr, len);
                const line = decoder.decode(b);
                logLines.push(line);
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
        tick(nowMs: number) { exp.emu_tick(nowMs); },
        button(index: number, down: boolean) { exp.emu_button(index, down ? 1 : 0); },
        buttonVerdict(index: number, isLong: boolean) { exp.emu_button_verdict(index, isLong ? 1 : 0); },
        appCurrent(): number { return exp.emu_app_current(); },
        poweredOffSince(sinceIndex: number): boolean {
            return logLines.slice(sinceIndex).some((l) => l.includes(POWEROFF_LOG_NEEDLE));
        },
        loggedSince(sinceIndex: number, needle: string): boolean {
            return logLines.slice(sinceIndex).some((l) => l.includes(needle));
        },
        logCount(): number { return logLines.length; },
    };
}

type Device = Awaited<ReturnType<typeof loadDevice>>;

async function main() {
    console.log(`=== reproduction + regression: the PWR-held-alone power-off gesture (threshold ${POWEROFF_MS}ms) ===\n`);

    // ---- scenario 1: PWR held past the threshold -> must decide to power
    //      off --------------------------------------------------------------
    {
        const dev = await loadDevice();
        dev.tick(0);
        const mark = dev.logCount();
        const holdTo = POWEROFF_MS + 100;
        console.log(`-- PWR held alone, 0ms -> ${holdTo}ms, no release --`);
        dev.button(BTN_PWR, true);
        for (let t = 0; t <= holdTo; t += 50) dev.tick(t);
        check(
            `PWR held alone for ${POWEROFF_MS}ms decides to power off`,
            dev.poweredOffSince(mark),
            "expected a log line containing " + JSON.stringify(POWEROFF_LOG_NEEDLE),
        );
    }

    // ---- scenario 2: PWR held alone, released just BEFORE the threshold ---
    //      -> must not ------------------------------------------------------
    {
        const dev = await loadDevice();
        dev.tick(0);
        const mark = dev.logCount();
        const releaseAt = POWEROFF_MS - 100;
        console.log(`-- PWR held alone, 0ms -> ${releaseAt}ms, then released --`);
        dev.button(BTN_PWR, true);
        for (let t = 0; t <= releaseAt; t += 50) dev.tick(t);
        dev.button(BTN_PWR, false);
        dev.tick(releaseAt + 50);
        // Keep ticking well past where the threshold since the original
        // press would have landed, to prove the timer does not keep running
        // after release.
        dev.tick(HARD_CEILING_MS + 1000);
        check(
            `releasing PWR at ${releaseAt}ms never powers off, even past the ${POWEROFF_MS}ms mark`,
            !dev.poweredOffSince(mark),
        );
    }

    // ---- scenario 3: the menu chord, held well past every threshold in the
    //      gesture -> must NEVER power off, however long it is held (the
    //      owner's requirement, verbatim).
    {
        const dev = await loadDevice();
        dev.tick(0);
        const mark = dev.logCount();
        const holdTo = HARD_CEILING_MS + 3000; // past the decision AND the
                                                // hard ceiling, with room
        console.log(`-- BOOT+PWR chord held together for ${holdTo}ms (well past the ${POWEROFF_MS}ms threshold) --`);
        dev.button(BTN_BOOT, true);
        dev.tick(0);
        dev.button(BTN_PWR, true);
        dev.tick(10);
        dev.buttonVerdict(BTN_PWR, true); // PMIC's own 1.5s long-press verdict
        dev.tick(10 + DIM_START_MS);
        const openedMenu = dev.appCurrent() === -1;
        for (let t = DIM_START_MS + 100; t <= holdTo; t += 50) dev.tick(t);
        dev.button(BTN_PWR, false);
        dev.tick(holdTo + 50);
        dev.button(BTN_BOOT, false);
        dev.tick(holdTo + 100);
        check("the chord opened the menu (sanity check this really is the chord)", openedMenu);
        check(
            `the chord, held ${holdTo}ms, never decides to power off`,
            !dev.poweredOffSince(mark),
        );
    }

    // ---- scenario 4: BOOT touches the hold AFTER dimming has started, then
    //      releases while PWR keeps being held past the threshold ----------
    //      -> must still never power off. This is the "tainted hold" design
    //      decision documented in runtime_core.c: a per-instant "is BOOT
    //      down right now" check would allow exactly this to power off.
    {
        const dev = await loadDevice();
        dev.tick(0);
        const mark = dev.logCount();
        const holdTo = HARD_CEILING_MS + 1000;
        console.log(`-- PWR held alone past the dim start, BOOT touches it, BOOT released, PWR held to ${holdTo}ms --`);
        dev.button(BTN_PWR, true);
        for (let t = 0; t <= DIM_START_MS + 200; t += 50) dev.tick(t); // into the ramp
        dev.button(BTN_BOOT, true);
        dev.tick(DIM_START_MS + 250);
        dev.button(BTN_BOOT, false); // let go of BOOT again, PWR still held
        for (let t = DIM_START_MS + 300; t <= holdTo; t += 50) dev.tick(t);
        dev.button(BTN_PWR, false);
        dev.tick(holdTo + 50);
        check(
            "a hold BOOT ever touched stays tainted for its whole duration, even after BOOT lets go",
            !dev.poweredOffSince(mark),
        );
    }

    // ---- scenario 5: the shutdown write does nothing, and PWR is never ---
    //      released - the exact failure the owner hit on real hardware.
    //      sensors_request_poweroff() is ALWAYS a no-op in this wasm build
    //      (no PMIC to command - emu_shim.c), so holding straight through
    //      the decision point is a perfect, deterministic reproduction of
    //      "the shutdown did not take": prove the panel is forced back to
    //      baseline within the recovery window rather than staying dark
    //      forever. RECOVER_MS/HARD_CEILING_MS are module-scope mirrors of
    //      runtime_core.c's PWR_HOLD_RECOVER_MS and
    //      PWR_HOLD_HARD_CEILING_MS - see their comment at the top.
    {
        const dev = await loadDevice();
        dev.tick(0);
        const mark = dev.logCount();
        console.log("-- PWR held alone, straight through the decision, never released (shutdown does nothing in wasm) --");
        dev.button(BTN_PWR, true);
        for (let t = 0; t <= HARD_CEILING_MS + 500; t += 50) dev.tick(t);
        check(
            `still decides to power off at ${POWEROFF_MS}ms (the write is attempted)`,
            dev.poweredOffSince(mark),
        );
        check(
            "recovers on its own once the shutdown demonstrably did not take",
            dev.loggedSince(mark, "restoring brightness"),
        );

        // Keep ticking with PWR still (physically) held, no new press event
        // ever arriving (real hardware only edges on an actual press/release
        // cycle) - the recovery must not re-fire the decision repeatedly.
        const afterRecoverMark = dev.logCount();
        for (let t = HARD_CEILING_MS + 550; t <= HARD_CEILING_MS + 4000; t += 50) dev.tick(t);
        check(
            "does not repeatedly re-decide to power off while still held with no new press",
            !dev.poweredOffSince(afterRecoverMark),
        );
    }

    // ---- scenario 6: a normal release right at the moment recovery would -
    //      have been needed still leaves the device in a sane, undimmed
    //      state - releasing is always at least as good as the automatic
    //      recovery ------------------------------------------------------
    {
        const dev = await loadDevice();
        dev.tick(0);
        const mark = dev.logCount();
        console.log(`-- PWR held past ${POWEROFF_MS}ms, released shortly after (before the recovery window elapses) --`);
        dev.button(BTN_PWR, true);
        for (let t = 0; t <= POWEROFF_MS + 200; t += 50) dev.tick(t);
        const decided = dev.poweredOffSince(mark);
        dev.button(BTN_PWR, false);
        dev.tick(POWEROFF_MS + 250);
        const releaseMark = dev.logCount();
        // Ticking well past what would have been the hard ceiling: since PWR
        // is now up, the recovery invariant must not need to fire at all
        // (release already handles it) - no "restoring brightness" line.
        for (let t = POWEROFF_MS + 300; t <= POWEROFF_MS + 3000; t += 50) dev.tick(t);
        check("still decided to power off before the release", decided);
        check(
            "a release after the decision needs no separate recovery log line",
            !dev.loggedSince(releaseMark, "restoring brightness"),
        );
    }

    // ---- scenario 7: a short PWR tap (KEY_SHORT territory) never dims or -
    //      powers off ----------------------------------------------------
    {
        const dev = await loadDevice();
        dev.tick(0);
        const mark = dev.logCount();
        console.log("-- a plain short PWR press-and-release --");
        dev.button(BTN_PWR, true);
        dev.tick(100);
        dev.button(BTN_PWR, false);
        dev.buttonVerdict(BTN_PWR, false);
        dev.tick(150);
        dev.tick(HARD_CEILING_MS + 1000); // idle well past where the
                                           // threshold since the press
                                           // would have landed
        check("a short PWR tap never decides to power off", !dev.poweredOffSince(mark));
    }

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
