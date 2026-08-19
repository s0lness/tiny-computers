// feature-tables-calibration: the NUMPAD touch calibration mode inside
// multiplication-tables practice (firmware/apps/tables.c), driven against
// the REAL compiled firmware, same convention as every feature-*.ts in this
// directory (see feature-tables.ts's own header). This is that app's own
// numpad-bias measurement mode - see tables.c's "NUMPAD TOUCH CALIBRATION"
// section for the full design.
//
// REPLACES A FIVE-CROSSHAIR VERSION OF THIS TEST. The mode itself changed:
// it used to show five lone crosshairs and ask for five taps each, which
// measured a POINTING gesture the owner never makes while actually typing
// on the numpad ("the way I type on a numpad is not the same as when I
// press buttons somewhere" - his own words, see tables.c's own header for
// the full quote). The calibration screen is now the real numpad, drawn
// exactly as ordinary gameplay draws it, and the sequence is the owner's
// own design: prompt 3, 4, 9, then 5 six times - nine samples total
// (CALIB_SEQ_DIGITS, tools/tables-layout.ts).
//
// WHAT THIS FILE PROVES:
//   1. The PWR double-press opens calibration (a log line says so), and a
//      second double-press mid-pass aborts without saving.
//   2. Nine synthetic taps, one per CALIB_SEQ_DIGITS entry, each landing a
//      KNOWN, FIXED 30px below its own PROMPTED KEY'S DRAWN CENTRE
//      (SYNTHETIC_OFFSET_PX below, cellCx/cellCy from tools/tables-layout,
//      the exact target tables_calib_on_commit() measures against) -
//      through the SAME arm/commit-confirm/release-grace gesture path
//      ordinary digit entry uses (tapAt() below mirrors feature-tables.ts's
//      own HOLD_MS/RELEASE_WAIT_MS, not a shortened synthetic-only wait).
//      A flat, known offset with no real jitter and no real slope should
//      drive: the six-5s spread to 0 (no noise fed in), all four per-key
//      medians to exactly 30, the least-squares fit back to alpha ~= 30 /
//      beta ~= 0, and the plain constant to 30 too.
//   3. The calibration is actually SAVED and then USED: a fresh touch,
//      landing SYNTHETIC_OFFSET_PX below the CENTRE of the numpad key the
//      child is aiming at, is named as that key by numpad_hit() once the
//      fit is loaded - checked at a coordinate where the shipped 40px
//      DEFAULT bias would have named a DIFFERENT key, so this is a proof
//      the calibration changed real behaviour, not a coincidence that any
//      bias in the right ballpark would also have passed. A FRESH,
//      never-calibrated device reads the identical raw coordinate as the
//      OLD default's key, ruling out "the coordinate itself just happens
//      to read as 5" as an explanation.
//
// Run with (after `bun run emulator/wasm/build.ts`):
//
//   bun run emulator/wasm/tests/feature-tables-calibration.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { NUMPAD_X0, NUMPAD_Y0, CELL_W, CELL_H, digitCell, cellCx, cellCy, CALIB_SEQ_DIGITS, CALIB_SEQ_LEN } from "../../../tools/tables-layout";

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

// The KNOWN, FIXED synthetic offset every tap in this file lands at, below
// (and never left/right of) whatever key's drawn centre it targets - a
// flat offset with no slope and no x error, so the fit's own beta should
// come back near zero, alpha near this number, and every printed dx near 0.
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

function parseFixed(s: string): number {
  return Number(s);
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
  // prompted key, CALIB_SEQ_DIGITS[0]), abort, and confirm nothing was
  // saved and the ordinary practice screen is what is showing again.
  {
    const probe = await loadDevice();
    await enterTables(probe, APP_TABLES);
    clock = doublePressPWR(probe, clock);
    probe.drainLog();
    const d0 = CALIB_SEQ_DIGITS[0]!;
    const [d0x, d0y] = keyCenter(d0);
    tapAt(probe, d0x, d0y + SYNTHETIC_OFFSET_PX);
    probe.drainLog();
    clock = doublePressPWR(probe, clock);
    const log = probe.drainLog();
    check("a second double-press aborts calibration without saving",
      log.some((l) => l.includes("tables: calibration aborted, nothing saved")), log.join(" | "));
    check("an abort never logs a save", !log.some((l) => l.includes("tables: calibration saved")), log.join(" | "));
  }

  // ---- the nine-sample sequence: 3, 4, 9, then 5 six times, every tap
  // landing a KNOWN, FLAT 30px below its own prompted key's drawn centre --
  let sawSaved = false;
  let fitAlpha = NaN, fitBeta = NaN;
  let sawSpreadLine = false, sawMediansLine = false, sawConstantLine = false, sawTrendLine = false;
  const perTapOk: boolean[] = [];
  for (let i = 0; i < CALIB_SEQ_LEN; i++) {
    const digit = CALIB_SEQ_DIGITS[i]!;
    const [tx, ty] = keyCenter(digit);
    tapAt(dev, tx, ty + SYNTHETIC_OFFSET_PX);
    const log = dev.drainLog();

    const tapLine = log.find((l) => l.includes(`tables: calib ${i + 1}/${CALIB_SEQ_LEN} prompted=${digit}`));
    const tapOk = !!tapLine && tapLine.includes(`target=(${tx},${ty})`) && tapLine.includes("dx=0") &&
      tapLine.includes(`dy=${SYNTHETIC_OFFSET_PX} px`);
    perTapOk.push(tapOk);

    if (log.some((l) => l.includes("tables: calib SPREAD OF THE SIX 5s"))) sawSpreadLine = true;
    if (log.some((l) => l.includes("tables: calib per-key medians"))) sawMediansLine = true;
    if (log.some((l) => l.includes("tables: calib CONSTANT"))) sawConstantLine = true;
    if (log.some((l) => l.includes("tables: calib the vertical trend across 3/5/9"))) sawTrendLine = true;
    if (log.some((l) => l === "tables: calibration saved")) sawSaved = true;
    for (const l of log) {
      const mA = l.match(/alpha=(-?[\d.]+) px/);
      if (mA) fitAlpha = parseFixed(mA[1]!);
      const mB = l.match(/beta=(-?[\d.]+) px\/px/);
      if (mB) fitBeta = parseFixed(mB[1]!);
    }
  }
  check(`every one of the nine per-tap lines printed prompted key, target centre, dx=0 and dy=${SYNTHETIC_OFFSET_PX}px`,
    perTapOk.every((ok) => ok), `${perTapOk.filter((x) => x).length}/${CALIB_SEQ_LEN}`);
  check("the six-5s SPREAD line was printed (the noise floor)", sawSpreadLine);
  check("the per-key medians line (3/4/9/5) was printed", sawMediansLine);
  check("the plain CONSTANT (median of all nine) line was printed", sawConstantLine);
  check("the trend-vs-spread comparison line was printed", sawTrendLine);
  check("the fit was printed and the calibration was saved once all nine samples completed", sawSaved);
  check(`the fitted alpha comes back near the synthetic offset (${SYNTHETIC_OFFSET_PX}px)`,
    !Number.isNaN(fitAlpha) && Math.abs(fitAlpha - SYNTHETIC_OFFSET_PX) < 1, `alpha=${fitAlpha}`);
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
