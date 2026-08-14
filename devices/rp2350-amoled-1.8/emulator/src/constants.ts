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
  // Position noise while contact IS held - a real, stationary finger still
  // occasionally reports a position far from where it actually is. Added
  // 2026-08-14 (see touchsim.ts's own header comment on where this number
  // comes from): a TOUCH_POLL_SELFTEST hardware session with the owner's
  // finger held deliberately still measured splits=10 (confirmed jumps,
  // sketch.c's own counter) in 40 seconds, which the emulator's touch input
  // could not reproduce at all before this field existed - dropouts and
  // strays alone never move the REPORTED position while contact is down,
  // only whether contact is reported at all. Off by default, same reason
  // dropoutsEnabled/straysEnabled default off (see TOUCH_DEFECTS_DEFAULT):
  // a filter that never sees noisy position input is not tested (decision
  // 0008's own line, now true of movement noise as much as dropouts).
  positionJitterEnabled: boolean;
  positionJitterPerSec: number;   // rate of a jitter EPISODE starting (Bernoulli, same model as dropouts/strays)
  positionJitterMinPx: number;    // episode displacement magnitude, uniform between min and max
  positionJitterMaxPx: number;
  positionJitterMaxHoldReports: number; // an episode holds (roughly) the SAME wrong spot for 1..this many
                                         // consecutive reports before reverting - see touchsim.ts's refresh()
                                         // for why holding matters: a lone far reading is a "glitch" to
                                         // sketch.c, but a SECOND reading landing near the first is what
                                         // gets "confirmed" into a "split" (stroke_end + stroke_begin).
}

// These are the rates the simulation uses ONCE the master "simulate
// controller defects" toggle is switched on (default off, see main.ts).
// Numbers carried over from the original rp2350-amoled-1.8 emulator, which
// measured them against the real FT3168: "1 to 3 per second while
// drawing" for dropouts. positionJitter* stays disabled here too - see its
// own comment above; emulator/wasm/tests/repro-touch-dropout-palette-
// open.ts is what turns it on and calibrates its rate against the
// hardware's own splits=10-per-40s reading, not this default.
export const TOUCHSIM_DEFAULTS: TouchSimConfig = {
  reportRateHz: 60,
  dropoutsEnabled: true,
  dropoutsPerSec: 2,
  straysEnabled: true,
  straysPerSec: 0.2,
  positionJitterEnabled: false,
  positionJitterPerSec: 0,
  positionJitterMinPx: 80,
  positionJitterMaxPx: 250,
  positionJitterMaxHoldReports: 3,
};

// Off by default: a clean, unthrottled pointer pass-through exercises none
// of the firmware's dropout-bridging or stray-rejection code, which is
// exactly why this exists as an opt-in rather than always-on (see
// emu_abi.h: "off by default, and clearly labelled when on").
export const TOUCH_DEFECTS_DEFAULT = false;

// What the REAL FT3168 on this board was measured doing, as opposed to
// TOUCHSIM_DEFAULTS above, which is what a controller is imagined doing.
// Every number here has a hardware session behind it:
//
//   dropoutsPerSec 34    798 lost contacts over roughly 23 seconds of
//                        touch-down time, TOUCH_POLL_SELFTEST, 2026-08-14.
//                        Seventeen times TOUCHSIM_DEFAULTS' guess of 2.
//   positionJitter*      splits=10 in 40 seconds with a finger held
//                        deliberately still: the reported centroid jumping
//                        80-250px away and STAYING there for a report or
//                        three. 1.5 episodes/sec is what reproduces that
//                        splits figure (see
//                        emulator/wasm/tests/repro-touch-dropout-palette-
//                        open.ts, where it was calibrated).
//   straysEnabled false  contact-down defects are what this profile is
//                        for; strays (a contact reported while nothing is
//                        touching) are a separate axis and are left to the
//                        test that wants them, so a failure here has one
//                        cause and not two.
//
// It exists as ONE named constant because it was previously spelled out by
// hand in four different test files, which is one edit away from four
// different opinions about what the hardware does. This is the profile the
// gate uses, and the profile any new gesture test should start from: a
// clean-input gesture test is the exception that has to be argued for
// (tools/gate/cleaninput.ts), because the sketchpad's palette passed all
// 22 of its clean-input checks and did not work in the owner's hands.
export const TOUCHSIM_HARDWARE_MEASURED: TouchSimConfig = {
  reportRateHz: 60,
  dropoutsEnabled: true,
  dropoutsPerSec: 34,
  straysEnabled: false,
  straysPerSec: 0,
  positionJitterEnabled: true,
  positionJitterPerSec: 1.5,
  positionJitterMinPx: 80,
  positionJitterMaxPx: 250,
  positionJitterMaxHoldReports: 3,
};

// How long a push-rectangle outline stays visible in the overlay before
// fully fading (see overlay.ts). Not part of any ABI; a UI choice.
export const PUSH_FADE_MS = 400;

// Recorder ring-buffer cap (see recorder.ts). At a steady 60Hz tick rate
// this is a little over 13 minutes of full-rate history, which comfortably
// covers "what just happened" without growing without bound in a session
// left running overnight.
export const TRACE_MAX_EVENTS = 50000;
