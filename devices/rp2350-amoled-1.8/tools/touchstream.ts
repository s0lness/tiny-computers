// Turns a gesture (a hold, a stroke) into the sequence of touch REPORTS a
// controller would produce for it, timestamped, ready to be injected over
// devlink one report at a time.
//
// WHY THIS FILE EXISTS AT ALL. `tools/dev.ts` could `tap` and it could
// `draw`, and both sent their points as fast as the link allowed. Neither
// could hold a finger still, so no gesture whose meaning is in its DURATION
// could be driven from the host: the sketchpad's palette (a still contact
// held for LONG_PRESS_MS = 550ms), Connect Four's sustained press and
// release, the menu's launch-on-release. The palette shipped broken to the
// board three times while the only instrument for it was the owner's thumb
// and a sentence describing what he saw.
//
// A HOLD IS NOT ONE COMMAND, IT IS A STREAM OF REPORTS. This is the whole
// design, and getting it wrong would make the tool lie. Firmware never sees
// "a finger is down"; it sees samples arriving in a queue
// (firmware/runtime/sensors.h). A single injected DOWN is not a held
// contact, it is one sample - and then silence, which
// firmware/apps/sketch.c's LIFT_DEBOUNCE_MS reads as a lift. A held contact
// is the same coordinate reported again, and again, for as long as the
// finger is there. So this file's output is a list of reports on a clock,
// not a list of gesture verbs.
//
// TWO KINDS OF TRUTH, AND BOTH ARE WANTED (see the `realism` option):
//
//   "clean"     one report per period, every period, exact coordinates,
//               no defects. Answers "does this gesture work at all", and
//               answers it the same way twice.
//   "hardware"  the same gesture run through the emulator's own TouchSim
//               under TOUCHSIM_HARDWARE_MEASURED - the profile measured on
//               THIS panel (dropouts 34/s, position jitter episodes of
//               80-250px lasting one to three reports; see
//               emulator/src/constants.ts for the sessions behind each
//               number). Answers "does this gesture survive this panel",
//               which is the question every broken palette round has
//               actually turned on.
//
// Clean is the default. A realistic run that fails has two candidate
// causes (the gesture, or the weather), a clean one has one; and a tool
// whose default output is nondeterministic is a bad instrument. `--real`
// is one word away, and it is seeded, so a failure replays exactly.
//
// THE SAME MODEL AS THE EMULATOR, ON PURPOSE. The defect model is imported
// from emulator/src/touchsim.ts rather than reimplemented here, and the
// numbers from emulator/src/constants.ts rather than retyped. Two
// implementations of "what this panel does" would agree on the day the
// second was written and never again (decision 0003's argument, one level
// down: decision 0008). It also means the differential harness
// (puck/harness) replays the same weather into the emulator and the board.
//
// WHAT THIS CANNOT REPRODUCE, and it matters. The real FT3168 repeats one
// coordinate about sixty times before producing a new one: core1 re-reads
// the controller at roughly 1.4kHz while the controller itself only
// refreshes at ~60Hz, so the sketchpad drains thousands of samples per
// second (measured: haveTouch 88977 against newReport 1399 in one
// session). Injection cannot: every report here is one devlink command
// down a 115200-baud line, and INJECT_Q_CAP is 8 deep with a silent
// drop-on-full policy (firmware/runtime/sensors.c), so a burst above what
// core0's main loop drains is lost with no error anywhere. This file
// therefore reports at the rate the CONTROLLER refreshes (60Hz), not at
// the rate core1 pushes samples - the same simplification TouchSim makes,
// which is why the emulator and the board see the same stream. The
// consequence to keep in mind when reading a "hardware" run: 34 dropouts
// per second land as a fraction of 60 reports rather than as isolated
// losses inside a dense stream, so contact is reported absent more often
// than the real panel does it. It is harsher than the panel on that one
// axis, and identical to what the gate and the emulator test against.

import { TouchSim } from "../emulator/src/touchsim";
import { TOUCHSIM_HARDWARE_MEASURED } from "../emulator/src/constants";
import type { TouchSimConfig } from "../emulator/src/constants";

export type Realism = "clean" | "hardware";

// One controller report. `fingers` is 0 or 1: this panel only ever has one.
// `atMs` is milliseconds from the start of the gesture, which is what the
// sender paces against - the firmware timestamps each injected sample with
// its own clock on arrival (sensors_inject_touch()), so the host's pacing
// IS the cadence the app sees.
export interface TouchReport {
  atMs: number;
  fingers: 0 | 1;
  x: number;
  y: number;
}

export interface StreamOptions {
  realism?: Realism;
  /** Report rate in Hz. Defaults to the profile's own (60). */
  rateHz?: number;
  /** Seed for the defect model, so a "hardware" run replays exactly. */
  seed?: number;
  panelW: number;
  panelH: number;
  /**
   * Zero-finger reports appended after the lift. Not strictly needed on
   * this board (core1 pushes an idle heartbeat every 5ms whether or not
   * anyone is injecting), but it makes an injected gesture self-contained:
   * the lift still lands if one UP is lost, and an emulator replaying the
   * same report list sees the same tail. Must exceed sketch.c's
   * LIFT_DEBOUNCE_MS (220ms) to be worth anything.
   */
  quietTailMs?: number;
}

// mulberry32. Deliberately not shared with tools/gate/touch.ts's copy of
// the same eight lines: that file is the EMULATOR-side twin of this one
// (same profile, same seeding discipline, different sink), and the two
// arrived independently. If they are ever refactored into one place, this
// is the note that says they should be.
export function seededRng(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// Where the finger actually is at time t, which is a different question
// from what the controller reports it as. Every gesture below is written as
// one of these and then run through the same sampler.
export type PointerPath = (tMs: number) => { down: boolean; x: number; y: number };

export const HARDWARE_PROFILE: TouchSimConfig = TOUCHSIM_HARDWARE_MEASURED;

function sample(path: PointerPath, durationMs: number, opts: StreamOptions): TouchReport[] {
  const realism = opts.realism ?? "clean";
  const rateHz = opts.rateHz ?? HARDWARE_PROFILE.reportRateHz;
  const periodMs = 1000 / rateHz;
  const quietTailMs = opts.quietTailMs ?? 0;
  const total = durationMs + quietTailMs;

  const out: TouchReport[] = [];

  if (realism === "clean") {
    for (let t = 0; t <= total; t += periodMs) {
      const p = path(t);
      out.push({
        atMs: t,
        fingers: p.down ? 1 : 0,
        x: Math.round(p.x),
        y: Math.round(p.y),
      });
    }
    return out;
  }

  // Same construction as tools/gate/touch.ts's TouchDriver.step(): advance
  // the clock by exactly one report period, set the true pointer, ask the
  // simulated controller what it would have reported. The profile's rate is
  // overridden by rateHz so TouchSim's internal "have I reached the next
  // report instant" test agrees with the loop driving it.
  const sim = new TouchSim(
    { ...HARDWARE_PROFILE, reportRateHz: rateHz },
    opts.panelW,
    opts.panelH,
    seededRng(opts.seed ?? 1)
  );
  for (let t = 0; t <= total; t += periodMs) {
    const p = path(t);
    sim.setPointer(p.down, p.x, p.y);
    const r = sim.poll(t);
    out.push({
      atMs: t,
      fingers: r.fingers > 0 ? 1 : 0,
      x: Math.round(r.x),
      y: Math.round(r.y),
    });
  }
  return out;
}

/** Contact at one point, held for holdMs, then lifted. */
export function holdStream(x: number, y: number, holdMs: number, opts: StreamOptions): TouchReport[] {
  const path: PointerPath = (t) => ({ down: t <= holdMs, x, y });
  return sample(path, holdMs, opts);
}

/**
 * A stroke through `points`, spread over totalMs and resampled at the
 * report rate, so the board sees the sample density a finger produces
 * rather than one report per listed vertex. `points` are the path's
 * corners; travel between two of them is linear in time (constant speed
 * per segment, not overall - a real hand does not slow down because a
 * later segment is long, and matching arc length would change what an
 * existing invocation's points mean).
 */
export function strokeStream(
  points: { x: number; y: number }[],
  totalMs: number,
  opts: StreamOptions
): TouchReport[] {
  if (points.length === 0) throw new Error("strokeStream needs at least one point");
  if (points.length === 1) return holdStream(points[0].x, points[0].y, totalMs, opts);
  const segs = points.length - 1;
  const segMs = totalMs / segs;
  const path: PointerPath = (t) => {
    if (t >= totalMs) return { down: t <= totalMs, x: points[segs].x, y: points[segs].y };
    const i = Math.min(segs - 1, Math.floor(t / segMs));
    const k = (t - i * segMs) / segMs;
    return {
      down: true,
      x: points[i].x + (points[i + 1].x - points[i].x) * k,
      y: points[i].y + (points[i + 1].y - points[i].y) * k,
    };
  };
  return sample(path, totalMs, opts);
}

/**
 * The wire form of one report. DOWN and MOVE are the same firmware hook
 * (both are sensors_inject_touch(1, x, y) - see runtime.c), and UP is
 * sensors_inject_touch(0, 0, 0), which is exactly what core1 pushes when
 * the controller reports zero fingers. That is why a mid-gesture dropout is
 * spelled UP here and is not a lie: at the queue, a dropout and a lift are
 * the same sample. Which one it was is decided downstream, by how long the
 * zeros last against sketch.c's LIFT_DEBOUNCE_MS.
 */
export function reportToCommand(r: TouchReport, isFirstContact: boolean): string {
  if (r.fingers === 0) return "UP";
  return `${isFirstContact ? "DOWN" : "MOVE"} ${r.x} ${r.y}`;
}

export function reportsToCommands(reports: TouchReport[]): string[] {
  let seenContact = false;
  return reports.map((r) => {
    const cmd = reportToCommand(r, !seenContact);
    if (r.fingers === 1) seenContact = true;
    return cmd;
  });
}

/** How many reports of each kind, for the one-line summary a run prints. */
export function summarise(reports: TouchReport[]): { contact: number; zero: number; gapsMs: number[] } {
  let contact = 0;
  let zero = 0;
  const gapsMs: number[] = [];
  let lastContactAt: number | null = null;
  for (const r of reports) {
    if (r.fingers === 1) {
      contact++;
      if (lastContactAt !== null) gapsMs.push(r.atMs - lastContactAt);
      lastContactAt = r.atMs;
    } else {
      zero++;
    }
  }
  return { contact, zero, gapsMs };
}
