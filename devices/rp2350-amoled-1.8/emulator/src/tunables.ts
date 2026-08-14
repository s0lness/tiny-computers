// Builds one slider per declared live tunable (emu_abi.h's "tunables"
// section: development-only knobs a firmware exposes for fast iteration,
// today sketch.c's SKETCH_LIVE_TUNE dropout-tolerance constants). Nothing
// here names "lift" or "sketch" or any other specific knob - the row set
// comes entirely from DeviceDescriptor.tunables, the same "declare the
// shape, build the chrome from the declaration" pattern main.ts's button
// loop and sensors.ts's buildSensorControls already use for buttons and
// event sensors. Adding a sixth tunable in sketch.c's own declaration later
// needs no change here.
//
// THIS PANEL IS FOR ITERATION SPEED, NOT FOR FINDING THE RIGHT VALUE. See
// emu_abi.h's tunables section and README-devlink.md's TUNE section: the
// emulator's TouchSim dropout model is measurably kinder than a real touch
// controller, so a value dialed in here is a hypothesis about the real
// device, not a result. Callers of buildTuneControls are expected to show
// that caveat once, near the panel - see main.ts's wiring - rather than
// have it live only in code comments nobody using the page will read.

import type { EmuExports, DeviceTunable } from "./wasm";

export function buildTuneControls(container: HTMLElement, tunables: DeviceTunable[], emu: EmuExports): void {
  container.innerHTML = "";
  if (tunables.length === 0 || !emu.emu_tune_get || !emu.emu_tune_set) return;

  const tuneGet = emu.emu_tune_get;
  const tuneSet = emu.emu_tune_set;

  tunables.forEach((t, index) => {
    const current = tuneGet(index);
    // Step: two decimal places for a sub-10 range (e.g. maxspeed's 0.5..30),
    // whole numbers otherwise (lift, confirm, minjump, maxjump all live in
    // the tens-to-hundreds). This is a display/interaction nicety, not a
    // protocol requirement - the device itself is the one that clamps.
    const step = t.max - t.min <= 40 ? 0.1 : 1;

    const row = document.createElement("div");
    row.className = "slider-row";
    row.innerHTML = `
      <div class="row-head"><b>${t.id}</b><span class="tune-val"></span></div>
      <input type="range" min="${t.min}" max="${t.max}" step="${step}" value="${current}" />
    `;
    const valEl = row.querySelector<HTMLElement>(".tune-val")!;
    const inputEl = row.querySelector<HTMLInputElement>("input[type=range]")!;
    const setValLabel = (v: number) => {
      valEl.textContent = Number.isInteger(v) ? `${v}` : v.toFixed(2);
    };
    setValLabel(current);

    inputEl.addEventListener("input", () => {
      const v = Number(inputEl.value);
      tuneSet(index, v);
      // Read back rather than trust the slider: the device (and this wasm
      // module, mirroring it) clamps to [min, max] itself, so an applied
      // value can differ from what was dragged to, and the label should
      // show the truth, not the request.
      setValLabel(tuneGet(index));
    });

    container.appendChild(row);
  });
}
