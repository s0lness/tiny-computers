// What's left here after the rewrite (see docs/decisions/0003 and the ABI
// header, emulator/wasm/emu_abi.h): panel size, pixel format, buttons,
// sensors and apps all now come from the firmware's own emu_device()
// descriptor at runtime (see wasm.ts), because this emulator is generic
// across devices and must not hardcode any of it.
//
// What's genuinely ours: the touch-controller defect simulation. There is
// no ABI for "how a real touch panel misbehaves" because that is not part
// of any firmware; it is this emulator's own model of what a physical
// controller does that a mouse never does (see touchsim.ts).

export interface TouchSimConfig {
  reportRateHz: number;
  dropoutsEnabled: boolean;
  dropoutsPerSec: number;
  straysEnabled: boolean;
  straysPerSec: number;
}

// These are the rates the simulation uses ONCE the master "simulate
// controller defects" toggle is switched on (default off, see main.ts).
// Numbers carried over from the original rp2350-amoled-1.8 emulator, which
// measured them against the real FT3168: "1 to 3 per second while
// drawing" for dropouts.
export const TOUCHSIM_DEFAULTS: TouchSimConfig = {
  reportRateHz: 60,
  dropoutsEnabled: true,
  dropoutsPerSec: 2,
  straysEnabled: true,
  straysPerSec: 0.2,
};

// Off by default: a clean, unthrottled pointer pass-through exercises none
// of the firmware's dropout-bridging or stray-rejection code, which is
// exactly why this exists as an opt-in rather than always-on (see
// emu_abi.h: "off by default, and clearly labelled when on").
export const TOUCH_DEFECTS_DEFAULT = false;

// How long a push-rectangle outline stays visible in the overlay before
// fully fading (see overlay.ts). Not part of any ABI; a UI choice.
export const PUSH_FADE_MS = 400;

// Recorder ring-buffer cap (see recorder.ts). At a steady 60Hz tick rate
// this is a little over 13 minutes of full-rate history, which comfortably
// covers "what just happened" without growing without bound in a session
// left running overnight.
export const TRACE_MAX_EVENTS = 50000;
