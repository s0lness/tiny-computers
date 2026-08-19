// Bug 7: the emulator was kinder than the hardware.
//
// The sketchpad's palette passed all 22 of its clean-input checks and did
// not work in the owner's hands. The mechanism to do better already
// existed - emulator/src/touchsim.ts has modelled dropouts and strays
// since before the rewrite - and it was pointed at gentle weather because
// nothing ever required otherwise. AGENTS.md now says, in prose, that a
// touch-driven feature needs both kinds of file. This is the mechanical
// half of that sentence.
//
// THE RULE: a test file that drives touch must drive it through TouchSim,
// unless it is named in the baseline below with a reason. Not "should".
// The baseline is what makes a clean-input gesture test the exception that
// has to be argued for rather than the default nobody notices.
//
// WHY A BASELINE AND NOT A FLAG DAY. Every file in the directory predates
// this rule, and rewriting eighteen tests someone else is actively editing
// to satisfy a lint would be a worse trade than writing down why each one
// is allowed to be what it is. A baseline entry is cheap to add and
// impossible to add by accident: it is an edit to this file, in this
// directory, next to the paragraph above.
import { readFileSync, readdirSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import type { Violation } from "./rules";

const DEVICE_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const TESTS_DIR = join(DEVICE_ROOT, "emulator", "wasm", "tests");

/**
 * Files allowed to drive touch without a simulated controller, and why.
 *
 * "It is a feature-* file" is a real reason and it is AGENTS.md's own: a
 * clean-input pass is what makes a feature file a readable statement of
 * what the feature IS. It is only half an answer, so each of these names
 * the repro-touch-dropout-* file that is the other half, and a feature
 * file with no such partner is listed as owing one.
 */
export const CLEAN_INPUT_BASELINE: Record<string, string> = {
  "feature-four.ts":
    "statement of what Connect Four is; the dropout half is repro-touch-dropout-four-drop.ts",
  "feature-four-choice.ts":
    "statement of what the vs-human/vs-cpu choice screen is (four.c section 8); the dropout/jitter half is repro-touch-dropout-four-choice.ts",
  "feature-four-cpu.ts":
    "statement of what the cpu opponent does (four.c section 9: only moves on its own turn, only legal drops, beatable) - a behavioural/legality check, not a gesture-under-duress one; its own touches are the same clean gesture feature-four.ts already covers under both clean and dropout input",
  "feature-clock.ts":
    "statement of what the clock is; the dropout half is repro-touch-dropout-clock-set.ts",
  "feature-morpion.ts":
    "statement of what morpion is; the dropout half is repro-touch-dropout-morpion.ts",
  "feature-dino.ts":
    "statement of what dino's jump/restart state machine does; the dropout half is repro-touch-dropout-dino-jump.ts",
  "feature-bowling.ts":
    "statement of what the bowling app is; the dropout half is repro-touch-dropout-bowling-throw.ts",
  "feature-bowling-provisional-release.ts":
    "the assertions are about EXACT millisecond timing relative to RELEASE_GRACE_MS (nudge fires well before the grace elapses, the real launch fires only once it genuinely has) - a simulated dropout could itself trigger an early or late release and contaminate the very before/after-grace boundary under test. The dropout-side property this feature touches (a premature release under real controller weather) is already covered by tools/sweep-bowling-grace.ts's own flick scenario and by repro-touch-dropout-bowling-throw.ts's held-still/drag scenarios; this file is deliberately the clean, exact-timing half, not a second copy of either.",
  "feature-sketch-palette.ts":
    "statement of what the palette is; the dropout half is repro-touch-dropout-palette-open.ts",
  "feature-tables.ts":
    "statement of what multiplication-tables practice is; the dropout/jitter half is repro-touch-dropout-tables.ts",
  "feature-tables-calibration.ts":
    "the assertions are about EXACT recovered values - a synthetic tap landing a KNOWN, FIXED 30px below its own crosshair must produce a raw delta of exactly 30, a per-target median of exactly 30, and a fitted alpha within 1px of 30 with beta within 0.02 of zero. A simulated controller's dropouts and 80-250px jitter would themselves perturb every one of those numbers, which is precisely what the MEDIAN-of-five-taps and the least-squares fit exist to survive - contaminating the exact-recovery check this file is about, not testing something new. Calibration's own tap detector (tables_calib_tick() in tables.c) is a verbatim copy of the numpad's ARM_SAMPLES/ARM_MS/RELEASE_GRACE_MS gesture idiom, so its robustness against dropouts and jitter is already the property repro-touch-dropout-tables.ts proves for that exact mechanism; this file is deliberately the clean, exact-recovery half, not a second copy of it.",
  "repro-tables-loupe-flicker.ts":
    "the whole point is a PERFECTLY unchanged touch coordinate across many consecutive ticks (the owner's own scenario, a finger held still); a simulated controller's dropouts and position jitter would themselves change the coordinate tick to tick, defeating exactly the condition under test - same reasoning as repro-sketch-palette-animation-starved.ts's own exemption (a simulated stream would be adding the very variation the test is about the absence of). The gesture-under-duress half of this app's touch handling is already covered by repro-touch-dropout-tables.ts; this file is about a redraw decision inside one already-settled hover, not about whether the hover itself survives real controller weather.",
  "feature-breakout.ts":
    "statement of what the lives/game-over state machine does (breakout.c's own header, \"THREE LIVES\"); the touch it drives is the one narrow exception, the restart tap read only at LP_GAMEOVER_WAIT, and the dropout half is repro-touch-dropout-breakout-restart.ts",
  "repro-arena-not-zeroed.ts":
    "about the arena on app switch, not about a gesture: the touch here is a way to dirty state, and controller noise would only make the dirtying less certain",
  "repro-switch-input.ts":
    "about whether input reaches an app at all after a switch; a dropped report would be indistinguishable from the bug under test",
  "repro-timer-boot-pwr-running-and-alarm.ts":
    "button-collision reproduction; touch is incidental",
  "repro-timer-swallows-pwr-short-with-boot.ts":
    "button-collision reproduction; touch is incidental",
  "repro-ring-shrink-residue.ts":
    "a rendering-residue reproduction: the assertion is about pixels left behind by a known drag, and a dropped report changes which drag was made",
  "repro-sketch-palette-watchdog-reset.ts":
    "counts render cost per drained sample; the sample stream is the independent variable and must be exact",
  "repro-sketch-palette-animation-starved.ts":
    "the whole point is a window with ZERO samples in it; a simulated controller would be adding samples to a test about their absence",
  "repro-sketch-palette-pop-in-residue.ts":
    "residue after a known pop-in; same reason as repro-ring-shrink-residue.ts",
  "repro-sketch-palette-cell-gap.ts":
    "measures rendered pixel geometry against palette_cell_bounds()'s own formula; the touch position selects which cell settles where, and must land exactly, not somewhere a controller's own noise happened to leave it",
};

export interface LintResult {
  checked: number;
  throughTouchSim: number;
  baselined: number;
  violations: Violation[];
}

export function lintCleanInput(): LintResult {
  const files = readdirSync(TESTS_DIR).filter((f) => f.endsWith(".ts"));
  if (files.length === 0) throw new Error(`no test files under ${TESTS_DIR} - the lint has nothing to read`);

  let checked = 0;
  let throughTouchSim = 0;
  let baselined = 0;
  const violations: Violation[] = [];
  const seen = new Set<string>();

  for (const f of files) {
    const src = readFileSync(join(TESTS_DIR, f), "utf8");
    // Does it drive touch at all? Either the raw export or a wrapper
    // method named touch/tap/drag/swipe/hold.
    const drivesTouch = /emu_touch\b/.test(src) || /\.(touch|tap|swipe|dragTo|hold)\(/.test(src);
    if (!drivesTouch) continue;
    checked++;

    const usesSim = /from\s+"[^"]*touchsim"/.test(src) || /\bTouchSim\b/.test(src) ||
      /from\s+"[^"]*tools\/gate\/touch"/.test(src) || /\bTouchDriver\b/.test(src);
    if (usesSim) { throughTouchSim++; continue; }

    if (f in CLEAN_INPUT_BASELINE) {
      baselined++;
      seen.add(f);
      continue;
    }
    violations.push({
      rule: "clean-input",
      why: "a gesture proven only against clean input is a gesture proven against a controller this board does not have; the palette passed 22 clean-input checks and did not work in the owner's hands",
      see: "docs/decisions/0008-the-emulator-seam-is-in-the-wrong-place.md",
      detail: `emulator/wasm/tests/${f} drives touch without TouchSim. Either drive it through ` +
        `tools/gate/touch.ts (TOUCHSIM_HARDWARE_MEASURED), or add it to CLEAN_INPUT_BASELINE in ` +
        `tools/gate/cleaninput.ts with the reason it is an exception.`,
    });
  }

  // A baseline entry for a file that no longer exists is a stale
  // exemption, and stale exemptions are how a lint quietly stops
  // covering things.
  for (const name of Object.keys(CLEAN_INPUT_BASELINE)) {
    if (!files.includes(name)) {
      violations.push({
        rule: "clean-input",
        why: "an exemption for a file that is gone is an exemption nobody re-argued",
        see: "docs/decisions/0008-the-emulator-seam-is-in-the-wrong-place.md",
        detail: `CLEAN_INPUT_BASELINE names ${name}, which is not in emulator/wasm/tests/ any more: delete the entry`,
      });
    } else if (!seen.has(name) && files.includes(name)) {
      // It exists, and it did not need the exemption (it drives TouchSim
      // now, or stopped touching the glass). Same reasoning.
      violations.push({
        rule: "clean-input",
        why: "an exemption that is no longer needed reads as permission, and the next file copies it",
        see: "docs/decisions/0008-the-emulator-seam-is-in-the-wrong-place.md",
        detail: `CLEAN_INPUT_BASELINE exempts ${name}, which no longer needs it: delete the entry`,
      });
    }
  }

  return { checked, throughTouchSim, baselined, violations };
}

if (import.meta.main) {
  const r = lintCleanInput();
  console.log(`${r.checked} test files drive touch: ${r.throughTouchSim} through TouchSim, ${r.baselined} argued exceptions`);
  for (const v of r.violations) console.log(`FAIL  ${v.detail}`);
  if (r.violations.length > 0) process.exit(1);
}
