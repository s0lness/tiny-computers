// feature-tables-calibration: the NUMPAD touch calibration mode inside
// multiplication-tables practice (firmware/apps/tables.c), driven against
// the REAL compiled firmware, same convention as every feature-*.ts in this
// directory (see feature-tables.ts's own header). This is that app's own
// numpad-bias measurement mode - see tables.c's "NUMPAD TOUCH CALIBRATION"
// section for the full design.
//
// REWRITTEN 2026-08-19, FROM A FITTED LINE TO NINE LEARNED CENTRES. The
// version this replaces prompted 3, 4, 9, then 5 six times (nine samples,
// CALIB_SEQ_DIGITS) and fit offset_y = alpha + beta*y through them. Three
// real runs on the board (same person, same grip, minutes apart) showed key
// 4 - which got exactly ONE sample - swing 31px between runs, and in one
// run the fitted trend no longer even exceeded the six-5s noise floor that
// same run measured. tables.c's own header has the full account. The
// owner's own fix: ask for ten taps per key, all nine keys, and let each
// key's own MEDIAN become its target - no fitted line, no assumed
// functional form.
//
// WHAT THIS FILE PROVES:
//   1. The PWR double-press opens calibration (a log line says so), and a
//      second double-press mid-pass aborts without saving.
//   2. NINETY synthetic taps - all nine digit keys, ten taps each, in the
//      ROTATED order tables.c's own calib_seq_digit()/calib_seq_tap() use
//      (1,2,...,9 repeating, not blocked) - each landing at a KNOWN, FIXED,
//      PER-KEY offset below its own prompted key's drawn centre
//      (TARGET_DY_FOR_DIGIT below - a DIFFERENT number for every one of the
//      nine keys, deliberately, so a single global bias could not pass this
//      test by accident). A flat per-key offset with no noise should drive
//      every key's own printed spread to 0 and every key's own printed
//      median back to exactly that key's TARGET_DY_FOR_DIGIT.
//   3. The calibration is actually SAVED (once, with a generation tag) and
//      then USED: nearest-learned-centre classification, at a probe
//      coordinate chosen so the calibrated centres name a DIFFERENT digit
//      than the shipped 40px default would - see PROBE_* below for the
//      geometry - proving the nine learned centres are what is actually
//      driving numpad_hit_calibrated(), not a coincidence of the
//      coordinate chosen. A FRESH, never-calibrated device reads the SAME
//      raw coordinate as the OLD default's digit, ruling that out.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/feature-tables-calibration.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import {
  NUMPAD_X0, NUMPAD_Y0, CELL_W, CELL_H, digitCell, cellCx, cellCy,
  CALIB_KEYS, CALIB_TAPS_PER_KEY, CALIB_SEQ_LEN, calibSeqDigit, calibSeqTap,
} from "../../../tools/tables-layout";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const BTN_PWR = 1;

// tables.c's own gesture-timing constants (ARM_MS/COMMIT_CONFIRM_MS/
// RELEASE_GRACE_MS, DOUBLE_PRESS_WINDOW_MS = CALIB_DOUBLE_PRESS_WINDOW_MS) -
// calibration's own commit path is now tables_gesture_tick(), the SAME
// function ordinary digit entry uses (see tables.c's own header, THE
// COMMIT PATH), so this file holds every tap exactly as long as
// feature-tables.ts's own pressCellFor()-style taps do - not a shortened
// wait tuned to a private tap detector, because there no longer is one.
const ARM_MS = 40, COMMIT_CONFIRM_MS = 72, RELEASE_GRACE_MS = 300, DOUBLE_PRESS_WINDOW_MS = 500;
const HOLD_MS = ARM_MS + COMMIT_CONFIRM_MS + 60;
const RELEASE_WAIT_MS = RELEASE_GRACE_MS + 60;
const STEP_MS = 16;

// A DIFFERENT known dy per digit key (1..9), 30..54 in steps of 3 - chosen
// so no single flat bias could make this test pass by accident, and small
// enough that every synthetic touch still lands on the numpad's own
// drawn cell (CELL_H=57, so even digit 9's own +54 dy stays inside its
// row plus a little, never wandering into a neighbour's drawn cell).
function targetDy(digit: number): number {
  return 30 + 3 * (digit - 1);
}

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

// One clean tap at a raw panel point, held and released through the SAME
// arm/commit-confirm/release-grace shape feature-tables.ts's own
// pressCellFor() uses for the identical gesture on ordinary gameplay -
// tables_gesture_tick() is now literally the same function on both sides
// (see tables.c's own header, THE COMMIT PATH), so this file no longer
// needs, and does not use, a shorter calibration-only wait.
function tapAt(dev: Device, x: number, y: number) {
  let t = clock;
  const end = t + HOLD_MS;
  while (t < end) { t += STEP_MS; dev.touch(true, x, y); dev.tick(t); }
  dev.touch(false, 0, 0);
  const end2 = t + RELEASE_WAIT_MS;
  while (t < end2) { t += STEP_MS; dev.tick(t); }
  clock = t;
}

// The prompted digit's drawn cell centre, EXACTLY as tables.c's cell_rect()
// computes it (integer division, by + bh/2 with bh=CELL_H=57 -> floor(28.5)
// = 28) - NOT tools/tables-layout.ts's own cellCy() unmodified, which
// divides in floating point (CELL_H/2 = 28.5) and so sits 0.5px above the
// firmware's actual integer target for every odd-height row. That half
// pixel was never load-bearing for older consumers of cellCy() (they only
// needed to land inside a cell's zone, not hit a pixel exactly), but this
// file's own per-tap assertions compare against tables_calib_on_commit()'s
// printed target=(x,y) verbatim, so it has to match bit for bit. cellCx()
// needs no such adjustment: CELL_W=100 is even, so CELL_W/2 is already a
// whole number.
function keyCenter(digit: number): [number, number] {
  const cell = digitCell(digit);
  return [cellCx(cell), Math.floor(cellCy(cell))];
}

async function main() {
  console.log("=== feature: tables numpad touch calibration ===\n");

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
  // open, take one tap (on the real numpad, same as gameplay - the first
  // prompted key, digit 1), abort, and confirm nothing was saved and the
  // ordinary practice screen is what is showing again.
  {
    const probe = await loadDevice();
    await enterTables(probe, APP_TABLES);
    clock = doublePressPWR(probe, clock);
    probe.drainLog();
    const d0 = calibSeqDigit(0);
    const [d0x, d0y] = keyCenter(d0);
    tapAt(probe, d0x, d0y + targetDy(d0));
    probe.drainLog();
    clock = doublePressPWR(probe, clock);
    const log = probe.drainLog();
    check("a second double-press aborts calibration without saving",
      log.some((l) => l.includes("tables: calibration aborted, nothing saved")), log.join(" | "));
    check("an abort never logs a save", !log.some((l) => l.includes("tables: calibration saved")), log.join(" | "));
  }

  // ---- the ninety-tap sequence: nine keys, ten taps each, rotated order,
  // every tap landing at a KNOWN, FLAT, PER-KEY offset below its own
  // prompted key's drawn centre -------------------------------------------
  let sawSaved = false;
  const perTapOk: boolean[] = [];
  const perKeyFinishOk: boolean[] = new Array(CALIB_KEYS).fill(false);
  for (let i = 0; i < CALIB_SEQ_LEN; i++) {
    const digit = calibSeqDigit(i);
    const tap = calibSeqTap(i);
    const dy = targetDy(digit);
    const [tx, ty] = keyCenter(digit);
    tapAt(dev, tx, ty + dy);
    const log = dev.drainLog();

    const tapLine = log.find((l) => l.includes(`tables: calib ${i + 1}/${CALIB_SEQ_LEN} prompted=${digit} tap=${tap + 1}/${CALIB_TAPS_PER_KEY}`));
    const tapOk = !!tapLine && tapLine.includes(`target=(${tx},${ty})`) && tapLine.includes("dx=0") &&
      tapLine.includes(`dy=${dy} px`);
    perTapOk.push(tapOk);

    if (log.some((l) => l === "tables: calibration saved (tag=1)")) sawSaved = true;
    for (let key = 0; key < CALIB_KEYS; key++) {
      const d = key + 1;
      const wantDy = targetDy(d);
      const finishLine = log.find((l) => l.includes(`tables: calib key ${d}: `));
      if (finishLine) {
        perKeyFinishOk[key] = finishLine.includes(`dy min=${wantDy} max=${wantDy} spread=0 median=${wantDy} px`) &&
          finishLine.includes("dx median=0 px");
      }
    }
  }
  check(`every one of the 90 per-tap lines printed prompted key, tap index, target centre, dx=0 and its own dy`,
    perTapOk.every((ok) => ok), `${perTapOk.filter((x) => x).length}/${CALIB_SEQ_LEN}`);
  check("every one of the nine per-key finish lines printed spread=0 and the exact synthetic median (no noise fed in)",
    perKeyFinishOk.every((ok) => ok), `${perKeyFinishOk.filter((x) => x).length}/${CALIB_KEYS}`);
  check("the calibration was saved once, with generation tag 1 (first pass ever)", sawSaved);

  // ---- the saved calibration is actually USED by numpad_hit_calibrated() -
  //
  // PROBE: column 1's own centre x (184), a raw y chosen so nearest-learned-
  // centre names digit 5 while the shipped 40px DEFAULT rectangle+bias
  // names digit 8 instead - worked out from the SAME synthetic offsets this
  // file just fed in (targetDy(5)=42, targetDy(8)=51 -> learned centres at
  // y=239+42=281 and y=296+51=347; a probe at y=310 sits 29px from the
  // first and 37px from the second, and the uncalibrated 40px-biased
  // rectangle grid puts y=310 in row 2 = digit 8's row) - so this is a
  // proof the nine learned centres are actually being read by
  // numpad_hit_calibrated(), not merely that some plausible bias also
  // happened to work.
  const PROBE_X = NUMPAD_X0 + CELL_W + CELL_W / 2; // 184, column 1's centre
  const PROBE_Y = 310;
  {
    const learnedY5 = NUMPAD_Y0 + CELL_H + Math.floor(CELL_H / 2) + targetDy(5); // row1 col1 centre + its dy
    const learnedY8 = NUMPAD_Y0 + 2 * CELL_H + Math.floor(CELL_H / 2) + targetDy(8); // row2 col1 centre + its dy
    const distTo5 = Math.abs(PROBE_Y - learnedY5), distTo8 = Math.abs(PROBE_Y - learnedY8);
    const uncalibratedRow = Math.floor(((PROBE_Y - 40) - NUMPAD_Y0) / CELL_H);
    check("the probe coordinate is a genuine differentiator (nearest learned centre = digit 5, uncalibrated default = digit 8)",
      distTo5 < distTo8 && uncalibratedRow === 2,
      `distTo5=${distTo5} distTo8=${distTo8} uncalibratedRow=${uncalibratedRow}`);
  }

  tapAt(dev, PROBE_X, PROBE_Y);
  {
    const log = dev.drainLog();
    check("after calibration, a touch the shipped default would have misread as '8' is correctly read as the key nearest its learned centre ('5')",
      log.some((l) => l.includes("tables: digit 5")), log.join(" | ") || "(nothing logged)");
  }

  // ---- a FRESH, never-calibrated device reads the SAME raw coordinate as
  // the OLD, uncalibrated key - proving the difference above is really the
  // calibration talking, not a coincidence of the coordinate chosen --------
  {
    const fresh = await loadDevice();
    await enterTables(fresh, APP_TABLES);
    tapAt(fresh, PROBE_X, PROBE_Y);
    const log = fresh.drainLog();
    check("the SAME raw touch, on a device with NO stored calibration, still reads as the shipped default's '8'",
      log.some((l) => l.includes("tables: digit 8")), log.join(" | ") || "(nothing logged)");
  }

  console.log(`\n${passCount} passed, ${failCount} failed`);
  if (failCount > 0) process.exit(1);
}

main();
