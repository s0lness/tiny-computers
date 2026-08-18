// puckpose: the ONE function that turns the page's own idea of "how is the
// puck being held right now" into a gravity vector, per firmware/runtime/
// tilt.h's axis ritual and docs/decisions/0003 (the emulator runs the real
// apps). Two knobs are enough to reach every pose the ritual describes and
// every TILT_UP_* edge tilt.c can report:
//
//   TURN  which of the panel's four edges is being lifted - the SAME
//         quarter-turn main.ts's view-rotation control (#rotQuick) already
//         offers (0/90/-90/180), reused rather than duplicated. See
//         main.ts's own header comment on why turning the puck to read a
//         landscape app sideways and turning it to test a different up-edge
//         had to become the same action, not two controls that happen to
//         agree today and can drift apart tomorrow.
//   TILT  degrees off flat: 0 lying flat screen up, 90 on edge, 180 screen
//         down flat - the exact quantity tilt.h's own tiltDeg field
//         measures, published back after the firmware's filter, so the
//         name matches on purpose.
//
// A third knob (spin about gravity's own axis) is deliberately absent: it
// cannot change anything gravity can see, so a control for it would be a
// third slider that never proves anything the other two don't already.
//
// WHY THIS OBEYS DECISION 0003'S RULE ("do not compute up/tiltDeg/filtering
// in TypeScript" - see the task that added this file, and firmware/runtime/
// tilt.h's own header): this function does neither. It picks a unit vector
// - which way is down - from two angles a hand can picture, exactly what
// sensors.ts's old, now-removed gravityFromAngles() did for its own pair of
// sliders. Which edge that vector makes tilt.c call "up", whether the
// filter has caught up to it yet, and the angle from flat it reports back
// are ALL decided by firmware/runtime/tilt.c, compiled into this same
// emu.wasm - never here. Callers hand the result straight to
// emu_sensor_vector() and read everything else back through emu_tilt().
//
// DEVICE AXES, NOT PANEL ONES - stated once here rather than re-derived at
// every call site. emu_sensor_vector() hands its argument straight to
// tilt_submit_device_g() (emu_shim.c's own header comment), exactly like a
// real QMI8658 sample, so firmware/runtime/tilt.c's device_to_panel() runs
// on whatever this function returns, for real. That mapping was MEASURED on
// real hardware 2026-08-17 (tilt.c's own header has the arithmetic: swap
// X/Y, negate Z) and is no longer the identity this file used to assume by
// accident - under the identity, "panel space" and "device space" were the
// same numbers, which is what let an earlier version of this function build
// the panel pose directly and hand it straight to emu_sensor_vector(). That
// stopped being correct the moment device_to_panel() became a real,
// non-identity mapping.
//
// So this function now does two steps, not one: first it builds the PANEL
// pose TURN/TILT describe, in the plain sense tilt.h itself uses (turn=0,
// tilt=90 -> (0,1,0) "upright, top edge up"; turn=90 -> (-1,0,0) "right
// edge up"; turn=180 -> (0,-1,0) "bottom edge up"; turn=-90 -> (1,0,0) "left
// edge up"; tilt=0 or 180 collapse to (0,0,+-1) regardless of turn, since
// turning a flat or face-down puck cannot change which way gravity points).
// Then it runs that pose through device_to_panel()'s own INVERSE to recover
// the device axes emu_sensor_vector() actually wants - the same conversion
// every regression test's own gravity()/tilt() helper applies (e.g.
// feature-tilt.ts's gravity(), feature-tiltball.ts's tilt()). Deciding "up"
// itself is still never done here (that is still all tilt.c, compiled into
// this same emu.wasm) - only which axes carry which number changed.
//
// device_to_panel() is currently ITS OWN INVERSE (swap X/Y, negate Z is an
// involution: applying it twice is the identity), so the line below repeats
// that exact formula rather than a second, independent one. THIS IS NOT
// AUTOMATIC: if device_to_panel() is ever corrected to a mapping that is
// not its own inverse, this function has to change to a real inverse, or it
// will silently start reporting the wrong up-edge again - there is no
// shared-C shortcut here the way there is for filtering or the up-edge
// decision, because turning a "which way is down" vector into device axes
// is exactly the one piece of the pipeline this file is trusted to do.
export function gravityFromPose(turnDeg: number, tiltDeg: number): [number, number, number] {
  const turn = (turnDeg * Math.PI) / 180;
  const tilt = (tiltDeg * Math.PI) / 180;
  const inPlane = Math.sin(tilt);
  const px = -Math.sin(turn) * inPlane;
  const py = Math.cos(turn) * inPlane;
  const pz = Math.cos(tilt);
  return [py, px, -pz]; // device_to_panel()'s inverse, which is itself - see header comment
}

// A label for the edge TURN currently raises - for the diagnostics strip and
// tooltips only. Never fed back into the vector above and never read by
// anything that decides app behaviour: that is tilt.c's own `up` field,
// read through emu_tilt() by the tests, never derived here (the same rule
// this whole file exists to honour).
export function edgeNameForTurn(turnDeg: number): string {
  const quarter = ((Math.round(turnDeg / 90) % 4) + 4) % 4;
  return ["TOP", "RIGHT", "BOTTOM", "LEFT"][quarter]!;
}

// A short, honest word for what TILT means right now, for the same
// tooltip/diagnostics use as edgeNameForTurn above.
export function tiltLabel(tiltDeg: number): string {
  if (tiltDeg <= 0) return "flat";
  if (tiltDeg >= 180) return "face down";
  if (tiltDeg === 90) return "on edge";
  return `${tiltDeg}°`;
}
