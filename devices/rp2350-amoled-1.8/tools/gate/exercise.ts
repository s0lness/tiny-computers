// Driving every app the same way, so that the rules in rules.ts apply to
// every app by construction rather than because a test author remembered.
//
// The stimuli below name no app. They are the physical things a hand does
// to a puck: tap it somewhere, drag across it, hold a finger down, click a
// button, shake it, put it down and walk away. Each one is delivered
// through TouchDriver, which means through the measured hardware
// controller profile (bug 7), and each one is checked on every tick it
// produces (bugs 1, 2, 3) plus once on the frame it settles into (bug 4).
//
// The two probes at the bottom are counterfactuals rather than scripts:
// they run the same prefix twice and compare the picture the app ends up
// with. That is the only way to ask "is this animation driven by the
// clock" (bug 5) and "did the animation leave residue" (bug 6) without
// knowing what the app is supposed to be drawing.

import { TOUCHSIM_DEFAULTS } from "../../emulator/src/constants";
import type { Device } from "./device";
import { BTN_BOOT, BTN_PWR, loadDevice } from "./device";
import type { Limits, Violation } from "./rules";
import { TickWatcher } from "./rules";
import { TouchDriver, seedFromName } from "./touch";

/**
 * Every gesture the gate makes stays this far from the panel edge, so that
 * a stroke's own width can never put the gate's OWN ink under the bezel
 * and make rule `bezel` accuse the app of something the gate did.
 */
export const GESTURE_INSET_PX = 48;

export interface Stimulus {
  label: string;
  run(d: TouchDriver, dev: Device): void;
}

export function commonStimuli(w: number, h: number, longPressMs: number): Stimulus[] {
  const i = GESTURE_INSET_PX;
  const cx = w / 2, cy = h / 2;
  const corners: [string, number, number][] = [
    ["top-left", i + 40, i + 40],
    ["top-right", w - i - 40, i + 40],
    ["bottom-left", i + 40, h - i - 40],
    ["bottom-right", w - i - 40, h - i - 40],
  ];

  const out: Stimulus[] = [
    { label: "tap centre", run: (d) => d.tap(cx, cy) },
    ...corners.map(([name, x, y]): Stimulus => ({ label: `tap ${name}`, run: (d) => d.tap(x, y) })),
    { label: "swipe left to right", run: (d) => d.swipe(i, cy, w - i, cy, 500) },
    { label: "swipe top to bottom", run: (d) => d.swipe(cx, i, cx, h - i, 500) },
    { label: "swipe diagonally", run: (d) => d.swipe(i, i, w - i, h - i, 600) },
    // The gesture that opened the palette, and the one a thumb makes by
    // accident: down, and then nothing for a second and a half.
    { label: "hold centre 1500ms", run: (d) => { d.hold(cx, cy, 1500); d.lift(); } },
    { label: "hold top-left 1500ms", run: (d) => { d.hold(i + 40, i + 40, 1500); d.lift(); } },
    // A drag that stops dead partway and resumes: the shape that makes a
    // dirty-rectangle calculation forget where it has been.
    {
      label: "drag, pause, drag on",
      run: (d) => {
        d.hold(i, cy, 100);
        d.dragTo(cx, cy, 300);
        d.hold(cx, cy, 400);
        d.dragTo(w - i, cy, 300);
        d.lift();
      },
    },
    { label: "BOOT click", run: (d, dev) => { dev.button(BTN_BOOT, true); d.idle(120); dev.button(BTN_BOOT, false); d.idle(200); } },
    { label: "PWR short press", run: (d) => d.pressButton(BTN_PWR, 200, longPressMs) },
    { label: "shake", run: (d) => { d.shake(); d.idle(200); } },
    { label: "left alone", run: (d) => d.idle(1500) },
  ];
  return out;
}

/**
 * The BOOT+PWR chord, which is how the menu is reached. It is a stimulus
 * like any other, but it changes which app is running, so it is applied
 * once at the end rather than mixed into the list above.
 */
export function chordStimulus(longPressMs: number): Stimulus {
  return {
    label: "BOOT+PWR chord (opens the menu)",
    run: (d, dev) => {
      dev.button(BTN_BOOT, true);
      d.idle(120);
      dev.buttonVerdict(BTN_PWR, true);
      d.idle(120);
      dev.button(BTN_BOOT, false);
      d.idle(400);
    },
  };
}

export interface AppReport {
  appIndex: number;
  appName: string;
  violations: Violation[];
  ticks: number;
  maxPushedPixels: number;
  maxPushes: number;
  // Which stimulus was the most expensive tick, for the budget line.
  peakContext: string;
  // What this app's enter() allocated from the arena (emu_abi.h's arena
  // oracle), read once the entry settle is over. Apps allocate only in
  // enter() (app.h), so this is the whole footprint.
  arenaBytes: number;
  arenaCapacity: number;
}

/**
 * Runs the whole stimulus list against one app, with every rule armed on
 * every tick.
 */
export async function exerciseApp(
  appIndex: number,
  appName: string,
  lim: Limits,
  longPressMs: number,
  extraStimuli: Stimulus[] = []
): Promise<AppReport> {
  const dev = await loadDevice({ skipFreshness: true });
  const watcher = new TickWatcher(dev, lim);
  const driver = new TouchDriver(dev, {
    seed: seedFromName(`gate:${appName}`),
    onTick: () => watcher.observe(),
  });

  watcher.setContext(`${appName}/enter`);
  dev.appSwitch(appIndex);
  // app.h: the runtime clears the framebuffer and pushes the WHOLE panel
  // once after enter() returns. That tick is a full panel by design.
  watcher.disarmBudgetForOneTick();
  driver.idle(400);
  watcher.checkSettledBezel();

  // Read the arena BEFORE the stimuli: the chord stimulus switches into the
  // menu, and a read after it would be the menu's footprint wearing this
  // app's name.
  const arenaBytes = dev.arenaUsed();
  const arenaCapacity = dev.arenaCapacity();

  const stimuli = [...commonStimuli(dev.panelW, dev.panelH, longPressMs), ...extraStimuli];
  for (const s of stimuli) {
    watcher.setContext(`${appName}/${s.label}`);
    s.run(driver, dev);
    // Let whatever it started finish, without touching the glass again.
    driver.idle(600);
    watcher.checkSettledBezel();
  }

  return {
    appIndex,
    appName,
    violations: watcher.violations,
    ticks: watcher.stats.ticks,
    maxPushedPixels: watcher.stats.maxPushedPixels,
    maxPushes: watcher.stats.maxPushes,
    peakContext: watcher.stats.peakContext,
    arenaBytes,
    arenaCapacity,
  };
}

/**
 * The menu's own arena footprint. It is not a g_apps[] entry (APP_INDEX_MENU
 * is -1), so the per-app loop never reads it, and it allocates like any app.
 */
export async function menuArena(): Promise<{ arenaBytes: number; arenaCapacity: number }> {
  const dev = await loadDevice({ skipFreshness: true });
  dev.appSwitch(-1); // APP_INDEX_MENU; the switch applies at the next tick
  dev.tick(1000);
  return { arenaBytes: dev.arenaUsed(), arenaCapacity: dev.arenaCapacity() };
}

/* ---- the two counterfactuals ----------------------------------------- */

const CLEAN_STILL = {
  ...TOUCHSIM_DEFAULTS,
  dropoutsEnabled: false,
  straysEnabled: false,
  positionJitterEnabled: false,
};

function fbCopy(dev: Device): Uint16Array {
  return new Uint16Array(dev.fb());
}

function firstDifference(a: Uint16Array, b: Uint16Array, w: number): { i: number; x: number; y: number; count: number } | null {
  let count = 0;
  let firstI = -1;
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) {
      count++;
      if (firstI < 0) firstI = i;
    }
  }
  if (firstI < 0) return null;
  return { i: firstI, x: firstI % w, y: (firstI / w) | 0, count };
}

/**
 * Bug 5: an animation driven by touch samples rather than by the clock.
 *
 * A real controller reports the SAME coordinate about sixty times before
 * producing a new one (decision 0008), so a finger held still is a stream
 * of identical reports, and an app that quietly depends on their arrival
 * looks fine on a bench and freezes on a board that drops contact. The
 * counterfactual: hold still, then either keep the repeats coming or stop
 * them dead. The picture the app arrives at must be the same.
 *
 * DELIBERATELY CLEAN INPUT, which is the exception bug 7's rule asks to be
 * argued for: this probe isolates ONE variable, the arrival of redundant
 * reports. Dropouts and jitter would change the reports' CONTENT, and then
 * a difference between the two runs would prove nothing.
 */
export async function probeClockDriven(
  appIndex: number,
  appName: string,
  lim: Limits,
  holdAt: { x: number; y: number },
  holdMs: number,
  windowMs: number
): Promise<Violation[]> {
  const run = async (repeatsAfterHold: boolean): Promise<Uint16Array> => {
    const dev = await loadDevice({ skipFreshness: true });
    const d = new TouchDriver(dev, { profile: CLEAN_STILL, seed: 1 });
    dev.appSwitch(appIndex);
    d.idle(400);
    d.hold(holdAt.x, holdAt.y, holdMs);
    if (repeatsAfterHold) d.hold(holdAt.x, holdAt.y, windowMs);
    else d.idleNoSamples(windowMs);
    return fbCopy(dev);
  };

  const withRepeats = await run(true);
  const withoutRepeats = await run(false);
  const diff = firstDifference(withRepeats, withoutRepeats, lim.panelW);
  if (diff === null) return [];
  return [{
    rule: "clock-driven",
    why: "a finger held still produces about sixty identical reports per new position, and stops producing them the instant the controller drops contact; an app whose picture depends on their arrival freezes on hardware and never on a bench",
    see: "docs/decisions/0008-the-emulator-seam-is-in-the-wrong-place.md",
    detail: `${appName}: holding at (${holdAt.x},${holdAt.y}) for ${holdMs}ms then ${windowMs}ms more, ` +
      `repeating the same report vs sending nothing gives ${diff.count} different pixels, first at (${diff.x},${diff.y})`,
  }];
}

/**
 * Bug 6: an animation that left residue, so cells overlapped after
 * settling.
 *
 * The assertion the owner asked for is "after an animation completes, the
 * framebuffer must equal what the settled state alone would produce". The
 * settled state alone is not something a general checker can construct, so
 * this constructs the next best thing and it turns out to be the same
 * thing: run the settle window at a coarse tick cadence, so the animation
 * has almost no intermediate frames to draw, and compare against the same
 * window run at 60Hz. A clock-driven app computes the same final picture
 * either way. An app that leaves residue behind its intermediate frames
 * does not, and the difference IS the residue.
 *
 * Both runs use the same input, the same seed and the same wall-clock
 * timestamps at every boundary. Only the number of ticks between them
 * differs.
 */
export async function probeSettleCadence(
  appIndex: number,
  appName: string,
  lim: Limits,
  holdAt: { x: number; y: number },
  holdMs: number,
  settleMs: number
): Promise<Violation[]> {
  const run = async (coarse: boolean): Promise<Uint16Array> => {
    const dev = await loadDevice({ skipFreshness: true });
    const d = new TouchDriver(dev, { profile: CLEAN_STILL, seed: 1 });
    dev.appSwitch(appIndex);
    d.idle(400);
    d.hold(holdAt.x, holdAt.y, holdMs);
    d.releaseSilently();
    if (coarse) {
      // Three ticks spanning the same window: enough for a state machine
      // to advance, nowhere near enough to animate frame by frame.
      const end = d.now + settleMs;
      d.tickAt(d.now + settleMs / 3);
      d.tickAt(d.now + (2 * settleMs) / 3);
      d.tickAt(end);
    } else {
      d.idleNoSamples(settleMs);
    }
    return fbCopy(dev);
  };

  const fine = await run(false);
  const coarse = await run(true);
  const diff = firstDifference(fine, coarse, lim.panelW);
  if (diff === null) return [];
  return [{
    rule: "settles-the-same",
    why: "once an animation has finished, the picture must be what the settled state alone would draw; if it also depends on how many intermediate frames were rendered, those frames left something behind",
    see: "emulator/wasm/tests/repro-sketch-palette-pop-in-residue.ts",
    detail: `${appName}: after holding at (${holdAt.x},${holdAt.y}) for ${holdMs}ms and settling for ${settleMs}ms, ` +
      `settling at 60Hz and settling in 3 ticks differ by ${diff.count} pixels, first at (${diff.x},${diff.y})`,
  }];
}
