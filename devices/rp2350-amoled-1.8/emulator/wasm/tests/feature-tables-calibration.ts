// feature-tables-calibration: the five-point touch calibration mode inside
// multiplication-tables practice (firmware/apps/tables.c), driven against
// the REAL compiled firmware, same convention as every feature-*.ts in this
// directory (see feature-tables.ts's own header). This is that app's own
// numpad-bias measurement mode - see tables.c's "FIVE-POINT TOUCH
// CALIBRATION" section for the full design: PWR double-press opens/aborts
// it, five targets near the numpad's own corners plus its centre, five taps
// each, the MEDIAN of the five raw per-tap deltas kept per target (not the
// mean - this controller is noisy), and a least-squares line fitted over
// the five per-target medians and packed into ONE stored record.
//
// WHAT THIS FILE PROVES:
//   1. The PWR double-press opens calibration (a log line says so).
//   2. Five synthetic taps per target, each landing a KNOWN, FIXED 30px
//      below its own crosshair (SYNTHETIC_OFFSET_PX below), drive the fit
//      back to alpha ~= 30, beta ~= 0 - a flat, known offset should recover
//      a flat line, not a spurious slope.
//   3. The calibration is actually SAVED and then USED: a fresh touch,
//      landing SYNTHETIC_OFFSET_PX below the CENTRE of the numpad key the
//      child is aiming at, is named as that key by numpad_hit() once the
//      fit is loaded - checked at a coordinate where the shipped 40px
//      DEFAULT bias would have named a DIFFERENT key, so this is a proof
//      the calibration changed real behaviour, not a coincidence that any
//      bias in the right ballpark would also have passed.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/feature-tables-calibration.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import {
  NUMPAD_X0, NUMPAD_Y0, CELL_W, CELL_H, NUMPAD_COLS,
  CALIB_TARGET_COUNT, CALIB_TARGET_X, CALIB_TARGET_Y,
} from "../../../tools/tables-layout";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const BTN_PWR = 1;

// tables.c's own gesture-timing constants (ARM_SAMPLES/ARM_MS/ARM_RATE_HZ,
// RELEASE_GRACE_MS, DOUBLE_PRESS_WINDOW_MS = CALIB_DOUBLE_PRESS_WINDOW_MS) -
// calibration's own tap detector reuses the numpad's own arm/release-grace
// idiom verbatim (see tables_calib_tick()'s own comment), so the same
// margins feature-tables.ts already uses for a "clean tap" apply here too.
const ARM_MS = 40, RELEASE_GRACE_MS = 300, DOUBLE_PRESS_WINDOW_MS = 500;
const HOLD_MS = ARM_MS + 60;          // comfortably past the arm point
const RELEASE_WAIT_MS = RELEASE_GRACE_MS + 60;
const STEP_MS = 16;

// The KNOWN, FIXED synthetic offset every tap in this file lands at, below
// whatever crosshair or key centre it is aimed at - a flat offset with no
// slope, so the fit's own beta should come back near zero and alpha near
// this number.
const SYNTHETIC_OFFSET_PX = 30;

let passCount = 0, failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
  if (ok) passCount++; else failCount++;
  console.log(`[${ok ? "PASS" : "FAIL"}] ${label}${detail ? " - " + detail : ""}`);
}

async function loadDevice() {
  let memory!: WebAssembly.Memory;
  const dec = new TextDecoder();
  const fwLog: string[] = [];
  const inst = await WebAssembly.instantiate(await WebAssembly.compile(readFileSync(WASM_PATH)), {
    env: {
      js_log(ptr: number, len: number) { fwLog.push(dec.decode(new Uint8Array(memory.buffer, ptr, len)).trimEnd()); },
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
  });
  memory = inst.exports.memory as WebAssembly.Memory;
  const e = inst.exports as any;
  if (e.emu_init() !== 1) throw new Error("emu_init() failed");
  return {
    tick(nowMs: number) { e.emu_tick(nowMs); },
    touch(down: boolean, x: number, y: number) { e.emu_touch(down ? 1 : 0, Math.round(x), Math.round(y)); },
    button(index: number, down: boolean) { e.emu_button(index, down ? 1 : 0); },
    buttonVerdict(index: number, isLong: boolean) { e.emu_button_verdict(index, isLong ? 1 : 0); },
    appSwitch(i: number) { e.emu_app_switch(i); },
    drainLog(): string[] { const o = fwLog.slice(); fwLog.length = 0; return o; },
    device(): any {
      const ptr = e.emu_device();
      const bytes = new Uint8Array(memory.buffer, ptr);
      let end = 0; while (bytes[end] !== 0) end++;
      return JSON.parse(dec.decode(bytes.subarray(0, end)));
    },
  };
}
type Device = Awaited<ReturnType<typeof loadDevice>>;

let clock = 0;

async function enterTables(dev: Device, appIndex: number) {
  dev.tick(0);
  dev.appSwitch(appIndex);
  clock = 16;
  dev.tick(clock);
  dev.drainLog();
}

// One PWR press-and-release with the PMIC's own short-press verdict
// (KEY_SHORT) - mirrors feature-clock.ts's shortPressPWR(), the real edges
// a physical short tap produces (sensors.h, emu_shim.c's PWR key section).
function shortPressPWR(dev: Device, t0: number): number {
  let t = t0;
  dev.button(BTN_PWR, true);
  t += 20; dev.tick(t);
  dev.button(BTN_PWR, false);
  dev.buttonVerdict(BTN_PWR, false);
  t += 20; dev.tick(t);
  return t;
}
function doublePressPWR(dev: Device, t0: number): number {
  let t = shortPressPWR(dev, t0);
  t += 100; // well inside DOUBLE_PRESS_WINDOW_MS, comfortably clear of it too
  t = shortPressPWR(dev, t);
  return t;
}

// One clean tap at a raw panel point - held long enough to arm, released,
// then waited out past RELEASE_GRACE_MS, same "clean tap" idiom feature-
// tables.ts's own pressCellFor() uses for the numpad's identical gesture
// shape (calibration's own tap detector is a private copy of it).
function tapAt(dev: Device, x: number, y: number) {
  let t = clock;
  const end = t + HOLD_MS;
  while (t < end) { t += STEP_MS; dev.touch(true, x, y); dev.tick(t); }
  dev.touch(false, 0, 0);
  const end2 = t + RELEASE_WAIT_MS;
  while (t < end2) { t += STEP_MS; dev.tick(t); }
  clock = t;
}

function parseFixed(s: string): number {
  return Number(s);
}

async function main() {
  console.log("=== feature: tables five-point touch calibration ===\n");

  let APP_TABLES = -1;
  {
    const dev = await loadDevice();
    const d = dev.device();
    APP_TABLES = (d.apps || []).map((a: string) => a.toLowerCase()).indexOf("tables");
    check("the app table carries 'tables'", APP_TABLES >= 0, `index ${APP_TABLES} of ${JSON.stringify(d.apps)}`);
    if (APP_TABLES < 0) { console.log(`\n${passCount} passed, ${failCount} failed`); process.exit(1); }
  }

  // ---- the double-press opens calibration --------------------------------
  const dev = await loadDevice();
  await enterTables(dev, APP_TABLES);
  clock = doublePressPWR(dev, clock);
  {
    const log = dev.drainLog();
    check("a PWR double-press opens calibration", log.some((l) => l.includes("tables: calibration opened")),
      log.join(" | ") || "(nothing logged)");
  }

  // ---- ABORT: a second double-press mid-calibration discards everything --
  //
  // Checked on its OWN probe device (never affecting the main run below):
  // open, take one tap, abort, and confirm nothing was saved and the
  // ordinary practice screen is what is showing again.
  {
    const probe = await loadDevice();
    await enterTables(probe, APP_TABLES);
    clock = doublePressPWR(probe, clock);
    probe.drainLog();
    tapAt(probe, CALIB_TARGET_X[0]!, CALIB_TARGET_Y[0]! + SYNTHETIC_OFFSET_PX);
    probe.drainLog();
    clock = doublePressPWR(probe, clock);
    const log = probe.drainLog();
    check("a second double-press aborts calibration without saving",
      log.some((l) => l.includes("tables: calibration aborted, nothing saved")), log.join(" | "));
    check("an abort never logs a save", !log.some((l) => l.includes("tables: calibration saved")), log.join(" | "));
  }

  // ---- five targets, five taps each, all landing a KNOWN, FLAT 30px below
  // their own crosshair -----------------------------------------------------
  let sawSaved = false;
  let fitAlpha = NaN, fitBeta = NaN;
  const targetMedianLines: string[] = [];
  for (let ti = 0; ti < CALIB_TARGET_COUNT; ti++) {
    const tx = CALIB_TARGET_X[ti]!, ty = CALIB_TARGET_Y[ti]!;
    let sawAllFiveDeltas = true;
    for (let k = 0; k < 5; k++) {
      tapAt(dev, tx, ty + SYNTHETIC_OFFSET_PX);
      const log = dev.drainLog();
      const deltaLine = log.find((l) => l.includes(`tables: calib target ${ti + 1}/${CALIB_TARGET_COUNT} tap ${k + 1}/5`));
      if (!deltaLine || !deltaLine.includes(`delta=${SYNTHETIC_OFFSET_PX} px`)) sawAllFiveDeltas = false;
      const medianLine = log.find((l) => l.includes(`tables: calib target ${ti + 1}/${CALIB_TARGET_COUNT} (y=${ty}) done`));
      if (medianLine) targetMedianLines.push(medianLine);
      const savedLine = log.find((l) => l === "tables: calibration saved");
      if (savedLine) sawSaved = true;
      for (const l of log) {
        const mA = l.match(/tables: calib\s+alpha = (-?[\d.]+) px$/);
        if (mA) fitAlpha = parseFixed(mA[1]!);
        const mB = l.match(/tables: calib\s+beta\s+= (-?[\d.]+) px\/px$/);
        if (mB) fitBeta = parseFixed(mB[1]!);
      }
    }
    check(`target ${ti + 1}/${CALIB_TARGET_COUNT} (y=${ty}): every one of its five raw per-tap deltas printed exactly ${SYNTHETIC_OFFSET_PX}px`,
      sawAllFiveDeltas);
  }
  check("all five per-target medians were printed (raw, before any fit)", targetMedianLines.length === CALIB_TARGET_COUNT,
    `${targetMedianLines.length} of ${CALIB_TARGET_COUNT}: ${targetMedianLines.join(" | ")}`);
  check("every per-target median was exactly the synthetic offset (no slope in the input, so none should appear in the medians)",
    targetMedianLines.every((l) => l.includes(`median = ${SYNTHETIC_OFFSET_PX} px`)), targetMedianLines.join(" | "));
  check("the fit was printed and the calibration was saved once all five targets completed", sawSaved);
  check(`the fitted alpha comes back near the synthetic offset (${SYNTHETIC_OFFSET_PX}px)`,
    !Number.isNaN(fitAlpha) && Math.abs(fitAlpha - SYNTHETIC_OFFSET_PX) < 1,
    `alpha=${fitAlpha}`);
  check("the fitted beta comes back near zero (a flat offset has no slope to find)",
    !Number.isNaN(fitBeta) && Math.abs(fitBeta) < 0.02, `beta=${fitBeta}`);

  // ---- the saved calibration is actually USED by numpad_hit() -----------
  //
  // Column 1's own centre x, and a raw y chosen so the TRUE required
  // correction (this file's own 30px) and the shipped 40px DEFAULT land in
  // DIFFERENT rows: with a 30px bias the touch resolves into row 1 (the
  // "5" key), with the old flat 40px guess it resolves one row up (the
  // "2" key) instead - so this is a proof the fit is actually being read
  // by numpad_hit(), not merely that some plausible bias still worked.
  const col1CenterX = NUMPAD_X0 + CELL_W + CELL_W / 2; // 184
  const boundaryRawY = NUMPAD_Y0 + CELL_H + (SYNTHETIC_OFFSET_PX + 2); // 243: (this - 30) sits 2px inside row 1
  check("the chosen probe coordinate is a genuine differentiator (30px bias names row 1, 40px would have named row 0)",
    (() => {
      const withCalib = Math.floor(((boundaryRawY - SYNTHETIC_OFFSET_PX) - NUMPAD_Y0) / CELL_H);
      const withDefault = Math.floor(((boundaryRawY - 40) - NUMPAD_Y0) / CELL_H);
      return withCalib === 1 && withDefault === 0;
    })(), `boundaryRawY=${boundaryRawY}`);

  tapAt(dev, col1CenterX, boundaryRawY);
  {
    const log = dev.drainLog();
    check("after calibration, a touch the shipped default would have misread as '2' is correctly read as the key she aimed at ('5')",
      log.some((l) => l.includes("tables: digit 5")), log.join(" | ") || "(nothing logged)");
  }

  // ---- a FRESH, never-calibrated device reads the SAME raw coordinate as
  // the OLD, uncalibrated key - proving the difference above is really the
  // calibration talking, not a coincidence of the coordinate chosen --------
  {
    const fresh = await loadDevice();
    await enterTables(fresh, APP_TABLES);
    tapAt(fresh, col1CenterX, boundaryRawY);
    const log = fresh.drainLog();
    check("the SAME raw touch, on a device with NO stored calibration, still reads as the shipped default's '2'",
      log.some((l) => l.includes("tables: digit 2")), log.join(" | ") || "(nothing logged)");
  }

  console.log(`\n${passCount} passed, ${failCount} failed`);
  if (failCount > 0) process.exit(1);
}

main();
