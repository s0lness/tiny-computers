// Detects a shake by watching the browser WINDOW move: several rapid
// direction reversals within a short rolling window, the same shape the
// firmware's own IMU-based detector uses (firmware/runtime/sensors.c):
// "detection requires several jolts inside a window, not one threshold
// crossing, so a social shake or a knock cannot fire it." One drag-past of
// the window must not fire this either.
//
// Real detector's numbers, read from firmware/runtime/sensors.c:
//   JOLT_DEV_MG 900.0f   (accelerometer deviation; no pixel equivalent)
//   JOLT_WINDOW_MS 700   (rolling window a jolt must fall inside)
//   JOLT_MIN_COUNT 4     (jolts needed inside the window to fire)
//   ERASE_COOLDOWN_MS 1200
//   JOLT_MAX 16          (ring buffer cap on tracked jolt timestamps)
// window/count/cooldown are matched exactly below; joltDevPx is this
// emulator's own threshold (there is no accelerometer here), tuned by feel.
//
// VERIFIED: window.screenX/screenY update live and the page keeps getting
// animation frames while the OS repositions the window (checked here by
// moving the window via CDP's Browser.setWindowBounds in a tight loop
// while a page-side rAF sampler ran: 55 samples over a 900ms move
// sequence, ~60fps throughout, no stalls).
//
// NOT VERIFIED, and worth being honest about: whether a REAL,
// mouse-on-titlebar drag behaves the same on Windows. That specific
// gesture enters a different code path (Windows' synchronous
// WM_ENTERSIZEMOVE modal loop), which is a long-documented source of
// renderer main-thread stalls in Chromium on Windows, and cannot be
// exercised by anything CDP can drive. If the jolt count in the readout
// below never moves while actually shaking the window, that is exactly
// what a stalled main thread would look like from here, and the shake
// sensor's plain button + keyboard shortcut (sensors.ts) is the fallback
// that does not depend on the window's message loop at all.

export interface WindowShakeConfig {
  joltDevPx: number;
  joltWindowMs: number;
  joltMinCount: number;
  cooldownMs: number;
}

export const WINDOW_SHAKE_DEFAULTS: WindowShakeConfig = {
  joltDevPx: 12,
  joltWindowMs: 700,
  joltMinCount: 4,
  cooldownMs: 1200,
};

const JOLT_MAX = 16;

export class WindowShakeDetector {
  cfg: WindowShakeConfig;
  lastJoltCount = 0; // for the on-page debug readout

  private lastX: number | null = null;
  private lastY: number | null = null;
  private lastDirX = 0;
  private lastDirY = 0;
  private joltTimes: number[] = [];
  private cooldownUntil = 0;

  constructor(cfg: WindowShakeConfig = WINDOW_SHAKE_DEFAULTS) {
    this.cfg = cfg;
  }

  // Called every frame with the window's current screen position. Returns
  // true exactly on the frame a shake is accepted (mirrors
  // sensors_erase_seq() being bumped once per accepted shake).
  // `suppressed` mirrors g_fingerDownShared: no shake while a finger (or a
  // mouse standing in for one) is down on the panel.
  poll(screenX: number, screenY: number, nowMs: number, suppressed: boolean): boolean {
    if (this.lastX === null || this.lastY === null) {
      this.lastX = screenX;
      this.lastY = screenY;
      return false;
    }
    const dx = screenX - this.lastX;
    const dy = screenY - this.lastY;
    this.lastX = screenX;
    this.lastY = screenY;

    const dirX = Math.sign(dx);
    const dirY = Math.sign(dy);
    const dist = Math.hypot(dx, dy);
    const reversed = (dirX !== 0 && dirX === -this.lastDirX) || (dirY !== 0 && dirY === -this.lastDirY);
    if (dirX !== 0) this.lastDirX = dirX;
    if (dirY !== 0) this.lastDirY = dirY;

    if (reversed && dist >= this.cfg.joltDevPx) {
      this.joltTimes = this.joltTimes.filter((t) => nowMs - t <= this.cfg.joltWindowMs);
      this.joltTimes.push(nowMs);
      if (this.joltTimes.length > JOLT_MAX) this.joltTimes.shift();
    } else {
      this.joltTimes = this.joltTimes.filter((t) => nowMs - t <= this.cfg.joltWindowMs);
    }
    this.lastJoltCount = this.joltTimes.length;

    if (this.joltTimes.length >= this.cfg.joltMinCount && nowMs >= this.cooldownUntil && !suppressed) {
      this.cooldownUntil = nowMs + this.cfg.cooldownMs;
      this.joltTimes = [];
      this.lastJoltCount = 0;
      return true;
    }
    return false;
  }
}
