// repro-poweroff-gesture: headless verification of the PWR-held-5s
// power-off gesture (runtime_core.c's "the PWR-held-alone power-off
// gesture" section) and, specifically, that the BOOT+PWR menu chord can
// never trigger it, however long it is held. Run with:
//
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
// decides to power off ("poweroff: PWR held 5s alone..."). js_log forwards
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
const DIM_START_MS = 1500; // PWR_HOLD_DIM_START_MS, runtime_core.c
const POWEROFF_MS = 5000; // PWR_HOLD_POWEROFF_MS, runtime_core.c
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
        logCount(): number { return logLines.length; },
    };
}

type Device = Awaited<ReturnType<typeof loadDevice>>;

async function main() {
    console.log("=== reproduction + regression: the PWR-held-5s power-off gesture ===\n");

    // ---- scenario 1: PWR held alone, past 5s -> must decide to power off --
    {
        const dev = await loadDevice();
        dev.tick(0);
        const mark = dev.logCount();
        console.log("-- PWR held alone, 0ms -> 5100ms, no release --");
        dev.button(BTN_PWR, true);
        for (let t = 0; t <= 5100; t += 50) dev.tick(t);
        check(
            "PWR held alone for 5s decides to power off",
            dev.poweredOffSince(mark),
            "expected a log line containing " + JSON.stringify(POWEROFF_LOG_NEEDLE),
        );
    }

    // ---- scenario 2: PWR held alone, released just BEFORE 5s -> must not -
    {
        const dev = await loadDevice();
        dev.tick(0);
        const mark = dev.logCount();
        console.log("-- PWR held alone, 0ms -> 4900ms, then released --");
        dev.button(BTN_PWR, true);
        for (let t = 0; t <= 4900; t += 50) dev.tick(t);
        dev.button(BTN_PWR, false);
        dev.tick(4950);
        // Keep ticking well past where 5s-since-original-press would have
        // landed, to prove the timer does not keep running after release.
        dev.tick(6000);
        check(
            "releasing PWR at 4.9s never powers off, even past the 5s mark",
            !dev.poweredOffSince(mark),
        );
    }

    // ---- scenario 3: the menu chord, held for 8s total -> must NEVER ------
    //      power off, however long it is held (the owner's requirement,
    //      verbatim).
    {
        const dev = await loadDevice();
        dev.tick(0);
        const mark = dev.logCount();
        console.log("-- BOOT+PWR chord held together for 8s (well past the 5s threshold) --");
        dev.button(BTN_BOOT, true);
        dev.tick(0);
        dev.button(BTN_PWR, true);
        dev.tick(10);
        dev.buttonVerdict(BTN_PWR, true); // PMIC's own 1.5s long-press verdict
        dev.tick(10 + 1500);
        const openedMenu = dev.appCurrent() === -1;
        for (let t = 1600; t <= 8000; t += 50) dev.tick(t);
        dev.button(BTN_PWR, false);
        dev.tick(8050);
        dev.button(BTN_BOOT, false);
        dev.tick(8100);
        check("the chord opened the menu (sanity check this really is the chord)", openedMenu);
        check(
            "the chord, held 8s, never decides to power off",
            !dev.poweredOffSince(mark),
        );
    }

    // ---- scenario 4: BOOT touches the hold AFTER dimming has started, then
    //      releases while PWR keeps being held past the original 5s mark ---
    //      -> must still never power off. This is the "tainted hold" design
    //      decision documented in runtime_core.c: a per-instant "is BOOT
    //      down right now" check would allow exactly this to power off.
    {
        const dev = await loadDevice();
        dev.tick(0);
        const mark = dev.logCount();
        console.log("-- PWR held alone past the dim start, BOOT touches it, BOOT released, PWR held to 6s --");
        dev.button(BTN_PWR, true);
        for (let t = 0; t <= DIM_START_MS + 200; t += 50) dev.tick(t); // into the ramp
        dev.button(BTN_BOOT, true);
        dev.tick(DIM_START_MS + 250);
        dev.button(BTN_BOOT, false); // let go of BOOT again, PWR still held
        for (let t = DIM_START_MS + 300; t <= 6000; t += 50) dev.tick(t);
        dev.button(BTN_PWR, false);
        dev.tick(6050);
        check(
            "a hold BOOT ever touched stays tainted for its whole duration, even after BOOT lets go",
            !dev.poweredOffSince(mark),
        );
    }

    // ---- scenario 5: a short PWR tap (KEY_SHORT territory) never dims or -
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
        dev.tick(6000); // idle well past where 5s-since-press would land
        check("a short PWR tap never decides to power off", !dev.poweredOffSince(mark));
    }

    console.log(`\n${passCount} passed, ${failCount} failed`);
    if (failCount > 0) process.exit(1);
}

main();
