// Builds one control per declared "event" sensor, from the device's own
// declaration (emu_abi.h: "a shake, a tap, a step: anything the firmware
// receives as 'it happened' rather than as a continuous value"), firing
// emu_sensor_event.
//
// A "gravity" sensor is deliberately NOT built here any more. It used to
// get its own pair of angle sliders, separate from the puck's own on-screen
// rotation - which was exactly the bug: two controls that could disagree
// about which way the puck was being held, one cosmetic and one real (see
// main.ts's own header comment, and docs/decisions/0003). main.ts now owns
// the single turn/tilt control that both rotates the puck the user sees AND
// drives emu_sensor_vector (see puckpose.ts), so a "gravity" sensor is
// simply skipped here - the same "unknown fields are ignored" policy
// emu_abi.h already states for any kind this file does not build a widget
// for.
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
  onFire?: (sensor: DeviceSensor, index: number) => void
): void {
  container.innerHTML = "";
  sensors.forEach((sensor, index) => {
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
