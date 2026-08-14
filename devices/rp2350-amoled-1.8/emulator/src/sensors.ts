// Builds one control per declared sensor, from the device's own
// declaration:
//
//   "kind": "event"    one button (emu_abi.h: "a shake, a tap, a step:
//                      anything the firmware receives as 'it happened'
//                      rather than as a continuous value"), firing
//                      emu_sensor_event.
//   "kind": "gravity"  two angle sliders, feeding emu_sensor_vector. The
//                      firmware declares the KIND, not the widget, and this
//                      file decides that an orientation is manipulated as
//                      two angles rather than as three number boxes:
//                      nobody thinks in components, everybody can tip a
//                      thing over.
//
// A sensor of any other kind is still skipped, and that is deliberate
// ("unknown fields are ignored", emu_abi.h) rather than a TODO.
//
// WHAT THE SLIDERS CANNOT DO, so that nothing is judged here that has to be
// judged on the board: they hold perfectly still, and they are exactly one
// g in length. A real accelerometer in a child's hand is never still and
// reads several g through a shake, which is why firmware/runtime/tilt.h's
// filter constant is a hypothesis until someone holds the real device. Same
// category as emu_abi.h's timing and input-defect caveats.
//
// Index passed to emu_sensor_event MUST be the sensor's index in the FULL
// declared array, not its position among the ones actually rendered, so a
// skipped non-event sensor does not shift every index after it.
//
// Keyboard shortcuts go through the shared ShortcutRegistry (see
// shortcuts.ts) rather than this module attaching its own window listener:
// the whole chrome, including this, is rebuilt from scratch on every wasm
// reload, and a per-module listener would leak one more copy of itself on
// every rebuild.

import type { EmuExports, DeviceSensor } from "./wasm";
import type { ShortcutRegistry } from "./shortcuts";
import { assignShortcut } from "./shortcuts";

// Gravity, in the panel's own axes, from two angles a hand can picture.
// Flat on a table is (0, 0, 1), which is what both sliders at zero means.
//
//   tip   raises the device's TOP edge. At 90 it is standing upright with
//         its top edge up, and gravity runs down the screen: (0, 1, 0).
//   roll  raises its RIGHT edge, so gravity runs toward the left: at 90,
//         (-1, 0, 0).
//
// Exactly unit length at every combination (the sines and cosines square
// away), so this can never hand the firmware a vector a still hand could
// not produce.
export function gravityFromAngles(tipDeg: number, rollDeg: number): [number, number, number] {
  const tip = (tipDeg * Math.PI) / 180;
  const roll = (rollDeg * Math.PI) / 180;
  return [-Math.sin(roll) * Math.cos(tip), Math.sin(tip), Math.cos(roll) * Math.cos(tip)];
}

export function buildSensorControls(
  container: HTMLElement,
  sensors: DeviceSensor[],
  emu: EmuExports,
  shortcuts: ShortcutRegistry,
  usedKeys: Set<string>,
  log: (text: string) => void,
  // Fired right after emu_sensor_event(), so a button/keyboard press can
  // drive something visible (the puck-motion shake, for the "shake"
  // sensor) even though there is no real window motion behind a click.
  onFire?: (sensor: DeviceSensor, index: number) => void,
  // Fired right after emu_sensor_vector(), so the caller can record it into
  // the replay trace. Sticky values have to be in the trace or a replay
  // runs the whole session at the boot pose (see recorder.ts).
  onVector?: (sensor: DeviceSensor, index: number, x: number, y: number, z: number) => void
): void {
  container.innerHTML = "";
  sensors.forEach((sensor, index) => {
    if (sensor.kind === "gravity") {
      buildGravityControl(container, sensor, index, emu, log, onVector);
      return;
    }
    if (sensor.kind !== "event") return;
    const fire = () => {
      emu.emu_sensor_event(index);
      log(`sensor: ${sensor.id}`);
      onFire?.(sensor, index);
    };
    const key = assignShortcut(sensor.id, usedKeys);
    const btn = document.createElement("button");
    btn.className = "btn sec sm sensor-btn";
    btn.textContent = sensor.label || sensor.id;
    if (key) {
      const kbd = document.createElement("span");
      kbd.className = "kbd";
      kbd.textContent = key.toUpperCase();
      btn.appendChild(kbd);
      shortcuts.bindClick(key, fire);
    }
    btn.addEventListener("click", fire);
    container.appendChild(btn);
  });
}

// Two sliders and a way back to flat. Deliberately NOT wired to the puck's
// cosmetic tilt slider next to it (main.ts's #tilt, which rotates the
// picture for a jauntier photo and sends the firmware nothing): one control
// changes what the page looks like, this one changes what the firmware
// believes about the world, and quietly merging them would make it
// impossible to tell which of the two a wrong-looking app was reacting to.
function buildGravityControl(
  container: HTMLElement,
  sensor: DeviceSensor,
  index: number,
  emu: EmuExports,
  log: (text: string) => void,
  onVector?: (sensor: DeviceSensor, index: number, x: number, y: number, z: number) => void
): void {
  if (!emu.emu_sensor_vector) return; // declared but not exported: nothing to drive

  let tipDeg = 0;
  let rollDeg = 0;

  const wrap = document.createElement("span");
  wrap.className = "sensor-vec";
  wrap.title =
    `${sensor.label || sensor.id}: which way is down. tip raises the top edge, roll raises the ` +
    `right edge; both at 0 is lying flat on a table, screen up.`;

  const readout = document.createElement("span");
  readout.className = "pill hint";

  const send = () => {
    const [x, y, z] = gravityFromAngles(tipDeg, rollDeg);
    emu.emu_sensor_vector!(index, x, y, z);
    onVector?.(sensor, index, x, y, z);
    // The COMMANDED pose, which is not the same thing as what the app is
    // seeing: the firmware filters it (tilt.h, 150ms), so the app's own
    // value lags this by design. Anything that has to assert on what the
    // app actually received reads emu_tilt() instead, and the headless test
    // (emulator/wasm/tests/feature-tilt.ts) is where that lives.
    readout.textContent = `tip ${tipDeg}° roll ${rollDeg}°`;
  };

  const slider = (label: string, min: number, max: number, set: (v: number) => void): HTMLElement => {
    const box = document.createElement("label");
    box.className = "sensor-vec-slider";
    const cap = document.createElement("span");
    cap.className = "kbd";
    cap.textContent = label;
    const input = document.createElement("input");
    input.type = "range";
    input.min = String(min);
    input.max = String(max);
    input.step = "1";
    input.value = "0";
    input.className = "tilt-slider";
    input.addEventListener("input", () => {
      set(Number(input.value));
      send();
    });
    box.appendChild(cap);
    box.appendChild(input);
    // Handed back so "flat" can put the thumbs back where the numbers went.
    (box as HTMLElement & { input?: HTMLInputElement }).input = input;
    return box;
  };

  // tip runs the full circle so face-down is reachable (at 180); roll only
  // needs a quarter turn each way to cover every remaining pose.
  const tipBox = slider("tip", -180, 180, (v) => (tipDeg = v));
  const rollBox = slider("roll", -90, 90, (v) => (rollDeg = v));

  const flat = document.createElement("button");
  flat.className = "btn sec sm";
  flat.textContent = "flat";
  flat.title = "lay it back down on the table (0, 0, 1)";
  flat.addEventListener("click", () => {
    tipDeg = 0;
    rollDeg = 0;
    for (const box of [tipBox, rollBox]) {
      const input = (box as HTMLElement & { input?: HTMLInputElement }).input;
      if (input) input.value = "0";
    }
    send();
    log(`sensor: ${sensor.id} back to flat`);
  });

  wrap.appendChild(tipBox);
  wrap.appendChild(rollBox);
  wrap.appendChild(flat);
  wrap.appendChild(readout);
  container.appendChild(wrap);

  send(); // the module already boots flat; this just fills the readout
}
