// Bug 7: the emulator was kinder than the hardware.
//
// Gesture tests passed against clean input and failed on a panel that
// repeats coordinates and drops contact 34 times a second. TouchSim has
// been able to produce that weather for a while; what was missing was any
// reason for a new gesture to be driven through it. This module is that
// reason: it is the ONLY gesture vocabulary the gate has, and it always
// goes through TOUCHSIM_HARDWARE_MEASURED.
//
// SEEDED, so the gate does not flap. A gate that fails one run in twenty
// gets re-run until it agrees, which is worse than not having it. Each
// scenario derives its stream from a fixed seed and the scenario's own
// name, so two apps do not get the identical dropout pattern and a rerun
// gets the identical one.
//
// It owns the clock as well as the pointer, because the two are one thing:
// a controller reports at 60Hz whatever the host does, and a gesture that
// advances the clock without producing reports is a gesture the board
// cannot make.
import { TOUCHSIM_HARDWARE_MEASURED } from "../../emulator/src/constants";
import type { TouchSimConfig } from "../../emulator/src/constants";
import { TouchSim } from "../../emulator/src/touchsim";
import type { Device } from "./device";

// mulberry32: 32 bits of state, uniform enough for a Bernoulli draw, and
// short enough to read. Nothing here needs cryptographic quality; it needs
// to produce the same sequence twice.
export function seededRng(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

export function seedFromName(name: string, salt = 0x9e3779b9): number {
  let h = salt >>> 0;
  for (let i = 0; i < name.length; i++) {
    h = Math.imul(h ^ name.charCodeAt(i), 0x01000193) >>> 0;
  }
  return h >>> 0;
}

export interface TouchDriverOptions {
  /** Defaults to TOUCHSIM_HARDWARE_MEASURED. Overriding it is a decision. */
  profile?: TouchSimConfig;
  seed: number;
  /** Wall clock the gesture starts at. */
  startMs?: number;
  /** Called once per tick, after emu_tick returns. */
  onTick?: (nowMs: number) => void;
}

/**
 * Drives a Device the way the board drives it: a controller polled at a
 * fixed rate, one emu_tick per report period, with the clock advancing in
 * step. Every method is a physical action, not an ABI call.
 */
export class TouchDriver {
  readonly sim: TouchSim;
  readonly stepMs: number;
  now: number;
  private dev: Device;
  private onTick?: (nowMs: number) => void;
  private down = false;
  private x = 0;
  private y = 0;

  constructor(dev: Device, opts: TouchDriverOptions) {
    const profile = opts.profile ?? TOUCHSIM_HARDWARE_MEASURED;
    this.dev = dev;
    this.sim = new TouchSim(profile, dev.panelW, dev.panelH, seededRng(opts.seed));
    this.stepMs = 1000 / profile.reportRateHz;
    this.now = opts.startMs ?? 1000;
    this.onTick = opts.onTick;
  }

  /** One controller report period: poll, deliver, tick. */
  step(): void {
    this.now += this.stepMs;
    this.sim.setPointer(this.down, this.x, this.y);
    const r = this.sim.poll(this.now);
    this.dev.touchRaw(r.fingers > 0, r.x, r.y);
    this.dev.tick(this.now);
    this.onTick?.(this.now);
  }

  /** Nothing touching the glass, for `ms` of clock. Strays still happen. */
  idle(ms: number): void {
    this.down = false;
    for (let t = 0; t < ms; t += this.stepMs) this.step();
  }

  /**
   * Ticks WITHOUT ever calling emu_touch. Not a gesture: this is the probe
   * for bug 5 (an animation driven by touch samples rather than by the
   * clock). It is what a real main loop does while the controller has
   * nothing new to say, which on this panel is most of the time.
   *
   * Walks in whole stepMs-cadence report periods, same as every other
   * `for (t=0; t<ms; t+=stepMs)` driver method here (`idle`, `hold`), and
   * therefore OVERSHOOTS past `ms` by up to one stepMs whenever stepMs does
   * not divide it evenly - 60Hz's stepMs is 16.667ms, which does not divide
   * the gate's own 700ms probe window, so this lands at 716.7ms rather than
   * 700. That is deliberately consistent with `hold()`, which
   * `probeClockDriven` compares this against and which overshoots by the
   * identical amount for the identical reason: the two must reach the same
   * absolute clock, and matching an existing method's shape is what
   * guarantees that without hand-tuning a second constant. See
   * `probeSettleCadence` (exercise.ts) for the probe that instead needed
   * ITS OWN "coarse" branch taught to overshoot the same way, because that
   * probe's other branch is this function, not `hold()`.
   */
  idleNoSamples(ms: number): void {
    for (let t = 0; t < ms; t += this.stepMs) this.tickAt(this.now + this.stepMs);
  }

  /** One tick at an absolute time, no touch call. Used by the cadence probe. */
  tickAt(nowMs: number): void {
    this.now = nowMs;
    this.dev.tick(this.now);
    this.onTick?.(this.now);
  }

  /** Lifts the finger without spending any clock on it. */
  releaseSilently(): void {
    this.down = false;
  }

  /** Finger down at (x, y), held for `ms`. */
  hold(x: number, y: number, ms: number): void {
    this.down = true;
    this.x = x;
    this.y = y;
    for (let t = 0; t < ms; t += this.stepMs) this.step();
  }

  /** Down, brief contact, up. `ms` is contact time, not including the lift. */
  tap(x: number, y: number, ms = 120): void {
    this.hold(x, y, ms);
    this.lift();
  }

  /** Straight-ish drag from a to b over `ms`, then still down. */
  dragTo(x1: number, y1: number, ms: number): void {
    const x0 = this.x;
    const y0 = this.y;
    const steps = Math.max(1, Math.round(ms / this.stepMs));
    this.down = true;
    for (let i = 1; i <= steps; i++) {
      const k = i / steps;
      this.x = x0 + (x1 - x0) * k;
      this.y = y0 + (y1 - y0) * k;
      this.step();
    }
  }

  /** Press at a, drag to b, release. The most common real gesture. */
  swipe(x0: number, y0: number, x1: number, y1: number, ms = 400): void {
    this.hold(x0, y0, this.stepMs * 2);
    this.dragTo(x1, y1, ms);
    this.lift();
  }

  /** Contact ends, plus enough quiet for any lift debounce to settle. */
  lift(quietMs = 400): void {
    this.down = false;
    for (let t = 0; t < quietMs; t += this.stepMs) this.step();
  }

  /** A button press with a duration, and the verdict the PMIC would give. */
  pressButton(index: number, ms: number, longPressMs?: number): void {
    this.dev.button(index, true);
    for (let t = 0; t < ms; t += this.stepMs) this.step();
    if (longPressMs !== undefined) this.dev.buttonVerdict(index, ms >= longPressMs);
    this.dev.button(index, false);
    this.step();
  }

  shake(): void {
    this.dev.sensorEvent(0);
    this.step();
  }
}
