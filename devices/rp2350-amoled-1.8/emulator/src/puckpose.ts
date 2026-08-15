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
// THE HONEST PART, stated once here rather than re-derived at every call
// site: what this function returns is fed to emu_sensor_vector() exactly as
// emu_shim.c's own header comment describes it - already in panel space,
// which is only the same thing as the QMI8658's own device space because
// tilt.c's device_to_panel() is CURRENTLY an identity hypothesis, not a
// measurement (tilt.c's "THE AXIS RITUAL"). This file cannot know any
// better than that hypothesis does - no software here has ever seen which
// way the part is actually soldered to this board. If the ritual is ever
// run on real hardware and device_to_panel() is corrected, this file does
// NOT need to change: the same vectors flow through the same
// tilt_submit_device_g() -> device_to_panel() pipeline (shared C, both
// targets), so the emulator's four-edge buttons automatically start
// reporting whatever the corrected mapping says, the next time emu.wasm is
// rebuilt. That is the whole point of never computing "up" here - see the
// task's report for what a user sees while the hypothesis is still wrong.
export function gravityFromPose(turnDeg: number, tiltDeg: number): [number, number, number] {
  const turn = (turnDeg * Math.PI) / 180;
  const tilt = (tiltDeg * Math.PI) / 180;
  const inPlane = Math.sin(tilt);
  // Verified against firmware/runtime/tilt.h's own axis ritual and
  // emulator/wasm/tests/feature-tilt.ts's assertions (both read PANEL
  // space): turn=0,tilt=90 -> (0,1,0) "upright, top edge up"; turn=90 ->
  // (-1,0,0) "quarter turn, right edge up"; turn=180 -> (0,-1,0) "upside
  // down, bottom edge up"; turn=-90 -> (1,0,0) "left edge up". tilt=0 or 180
  // collapse to (0,0,+-1) regardless of turn, which is the point: turning a
  // flat or face-down puck cannot change which way gravity points.
  return [-Math.sin(turn) * inPlane, Math.cos(turn) * inPlane, Math.cos(tilt)];
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
