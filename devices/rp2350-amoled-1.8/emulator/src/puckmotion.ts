// A visual model of inertia for the puck's on-screen position. When the
// browser window itself is shaken, or a shake sensor event fires from a
// button/keyboard press with no real window motion to derive from, the
// puck displaces on screen and springs back the way a loose object would
// react to being carried in a shaken hand: pushed against the direction of
// travel, lagging, overshooting, and settling once the motion stops.
//
// This is a hand tuned visual effect, not a physics simulation, and the
// numbers below (stiffness, damping, drag gain) were picked by eye for
// "reads as inertia", not measured from anything. It must never be read as
// more than that: nothing here computes or sends an acceleration value.
// The ABI's shake sensor is an EVENT (emu_sensor_event), not a vector, and
// the firmware never receives any of the offsets this class produces. What
// the page shows and what the firmware receives share a trigger, not data.

function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}

export class PuckMotion {
  offsetX = 0;
  offsetY = 0;

  private lastNow: number | null = null;
  private lastWinX = 0;
  private lastWinY = 0;
  private velX = 0; // the spring's own velocity, px/s
  private velY = 0;

  private readonly stiffness = 140; // higher: snaps back to centre faster
  private readonly damping = 12; // higher: less oscillation/overshoot
  private readonly dragGain = 0.12; // how much window speed displaces the target
  private readonly maxOffset = 26; // px: caps how far a hard shake pushes the puck

  // Called every frame with the window's current screen position. With no
  // real motion (window stationary), the target collapses to 0 and any
  // residual spring velocity (from a prior impulse()) simply relaxes out,
  // so a single call site covers both "window is being shaken" and
  // "settling back down" without a separate mode.
  tick(screenX: number, screenY: number, nowMs: number): void {
    if (this.lastNow === null) {
      this.lastNow = nowMs;
      this.lastWinX = screenX;
      this.lastWinY = screenY;
      return;
    }
    const dtSec = clamp((nowMs - this.lastNow) / 1000, 0, 0.05); // caps a huge dt (backgrounded tab) from launching the spring
    this.lastNow = nowMs;

    const winVx = dtSec > 0 ? (screenX - this.lastWinX) / dtSec : 0;
    const winVy = dtSec > 0 ? (screenY - this.lastWinY) / dtSec : 0;
    this.lastWinX = screenX;
    this.lastWinY = screenY;

    const targetX = clamp(-winVx * this.dragGain, -this.maxOffset, this.maxOffset);
    const targetY = clamp(-winVy * this.dragGain, -this.maxOffset, this.maxOffset);

    const ax = (targetX - this.offsetX) * this.stiffness - this.velX * this.damping;
    const ay = (targetY - this.offsetY) * this.stiffness - this.velY * this.damping;
    this.velX += ax * dtSec;
    this.velY += ay * dtSec;
    this.offsetX += this.velX * dtSec;
    this.offsetY += this.velY * dtSec;
  }

  // A kick with no real window motion behind it (a button or keyboard
  // shake): nudges the spring's own velocity, same as flicking one end of
  // it, so it still overshoots and settles the same way tick() would drive
  // it, purely for feedback that the press registered.
  impulse(dx: number, dy: number): void {
    this.velX += dx;
    this.velY += dy;
  }
}
