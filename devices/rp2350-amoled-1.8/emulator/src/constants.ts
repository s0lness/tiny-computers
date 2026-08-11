// Tunable constants, ported 1:1 from the #defines at the top of
// firmware/main.c (lines 41-82). Names and default values match exactly so a
// value tuned here by feel can be pasted straight back as a #define.
//
// Two constants used by the pen (radius clamp 1..8px, the 0.35/0.65 start
// taper blend, the 0.7/0.45/0.25 end-taper scales, the 1.2px end-taper step)
// are also lifted from main.c but are NOT exposed as sliders here because
// the task list of tunables did not include them; they stay as fixed
// constants in pen.ts, called out in the README.

export interface Tunables {
  STREAMLINE: number;
  DEDUPE_PX: number;
  SPEED_MAX: number;
  PRESSURE_LERP: number;
  PEN_SIZE: number;
  PEN_THINNING: number;
  START_TAPER_LEN: number;
  LIFT_DEBOUNCE_MS: number;
  MAX_SPEED_PX_PER_MS: number;
  MIN_JUMP_ALLOW_PX: number;
  MAX_JUMP_PX: number;
  CONFIRM_PX: number;
}

// firmware/main.c lines 41-82, verbatim defaults.
export const FIRMWARE_DEFAULTS: Tunables = {
  STREAMLINE: 0.35,
  DEDUPE_PX: 0.7,
  SPEED_MAX: 14.0,
  PRESSURE_LERP: 0.275,
  PEN_SIZE: 5.0,
  PEN_THINNING: 0.5,
  START_TAPER_LEN: 10.0,
  LIFT_DEBOUNCE_MS: 80,
  MAX_SPEED_PX_PER_MS: 6.0,
  MIN_JUMP_ALLOW_PX: 40.0,
  MAX_JUMP_PX: 150.0,
  CONFIRM_PX: 25.0,
};

export type TunableKey = keyof Tunables;

export interface TunableMeta {
  key: TunableKey;
  label: string;
  group: "pen" | "touch";
  min: number;
  max: number;
  step: number;
  unit: string;
  note: string;
}

// Slider metadata for the control panel. Ranges are generous headroom
// around the firmware default, not hard limits from the hardware.
export const TUNABLE_META: TunableMeta[] = [
  { key: "STREAMLINE", label: "STREAMLINE", group: "pen", min: 0, max: 0.9, step: 0.01, unit: "",
    note: "how much the drawn point trails the raw touch (latency vs jitter)" },
  { key: "DEDUPE_PX", label: "DEDUPE_PX", group: "pen", min: 0, max: 3, step: 0.05, unit: "px",
    note: "resting-finger jitter dropped below this travel distance" },
  { key: "SPEED_MAX", label: "SPEED_MAX", group: "pen", min: 1, max: 40, step: 0.5, unit: "px",
    note: "travel speed that maps to minimum simulated pressure" },
  { key: "PRESSURE_LERP", label: "PRESSURE_LERP", group: "pen", min: 0.01, max: 1, step: 0.005, unit: "",
    note: "rate-limit on pressure moving toward its speed-derived target" },
  { key: "PEN_SIZE", label: "PEN_SIZE", group: "pen", min: 1, max: 12, step: 0.1, unit: "px",
    note: "base stroke radius before thinning and taper" },
  { key: "PEN_THINNING", label: "PEN_THINNING", group: "pen", min: 0, max: 1, step: 0.01, unit: "",
    note: "how much pressure widens/narrows the radius" },
  { key: "START_TAPER_LEN", label: "START_TAPER_LEN", group: "pen", min: 0, max: 40, step: 0.5, unit: "px",
    note: "arc length over which a new stroke ramps up to full width" },
  { key: "LIFT_DEBOUNCE_MS", label: "LIFT_DEBOUNCE_MS", group: "touch", min: 0, max: 300, step: 5, unit: "ms",
    note: "how long a zero-finger report must persist before believing a real lift" },
  { key: "MAX_SPEED_PX_PER_MS", label: "MAX_SPEED_PX_PER_MS", group: "touch", min: 0.5, max: 20, step: 0.1, unit: "px/ms",
    note: "speed limit used to size the allowed jump between samples" },
  { key: "MIN_JUMP_ALLOW_PX", label: "MIN_JUMP_ALLOW_PX", group: "touch", min: 0, max: 150, step: 1, unit: "px",
    note: "floor on the allowed jump, even at dt=0" },
  { key: "MAX_JUMP_PX", label: "MAX_JUMP_PX", group: "touch", min: 0, max: 368, step: 1, unit: "px",
    note: "ceiling on the allowed jump; beyond this a gap always splits the stroke" },
  { key: "CONFIRM_PX", label: "CONFIRM_PX", group: "touch", min: 0, max: 100, step: 1, unit: "px",
    note: "agreement radius for a second report to confirm a rejected jump" },
];

// Touch-controller imperfection model. Not in the firmware (there is no
// #define for a bug the hardware itself has); this is our own simulation of
// what the FT3168 actually does on this board, per AGENTS.md and main.c's
// comments on glitches/dropouts/strays.
export interface TouchSimConfig {
  reportRateHz: number;
  dropoutsEnabled: boolean;
  dropoutsPerSec: number;
  straysEnabled: boolean;
  straysPerSec: number;
}

export const TOUCHSIM_DEFAULTS: TouchSimConfig = {
  reportRateHz: 60, // FT3168_PERIOD_MS = 10 in main.c targets 100Hz, but the
                     // observed/default scan rate the firmware's own comments
                     // describe (and what it found before tuning) is 60Hz.
  dropoutsEnabled: true,
  dropoutsPerSec: 2, // "1 to 3 per second while drawing" (task brief), measured
  straysEnabled: true,
  straysPerSec: 0.2,
};

export const PANEL_W = 368;
export const PANEL_H = 448;
