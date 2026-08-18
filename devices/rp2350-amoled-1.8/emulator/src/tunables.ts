// Builds the live-tunable panel (emu_abi.h's "tunables" section:
// development-only knobs a firmware exposes for fast iteration - started
// as sketch.c's own SKETCH_LIVE_TUNE dropout-tolerance constants, joined
// since by clock.c's pulse shape and tables.c's thumb bias, all merged
// behind firmware/runtime/tune_registry.h's one registry). Nothing here
// names "lift", "sketch", or any other specific knob or app - the row set comes
// from DeviceDescriptor.tunables (the emulator's own declaration) UNIONED,
// when a board is connected, with whatever the board's own `TUNE` command
// reports, the same "declare the shape, build the chrome from the
// declaration" pattern main.ts's button loop and sensors.ts's
// buildSensorControls already use for buttons and event sensors.
//
// THE BUG THIS FILE EXISTS TO NOT REPRODUCE: the owner moved these sliders,
// drew on the board, and felt nothing change, because the sliders tuned the
// emulator's own copy - a control that looks identical whether it is
// touching the emulator or the real device is exactly the kind of "looks
// like a toggle, guesses like a coin flip" bug this project's UI feedback
// work (see main.ts's mute-icon comment) keeps re-learning the same lesson
// about. The fix here is not a mode switch (considered and rejected, see
// the task's own reasoning: a mode is invisible state that decides for
// you): when a board is connected, EVERY row shows two numbers, one per
// machine, side by side, with an explicit per-row lock for when they should
// move together. Two visible numbers cannot be misread as one.
//
// A knob present on only one side (emulator build vs. board's own build)
// shows as such - "not on device" / "not on emulator" - rather than
// vanishing from the union silently, and a board whose firmware has live
// tuning compiled out (devlink's "ERR no tunables") says so in a banner
// rather than leaving the device column blank with no explanation.

import type { EmuExports, DeviceTunable } from "./wasm";
import type { DevlinkClient } from "./devlinkClient";
import type { DeviceTuneRow } from "./devlinkProtocol";
import { parseTuneListLine, parseTuneValueLine, parseTuneResetLine, parseErrLine, tuneListMatcher, tuneValueMatcher, tuneResetOneMatcher } from "./devlinkProtocol";

function fmtStep(min: number, max: number): number {
  return max - min <= 40 ? 0.1 : 1;
}
function fmtLabel(v: number): string {
  return Number.isInteger(v) ? `${v}` : v.toFixed(2);
}

interface RowRefs {
  root: HTMLElement;
  lockWrap: HTMLElement;
  lockInput: HTMLInputElement;
  emuCol: HTMLElement;
  emuBody: HTMLElement;
  devCol: HTMLElement;
  devBody: HTMLElement;
}

function buildRowSkeleton(container: HTMLElement, name: string): RowRefs {
  const root = document.createElement("div");
  root.className = "slider-row tune-row";
  root.dataset.name = name;
  root.innerHTML = `
    <div class="row-head">
      <b>${name}</b>
      <label class="tune-lock hidden" title="move together: changing either side also sets the other">
        <input type="checkbox" class="tune-lock-input" /><span>lock</span>
      </label>
    </div>
    <div class="tune-cols">
      <div class="tune-col tune-col-emu">
        <span class="tune-col-label">emu</span>
        <div class="tune-col-body"></div>
      </div>
      <div class="tune-col tune-col-dev hidden">
        <span class="tune-col-label">device</span>
        <div class="tune-col-body"></div>
      </div>
    </div>
  `;
  container.appendChild(root);
  return {
    root,
    lockWrap: root.querySelector<HTMLElement>(".tune-lock")!,
    lockInput: root.querySelector<HTMLInputElement>(".tune-lock-input")!,
    emuCol: root.querySelector<HTMLElement>(".tune-col-emu")!,
    emuBody: root.querySelector<HTMLElement>(".tune-col-emu .tune-col-body")!,
    devCol: root.querySelector<HTMLElement>(".tune-col-dev")!,
    devBody: root.querySelector<HTMLElement>(".tune-col-dev .tune-col-body")!,
  };
}

// Fills one column's body with either a working slider (min/max/value known
// on this side) or a quiet note ("not on device" and friends) - the same
// column element either way, so connecting/disconnecting or a device reply
// arriving never has to rebuild the row, only swap what is inside one body.
function renderControl(
  body: HTMLElement,
  range: { min: number; max: number } | null,
  value: number | null,
  onInput: ((v: number) => void) | null,
  onReset: (() => void) | null,
  note: string | null
): { setValue: (v: number) => void } {
  body.innerHTML = "";
  if (!range || value === null) {
    const span = document.createElement("span");
    span.className = "tune-missing-note";
    span.textContent = note ?? "–";
    body.appendChild(span);
    return { setValue: () => {} };
  }
  const step = fmtStep(range.min, range.max);
  const input = document.createElement("input");
  input.type = "range";
  input.min = String(range.min);
  input.max = String(range.max);
  input.step = String(step);
  input.value = String(value);
  body.appendChild(input);

  const foot = document.createElement("div");
  foot.className = "tune-col-foot";
  const valEl = document.createElement("span");
  valEl.className = "tune-val";
  valEl.textContent = fmtLabel(value);
  foot.appendChild(valEl);
  if (onReset) {
    const resetBtn = document.createElement("button");
    resetBtn.type = "button";
    resetBtn.className = "tune-reset chrome-btn";
    resetBtn.textContent = "reset";
    resetBtn.addEventListener("click", onReset);
    foot.appendChild(resetBtn);
  }
  body.appendChild(foot);

  const setValue = (v: number) => {
    valEl.textContent = fmtLabel(v);
    if (document.activeElement !== input) input.value = String(v);
  };
  if (onInput) {
    input.addEventListener("input", () => onInput(Number(input.value)));
  }
  return { setValue };
}

export function buildTuneControls(container: HTMLElement, wrap: HTMLElement, tunables: DeviceTunable[], emu: EmuExports, devlink?: DevlinkClient): void {
  container.innerHTML = "";
  const hasEmuTune = tunables.length > 0 && !!emu.emu_tune_get && !!emu.emu_tune_set;
  wrap.classList.toggle("hidden", !hasEmuTune && !devlink);

  if (!hasEmuTune && !devlink) return;

  const statusEl = document.createElement("div");
  statusEl.className = "tune-device-status hidden";
  container.appendChild(statusEl);

  interface RowState {
    refs: RowRefs;
    emuIndex: number | null; // index into `tunables`, or null if device-only
    emuSetValue: ((v: number) => void) | null;
    devSetValue: ((v: number) => void) | null;
    hasDev: boolean;
  }
  const rows = new Map<string, RowState>();

  function updateLockVisibility(state: RowState): void {
    const show = state.emuIndex !== null && state.hasDev;
    state.refs.lockWrap.classList.toggle("hidden", !show);
    if (!show) state.refs.lockInput.checked = false;
  }

  // ---- emulator-declared rows: unchanged behaviour from before this task
  // when no device is connected (same slider, same read-back-after-set
  // discipline), plus a reset button (emu_tune_reset, wired here for the
  // first time - see emu_shim.c's own comment on why it existed with no
  // caller until now).
  if (hasEmuTune) {
    const tuneGet = emu.emu_tune_get!;
    const tuneSet = emu.emu_tune_set!;
    const tuneReset = emu.emu_tune_reset;
    tunables.forEach((t, index) => {
      const refs = buildRowSkeleton(container, t.id);
      const state: RowState = { refs, emuIndex: index, emuSetValue: null, devSetValue: null, hasDev: false };
      rows.set(t.id, state);

      const applyEmu = (v: number) => {
        tuneSet(index, v);
        const applied = tuneGet(index);
        state.emuSetValue?.(applied);
        if (state.refs.lockInput.checked && state.hasDev) void setDevice(t.id, applied, true);
        return applied;
      };
      const resetEmu = () => {
        tuneReset?.(index);
        const applied = tuneGet(index);
        state.emuSetValue?.(applied);
        if (state.refs.lockInput.checked && state.hasDev) void resetDevice(t.id, true);
      };
      const ctl = renderControl(refs.emuBody, { min: t.min, max: t.max }, tuneGet(index), applyEmu, tuneReset ? resetEmu : null, null);
      state.emuSetValue = ctl.setValue;
    });
  }

  if (!devlink) return;

  // ---- mirroring: "a way to move them together for when he wants them
  // locked". Only fires from the SIDE THAT WAS NOT the origin of the
  // change (skipOrigin), so a locked round trip cannot echo back into the
  // control the owner is actually holding.
  async function setDevice(name: string, value: number, fromLock: boolean): Promise<void> {
    const state = rows.get(name);
    try {
      const lines = await devlink!.request(`TUNE SET ${name} ${value}`, tuneValueMatcher);
      const last = lines[lines.length - 1];
      const parsed = parseTuneValueLine(last);
      if (parsed) {
        state?.devSetValue?.(parsed.value);
        if (!fromLock && state?.refs.lockInput.checked && state.emuIndex !== null) {
          emu.emu_tune_set!(state.emuIndex, parsed.value);
          state.emuSetValue?.(emu.emu_tune_get!(state.emuIndex));
        }
      }
    } catch {
      // Round trip failed (device dropped mid-request, timeout): the next
      // status change or manual refresh will resync; a dropped SET must
      // not be reported as applied.
    }
  }
  async function resetDevice(name: string, fromLock: boolean): Promise<void> {
    const state = rows.get(name);
    try {
      const lines = await devlink!.request(`TUNE RESET ${name}`, tuneResetOneMatcher);
      const last = lines[lines.length - 1];
      const parsed = parseTuneResetLine(last);
      if (parsed) {
        state?.devSetValue?.(parsed.applied);
        if (!fromLock && state?.refs.lockInput.checked && state.emuIndex !== null) {
          emu.emu_tune_reset?.(state.emuIndex);
          state.emuSetValue?.(emu.emu_tune_get!(state.emuIndex));
        }
      }
    } catch {
      // same reasoning as setDevice's catch above
    }
  }

  function ensureRow(name: string): RowState {
    let state = rows.get(name);
    if (!state) {
      const refs = buildRowSkeleton(container, name);
      state = { refs, emuIndex: null, emuSetValue: null, devSetValue: null, hasDev: false };
      renderControl(refs.emuBody, null, null, null, null, "not on emulator");
      rows.set(name, state);
    }
    return state;
  }

  function renderDeviceRow(state: RowState, row: DeviceTuneRow): void {
    state.hasDev = true;
    const onInput = (v: number) => void setDevice(row.name, v, false);
    const onReset = () => void resetDevice(row.name, false);
    const ctl = renderControl(state.refs.devBody, { min: row.min, max: row.max }, row.value, onInput, onReset, null);
    state.devSetValue = ctl.setValue;
    updateLockVisibility(state);
  }

  function renderDeviceMissing(state: RowState, note: string): void {
    state.hasDev = false;
    state.devSetValue = null;
    renderControl(state.refs.devBody, null, null, null, null, note);
    updateLockVisibility(state);
  }

  function showDeviceColumns(show: boolean): void {
    for (const state of rows.values()) state.refs.devCol.classList.toggle("hidden", !show);
    container.classList.toggle("device-connected", show);
  }

  async function refreshDevice(): Promise<void> {
    if (!devlink!.status.connected) {
      statusEl.classList.add("hidden");
      showDeviceColumns(false);
      wrap.classList.toggle("hidden", !hasEmuTune);
      return;
    }
    statusEl.classList.remove("hidden");
    statusEl.textContent = "device: reading tunables…";
    let lines: string[];
    try {
      lines = await devlink!.request("TUNE", tuneListMatcher, 3000);
    } catch (err) {
      statusEl.textContent = `device: could not read tunables (${err instanceof Error ? err.message : String(err)})`;
      showDeviceColumns(true);
      for (const state of rows.values()) renderDeviceMissing(state, "unreachable");
      return;
    }
    const last = lines[lines.length - 1];
    const errText = last ? parseErrLine(last) : null;
    if (errText !== null) {
      statusEl.textContent = errText.startsWith("no tunables") ? "device: no live tunables in this build (every LIVE_TUNE flag off)" : `device: ${errText}`;
      showDeviceColumns(true);
      for (const state of rows.values()) renderDeviceMissing(state, "none declared");
      wrap.classList.remove("hidden");
      return;
    }

    const deviceRows = lines
      .filter((l) => l !== "END")
      .map(parseTuneListLine)
      .filter((r): r is DeviceTuneRow => r !== null);
    statusEl.textContent = `device: ${deviceRows.length} tunable${deviceRows.length === 1 ? "" : "s"}`;
    wrap.classList.remove("hidden");
    showDeviceColumns(true);

    const seen = new Set<string>();
    for (const row of deviceRows) {
      seen.add(row.name);
      renderDeviceRow(ensureRow(row.name), row);
    }
    for (const [name, state] of rows) {
      if (!seen.has(name)) renderDeviceMissing(state, "not on device");
    }
  }

  devlink.onStatus(() => void refreshDevice());
}
