// Orchestrates the whole page: loads a firmware's wasm module (see
// wasm.ts), builds the puck chrome entirely from its own emu_device()
// descriptor, drives the deterministic tick loop, and wires touch,
// buttons, sensors, apps, the push-window overlay, the touch-contact
// overlay, input recording/replay, and freeze/trace capture.
//
// No firmware behaviour lives here: every ABI call is a straight pass
// through to the module. This file's only job is turning DOM events into
// ABI calls, and ABI state into pixels.

import { type EmuExports, type DeviceDescriptor, DEFAULT_WASM_URL, fetchWasmBytes, instantiate, readDeviceDescriptor } from "./wasm";
import { readPushes, pixelReaderFor, blitRect, blitAll, type PixelReader } from "./panel";
import { PushOverlay } from "./overlay";
import { TouchOverlay, CONTACT_PRESETS, DEFAULT_PX_PER_MM } from "./touchoverlay";
import { mapClientPoint } from "./rotate";
import { makeDraggable, wireButton, createButtonElement, applyRotation, type WiredButton } from "./device";
import { buildSensorControls } from "./sensors";
import { buildAppStrip, type AppStripControl } from "./appstrip";
import { ShortcutRegistry, assignShortcut } from "./shortcuts";
import { ConsoleLog, type LogLine } from "./consolelog";
import { Recorder, type Trace } from "./recorder";
import { Replayer } from "./replay";
import { postFreeze, canvasToPngBase64, openAnnotationModal, type FreezeBundle } from "./freeze";
import { emptyJournal } from "./journal";
import { TouchSim, type TouchReport } from "./touchsim";
import { type TouchSimConfig, TOUCHSIM_DEFAULTS, TOUCH_DEFECTS_DEFAULT } from "./constants";
import { WindowShakeDetector } from "./windowshake";

const $ = <T extends HTMLElement>(sel: string) => document.querySelector<T>(sel)!;

function errMsg(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}
function escapeHtml(s: string): string {
  return s.replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]!);
}

// ---- DOM ----
const bezelEl = $("#bezel");
const deviceWrapEl = $("#deviceWrap");
const panelEl = $<HTMLCanvasElement>("#panel");
const overlayEl = $<HTMLCanvasElement>("#overlay");
const panelCtx = panelEl.getContext("2d", { willReadFrequently: true })!;
const overlayCtx = overlayEl.getContext("2d")!;
const wasmErrorEl = $("#wasmError");
const consolePaneEl = $("#consolePane");
const touchReadoutEl = $("#touchReadout");
const pushReadoutEl = $("#pushReadout");
const traceStatusEl = $("#traceStatus");
const replayBarEl = $("#replayBar");
const btnStopReplay = $<HTMLButtonElement>("#btnStopReplay");
const btnPause = $<HTMLButtonElement>("#btnPause");
const shakeReadoutEl = $("#shakeReadout");
const stageEl = $("#stage");

// ---- state ----
let emu: EmuExports | null = null;
let wasmBytes: ArrayBuffer | null = null;
let device: DeviceDescriptor | null = null;
let panelW = 0;
let panelH = 0;
let fbPtr = 0;
let pixelReader: PixelReader | null = null;
let touchEnabled = false;
let accentColor = "#c4621f";

const recorder = new Recorder();
const consoleLog = new ConsoleLog(500, appendConsoleLine);
const pushOverlay = new PushOverlay();
const touchOverlay = new TouchOverlay();
const shortcuts = new ShortcutRegistry();
const windowShake = new WindowShakeDetector();
let shakeSensorIndex = -1;
let centeredOnce = false;

const touchCfg: TouchSimConfig = { ...TOUCHSIM_DEFAULTS };
let touchDefectsEnabled = TOUCH_DEFECTS_DEFAULT;
let touchSim: TouchSim | null = null;
let liveTouch: TouchReport = { fingers: 0, x: 0, y: 0 };
let pointerIdDown: number | null = null;

let quickDeg = 0;
let tiltDeg = 0;

let wiredButtons: WiredButton[] = [];
let appStripControl: AppStripControl | null = null;
let lastAppIndex = 0;

let paused = false;
let replayer: Replayer | null = null;

const pushHistory: { tMs: number; x: number; y: number; w: number; h: number }[] = [];
const PUSH_HISTORY_MAX = 400;

// ---- console pane ----
function appendConsoleLine(line: LogLine): void {
  const div = document.createElement("div");
  div.className = "console-line";
  div.textContent = line.text;
  consolePaneEl.appendChild(div);
  while (consolePaneEl.childElementCount > 300) consolePaneEl.removeChild(consolePaneEl.firstChild!);
  consolePaneEl.scrollTop = consolePaneEl.scrollHeight;
}

// ---- wasm error banner ----
// Never a blank or frozen page on failure: this banner is always the raw
// error message plus a way forward, and (see bringUp) it is shown without
// ever tearing down a module that was already running.
function showWasmError(err: unknown): void {
  wasmErrorEl.innerHTML = "";
  const text = document.createElement("div");
  text.className = "wasm-error-text";
  text.textContent = errMsg(err);
  const retry = document.createElement("button");
  retry.className = "btn sec sm";
  retry.textContent = "retry";
  retry.addEventListener("click", () => void reloadModule("manual retry"));
  wasmErrorEl.appendChild(text);
  wasmErrorEl.appendChild(retry);
  wasmErrorEl.classList.remove("hidden");
  console.error(err);
}
function hideWasmError(): void {
  wasmErrorEl.classList.add("hidden");
}
function failReload(err: unknown): void {
  showWasmError(err);
  if (emu) consoleLog.push(`reload failed, keeping previous session running: ${errMsg(err)}`);
}

// "reloaded at HH:MM:SS, N bytes" so a stale or failed reload is obvious
// at a glance, without having to dig through the console pane.
function updateReloadStatus(byteLength: number): void {
  const t = new Date();
  const pad = (n: number) => String(n).padStart(2, "0");
  $("#reloadStatus").textContent = `reloaded ${pad(t.getHours())}:${pad(t.getMinutes())}:${pad(t.getSeconds())}, ${byteLength.toLocaleString()} bytes`;
}

// ---- physical contact size ----
function derivePxPerMm(d: DeviceDescriptor): number {
  // Not part of emu_abi.h today; a forward-compatible, defensive read in
  // case a firmware ever declares its own physical size, preferred over
  // the fallback constant when present.
  const p = d.panel as unknown as { wMm?: number; hMm?: number };
  if (typeof p.wMm === "number" && p.wMm > 0) return d.panel.w / p.wMm;
  if (typeof p.hMm === "number" && p.hMm > 0) return d.panel.h / p.hMm;
  return DEFAULT_PX_PER_MM;
}
function refreshContactInfo(): void {
  const px = Math.round(touchOverlay.contactMm * touchOverlay.pxPerMm);
  $("#contactInfo").textContent = `${touchOverlay.contactMm}mm @ ${touchOverlay.pxPerMm.toFixed(2)}px/mm = ~${px}px diameter`;
}
function wireContactPresets(): void {
  const el = $("#contactPreset");
  el.innerHTML = "";
  CONTACT_PRESETS.forEach((preset) => {
    const b = document.createElement("button");
    b.textContent = `${preset.label} (${preset.mm}mm)`;
    if (preset.mm === touchOverlay.contactMm) b.classList.add("active");
    b.addEventListener("click", () => {
      touchOverlay.contactMm = preset.mm;
      el.querySelectorAll("button").forEach((x) => x.classList.remove("active"));
      b.classList.add("active");
      refreshContactInfo();
    });
    el.appendChild(b);
  });
}

// ---- button ABI calls, recorded ----
function emuButtonDown(index: number): void {
  if (!emu) return;
  emu.emu_button(index, 1);
  recorder.record({ t: performance.now(), k: "button", i: index, down: 1 });
}
function emuButtonUp(index: number): void {
  if (!emu) return;
  emu.emu_button(index, 0);
  recorder.record({ t: performance.now(), k: "button", i: index, down: 0 });
}
function emuButtonVerdict(index: number, isLong: boolean): void {
  if (!emu) return;
  emu.emu_button_verdict(index, isLong ? 1 : 0);
  recorder.record({ t: performance.now(), k: "verdict", i: index, long: isLong ? 1 : 0 });
}

// ---- chrome: rebuilt from the device descriptor on every (re)load ----
function buildChrome(d: DeviceDescriptor): void {
  document.documentElement.style.setProperty("--panel-w", `${d.panel.w}px`);
  document.documentElement.style.setProperty("--panel-h", `${d.panel.h}px`);
  panelEl.width = d.panel.w;
  panelEl.height = d.panel.h;
  overlayEl.width = d.panel.w;
  overlayEl.height = d.panel.h;

  $("#deviceName").textContent = d.name || "device emulator";
  $("#deviceInfo").innerHTML =
    `<div>${escapeHtml(d.name || "unnamed device")}</div>` + `<div class="hint">${d.panel.w}x${d.panel.h} px, ${escapeHtml(d.panel.format)}</div>`;

  bezelEl.querySelectorAll(".dev-btn").forEach((el) => el.remove());
  wiredButtons = [];
  shortcuts.clear();
  const usedKeys = new Set<string>();
  const shortcutListEl = $("#buttonShortcuts");
  shortcutListEl.innerHTML = "";

  const buttons = d.buttons || [];
  buttons.forEach((btn, index) => {
    const el = createButtonElement(btn.edge, btn.at, bezelEl.clientWidth, bezelEl.clientHeight);
    el.title = `${btn.label} (${btn.edge} @ ${(btn.at * 100).toFixed(0)}%)`;
    bezelEl.appendChild(el);
    const wired = wireButton(
      el,
      {
        onDown: () => emuButtonDown(index),
        onUp: () => emuButtonUp(index),
        onVerdict: (isLong) => emuButtonVerdict(index, isLong),
      },
      btn.longPressMs
    );
    wiredButtons.push(wired);

    // Held via the keyboard, which is what makes a two-button gesture
    // (this device's app switch) actually performable: a mouse only has
    // one pointer and can hold at most one button at a time, but two
    // physical keys can be held together on a real keyboard, exactly like
    // two fingers on two side buttons.
    const key = assignShortcut(btn.id, usedKeys);
    if (key) shortcuts.bindHeld(key, { down: wired.down, up: wired.up });

    const row = document.createElement("div");
    row.className = "shortcut-row";
    row.innerHTML =
      `<span class="kbd">${key ? key.toUpperCase() : "-"}</span> ${escapeHtml(btn.label)} ` +
      `<span class="hint">${btn.edge} @ ${(btn.at * 100).toFixed(0)}%${btn.longPressMs ? `, hold ${btn.longPressMs}ms = long` : ""}</span>`;
    shortcutListEl.appendChild(row);
  });

  if (emu) {
    buildSensorControls($("#sensorControls"), d.sensors || [], emu, shortcuts, usedKeys, (t) => consoleLog.push(t));
    appStripControl = buildAppStrip($("#appStrip"), d.apps || [], emu);
  }

  shakeSensorIndex = (d.sensors || []).findIndex((s) => s.id.toLowerCase() === "shake");

  touchEnabled = (d.touch?.points ?? 0) > 0;
  panelEl.style.cursor = touchEnabled ? "crosshair" : "default";

  touchOverlay.pxPerMm = derivePxPerMm(d);
  refreshContactInfo();
  buildGestures(d);
  centerDeviceOnce();
}

// ---- gestures: "how do I double press so I can really test this" -------
// PROPOSED addition to emu_device()'s JSON (device.gestures, see wasm.ts):
// a compound gesture the runtime recognises across multiple inputs (a menu,
// an app switch) has no single button to hang a label on, and this device's
// own gesture is actively changing (see the task report), so it must come
// from the descriptor rather than prose written here that goes stale the
// next time it changes.
function buildGestures(d: DeviceDescriptor): void {
  const wrap = $("#gesturesWrap");
  const list = $("#gesturesList");
  list.innerHTML = "";
  if (!d.gestures || d.gestures.length === 0) {
    wrap.classList.remove("hidden");
    list.innerHTML =
      '<div class="hint">this build does not declare its gestures yet ' +
      "(proposed: emu_device()'s JSON gains a <code>gestures</code> array of " +
      "{id, label, how}; see this task's report). Ask on the board, don't guess from here.</div>";
    return;
  }
  wrap.classList.remove("hidden");
  for (const g of d.gestures) {
    const row = document.createElement("div");
    row.className = "gesture-row";
    row.innerHTML = `<b>${escapeHtml(g.label)}</b><br><span class="how">${escapeHtml(g.how)}</span>`;
    list.appendChild(row);
  }
}

// ---- centre the puck over the stage, once, on first load ----------------
function centerDeviceOnce(): void {
  if (centeredOnce) return;
  const stageRect = stageEl.getBoundingClientRect();
  const bw = bezelEl.offsetWidth;
  const bh = bezelEl.offsetHeight;
  if (bw === 0 || bh === 0 || stageRect.width === 0) return; // not laid out yet
  deviceWrapEl.style.left = `${Math.max(20, Math.round((stageRect.width - bw) / 2))}px`;
  deviceWrapEl.style.top = `${Math.max(20, Math.round((stageRect.height - bh) / 2))}px`;
  centeredOnce = true;
}

// ---- load / reload ----
// Guards against overlapping reloads: the server already waits for the
// wasm file to go quiet before broadcasting (server.ts, waitForStableFile),
// but a manual retry click landing while a reload triggered by that
// broadcast is still in flight would otherwise race two bringUp() calls
// against the same `emu` slot. Simpler to just refuse the second one; the
// first will finish in well under a second either way.
let reloadInFlight = false;

async function reloadModule(reason: string): Promise<void> {
  if (reloadInFlight) {
    consoleLog.push(`reload already in progress, ignoring (${reason})`);
    return;
  }
  reloadInFlight = true;
  consoleLog.push(`loading wasm module (${reason})...`);
  try {
    let bytes: ArrayBuffer;
    try {
      bytes = await fetchWasmBytes(`${DEFAULT_WASM_URL}?t=${Date.now()}`);
    } catch (err) {
      failReload(err);
      return;
    }
    await bringUp(bytes, reason);
  } finally {
    reloadInFlight = false;
  }
}

async function bringUp(bytes: ArrayBuffer, reason: string): Promise<void> {
  let newEmu: EmuExports;
  try {
    newEmu = await instantiate(bytes, (text) => consoleLog.push(text));
  } catch (err) {
    failReload(err);
    return;
  }
  if (newEmu.emu_init() === 0) {
    failReload(new Error("emu_init() returned 0 (framebuffer allocation failed; see console pane above for why)"));
    return;
  }
  let newDevice: DeviceDescriptor;
  let reader: PixelReader;
  try {
    newDevice = readDeviceDescriptor(newEmu);
    reader = pixelReaderFor(newDevice.panel.format);
  } catch (err) {
    failReload(err);
    return;
  }

  hideWasmError();
  wasmBytes = bytes;
  const prevAppIndex = lastAppIndex;
  emu = newEmu;
  device = newDevice;
  panelW = newDevice.panel.w;
  panelH = newDevice.panel.h;
  fbPtr = newEmu.emu_fb();
  pixelReader = reader;

  if (touchSim) touchSim.setBounds(panelW, panelH);
  else touchSim = new TouchSim(touchCfg, panelW, panelH);

  buildChrome(newDevice);

  if (newDevice.apps && newDevice.apps.length > 0 && newEmu.emu_app_switch && newEmu.emu_app_current) {
    const idx = Math.min(prevAppIndex, newDevice.apps.length - 1);
    if (idx !== newEmu.emu_app_current()) {
      newEmu.emu_app_switch(idx);
      consoleLog.push(
        `resumed at app "${newDevice.apps[idx]}" (index preserved across reload; the app's own state was ` +
          `NOT, since the arena resets on every init/switch -- this is a "which app", not a "where it was")`
      );
      appStripControl?.refresh();
    }
  }

  // Painted immediately, not left to wait for the next tick's partial
  // pushes: the screen must never show a stale frame from a previous
  // module after a reload.
  blitAll(panelCtx, newEmu.memory, fbPtr, panelW, panelH, reader);
  consoleLog.push(`ready: ${newDevice.name || "device"} ${panelW}x${panelH} ${newDevice.panel.format} (${reason})`);
  updateReloadStatus(bytes.byteLength);
}

// ---- readouts ----
function updateTouchReadout(mapped: { panel: { x: number; y: number }; view: { x: number; y: number }; viewW: number; viewH: number }): void {
  touchReadoutEl.textContent = `panel ${mapped.panel.x},${mapped.panel.y}   view ${mapped.view.x},${mapped.view.y} (${mapped.viewW}x${mapped.viewH})`;
}
function updatePushReadout(): void {
  pushReadoutEl.textContent = `pushes ${pushOverlay.lastCount}   last w ${pushOverlay.lastWidth}px`;
}
function updateReplayBar(): void {
  if (!replayer) {
    replayBarEl.classList.add("hidden");
    btnStopReplay.classList.add("hidden");
    return;
  }
  const p = replayer.progress;
  replayBarEl.classList.remove("hidden");
  btnStopReplay.classList.remove("hidden");
  replayBarEl.textContent = `replaying: ${p.at}/${p.total}${paused ? " (paused)" : ""}`;
}

// ---- panel input: mouse/touch -> panel coordinates -> emu_touch ----
function wirePanelInput(): void {
  panelEl.addEventListener("pointerdown", (e) => {
    if (!touchEnabled || replayer) return;
    pointerIdDown = e.pointerId;
    panelEl.setPointerCapture(e.pointerId);
    const m = mapClientPoint(e.clientX, e.clientY, panelEl, quickDeg, tiltDeg, panelW, panelH);
    liveTouch = { fingers: 1, x: m.panel.x, y: m.panel.y };
    touchSim?.setPointer(true, m.panel.x, m.panel.y);
    updateTouchReadout(m);
    e.preventDefault();
  });
  panelEl.addEventListener("pointermove", (e) => {
    if (!touchEnabled) return;
    const m = mapClientPoint(e.clientX, e.clientY, panelEl, quickDeg, tiltDeg, panelW, panelH);
    if (pointerIdDown === e.pointerId) {
      liveTouch = { fingers: 1, x: m.panel.x, y: m.panel.y };
      touchSim?.setPointer(true, m.panel.x, m.panel.y);
      updateTouchReadout(m);
    } else if (pointerIdDown === null && !replayer) {
      touchOverlay.recordHover(m.panel.x, m.panel.y);
      updateTouchReadout(m);
    }
  });
  function release(e: PointerEvent): void {
    if (pointerIdDown !== e.pointerId) return;
    pointerIdDown = null;
    liveTouch = { fingers: 0, x: liveTouch.x, y: liveTouch.y };
    touchSim?.setPointer(false, liveTouch.x, liveTouch.y);
  }
  panelEl.addEventListener("pointerup", release);
  panelEl.addEventListener("pointercancel", release);
  panelEl.addEventListener("pointerleave", () => {
    if (pointerIdDown === null) touchOverlay.recordHover(null, null);
  });
}

// ---- the tick loop ----
function stepOnce(): void {
  if (!emu || !pixelReader) return;
  if (replayer) {
    const t = replayer.stepFrame(emu);
    if (t === null) {
      consoleLog.push("replay finished");
      paused = true;
      btnPause.textContent = "resume";
    } else {
      afterTick(t);
    }
    updateReplayBar();
    return;
  }
  const now = performance.now();
  if (touchEnabled) {
    const report: TouchReport = touchDefectsEnabled && touchSim ? touchSim.poll(now) : liveTouch;
    emu.emu_touch(report.fingers, Math.round(report.x), Math.round(report.y));
    recorder.record({ t: now, k: "touch", down: report.fingers, x: Math.round(report.x), y: Math.round(report.y) });
    touchOverlay.recordTouch(report.fingers === 1, report.x, report.y, now);
  }
  emu.emu_tick(now);
  recorder.record({ t: now, k: "tick" });
  afterTick(now);
}

function afterTick(now: number): void {
  if (!emu || !pixelReader) return;
  const rects = readPushes(emu);
  for (const r of rects) blitRect(panelCtx, emu.memory, fbPtr, panelW, pixelReader, r);
  pushOverlay.record(rects, now);
  for (const r of rects) pushHistory.push({ tMs: now, ...r });
  while (pushHistory.length > PUSH_HISTORY_MAX) pushHistory.shift();
  if (device?.apps && device.apps.length > 0 && emu.emu_app_current) lastAppIndex = emu.emu_app_current();
  appStripControl?.refresh();
  updatePushReadout();
}

function frame(): void {
  if (emu && !paused) stepOnce();
  const now = performance.now();
  overlayCtx.clearRect(0, 0, overlayEl.width, overlayEl.height);
  pushOverlay.paint(overlayCtx, now, accentColor);
  touchOverlay.paint(overlayCtx, now, accentColor);
  traceStatusEl.textContent = `${recorder.events.length.toLocaleString()} events recorded`;
  pollWindowShake(now);
  requestAnimationFrame(frame);
}

// ---- shake by physically shaking the window ------------------------------
function pollWindowShake(now: number): void {
  const suppressed = liveTouch.fingers === 1 || pointerIdDown !== null;
  const shaken = windowShake.poll(window.screenX, window.screenY, now, suppressed);
  shakeReadoutEl.textContent = `window jolts: ${windowShake.lastJoltCount}/${windowShake.cfg.joltMinCount} (shake the whole browser window, or press the button/key above)`;
  if (shaken && emu && shakeSensorIndex >= 0 && !replayer) {
    emu.emu_sensor_event(shakeSensorIndex);
    recorder.record({ t: now, k: "sensor", i: shakeSensorIndex });
    consoleLog.push("shake: window jolt accepted");
  }
}

// ---- replay ----
function startReplay(trace: Trace): void {
  if (!wasmBytes) {
    consoleLog.push("cannot replay: no wasm module loaded yet");
    return;
  }
  recorder.enabled = false;
  paused = true;
  btnPause.textContent = "resume";
  void bringUp(wasmBytes, `replay of trace recorded ${trace.recordedAt}`).then(() => {
    replayer = new Replayer(trace.events);
    updateReplayBar();
    consoleLog.push(`replay loaded: ${trace.events.length} events. step or resume to play.`);
  });
}
function stopReplay(): void {
  replayer = null;
  recorder.enabled = true;
  paused = false;
  btnPause.textContent = "pause";
  updateReplayBar();
  void reloadModule("resuming live input after replay");
}
function wireTraceFile(): void {
  const input = $<HTMLInputElement>("#traceFileInput");
  input.addEventListener("change", () => {
    const file = input.files?.[0];
    input.value = "";
    if (!file) return;
    void (async () => {
      try {
        const text = await file.text();
        const trace = JSON.parse(text) as Trace;
        if (!Array.isArray(trace.events)) throw new Error("not a trace file (missing events array)");
        startReplay(trace);
      } catch (err) {
        consoleLog.push(`could not load trace: ${errMsg(err)}`);
      }
    })();
  });
}

// ---- freeze / trace save ----
async function runFreeze(): Promise<void> {
  if (!emu || !device) {
    consoleLog.push("cannot freeze: no wasm module loaded");
    return;
  }
  const panelPngBase64 = canvasToPngBase64(panelEl);
  const currentApp = device.apps && device.apps.length > 0 && emu.emu_app_current ? device.apps[emu.emu_app_current()] ?? null : null;
  const bundle: FreezeBundle = {
    schemaVersion: 1,
    capturedAt: new Date().toISOString(),
    device,
    currentApp,
    pushes: pushHistory.slice(-200),
    input: recorder.recent(200),
    console: consoleLog.recent(100),
    journal: emptyJournal(),
    panelPngBase64,
  };
  const first = await postFreeze(bundle);
  if (!first.ok) {
    consoleLog.push(`freeze failed: ${first.error}`);
    return;
  }
  consoleLog.push(`frozen -> ${first.path}`);
  const journal = await openAnnotationModal(panelPngBase64, panelW, panelH);
  if (journal && (journal.strokes.length > 0 || journal.notes.length > 0)) {
    const second = await postFreeze({ ...bundle, journal }, first.id);
    consoleLog.push(second.ok ? `annotations saved -> ${second.path}` : `saving annotations failed: ${second.error}`);
  }
}

async function saveTraceToServer(): Promise<void> {
  if (!device) {
    consoleLog.push("nothing to save yet");
    return;
  }
  const trace = recorder.toTrace(device);
  try {
    const res = await fetch("/api/trace", { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify(trace) });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = (await res.json()) as { path: string };
    consoleLog.push(`trace saved -> ${data.path}`);
  } catch (err) {
    consoleLog.push(`save trace failed: ${errMsg(err)}`);
  }
}

// ---- live reload: watch server tells us when the wasm build changed ----
function connectLiveReload(): void {
  const dot = $("#reloadDot");
  try {
    const ws = new WebSocket(`ws://${location.host}/api/livereload`);
    ws.addEventListener("open", () => dot.classList.add("connected"));
    ws.addEventListener("close", () => {
      dot.classList.remove("connected");
      setTimeout(connectLiveReload, 2000);
    });
    ws.addEventListener("error", () => ws.close());
    ws.addEventListener("message", (e) => {
      if (e.data === "reload" && !replayer) void reloadModule("wasm file changed on disk");
    });
  } catch {
    // No websocket route to connect to (e.g. served statically through
    // markup, which is read-only): fine, just no live reload there.
  }
}

// ---- touch controller sliders ----
function buildTouchControls(): void {
  const el = $("#touchControls");
  el.innerHTML = `
    <div class="slider-row">
      <div class="row-head"><b>report rate</b><span id="rrVal">${touchCfg.reportRateHz}Hz</span></div>
      <input id="rrInput" type="range" min="10" max="240" step="5" value="${touchCfg.reportRateHz}" />
    </div>
    <div class="toggle-row">
      <input id="dropoutsOn" type="checkbox" ${touchCfg.dropoutsEnabled ? "checked" : ""} />
      <label for="dropoutsOn">dropouts mid-stroke</label>
    </div>
    <div class="slider-row">
      <div class="row-head"><b>dropout rate</b><span id="dpVal">${touchCfg.dropoutsPerSec}/s</span></div>
      <input id="dpInput" type="range" min="0" max="5" step="0.1" value="${touchCfg.dropoutsPerSec}" />
    </div>
    <div class="toggle-row">
      <input id="straysOn" type="checkbox" ${touchCfg.straysEnabled ? "checked" : ""} />
      <label for="straysOn">stray contacts</label>
    </div>
    <div class="slider-row">
      <div class="row-head"><b>stray rate</b><span id="stVal">${touchCfg.straysPerSec}/s</span></div>
      <input id="stInput" type="range" min="0" max="2" step="0.05" value="${touchCfg.straysPerSec}" />
    </div>
  `;
  $<HTMLInputElement>("#rrInput").addEventListener("input", (e) => {
    touchCfg.reportRateHz = Number((e.target as HTMLInputElement).value);
    $("#rrVal").textContent = `${touchCfg.reportRateHz}Hz`;
  });
  $<HTMLInputElement>("#dropoutsOn").addEventListener("change", (e) => {
    touchCfg.dropoutsEnabled = (e.target as HTMLInputElement).checked;
  });
  $<HTMLInputElement>("#dpInput").addEventListener("input", (e) => {
    touchCfg.dropoutsPerSec = Number((e.target as HTMLInputElement).value);
    $("#dpVal").textContent = `${touchCfg.dropoutsPerSec}/s`;
  });
  $<HTMLInputElement>("#straysOn").addEventListener("change", (e) => {
    touchCfg.straysEnabled = (e.target as HTMLInputElement).checked;
  });
  $<HTMLInputElement>("#stInput").addEventListener("input", (e) => {
    touchCfg.straysPerSec = Number((e.target as HTMLInputElement).value);
    $("#stVal").textContent = `${touchCfg.straysPerSec}/s`;
  });
}

// ---- static UI: everything not dependent on a loaded module ----
function wireStaticUI(): void {
  wireContactPresets();
  refreshContactInfo();

  makeDraggable(bezelEl, deviceWrapEl);
  wirePanelInput();
  connectLiveReload();
  wireTraceFile();

  $("#rotQuick")
    .querySelectorAll<HTMLButtonElement>("button")
    .forEach((b) => {
      b.addEventListener("click", () => {
        quickDeg = Number(b.dataset.deg);
        $("#rotQuick")
          .querySelectorAll("button")
          .forEach((x) => x.classList.remove("active"));
        b.classList.add("active");
        applyRotation(bezelEl, quickDeg + tiltDeg);
      });
    });
  $<HTMLInputElement>("#tilt").addEventListener("input", (e) => {
    tiltDeg = Number((e.target as HTMLInputElement).value);
    applyRotation(bezelEl, quickDeg + tiltDeg);
  });

  btnPause.addEventListener("click", () => {
    paused = !paused;
    btnPause.textContent = paused ? "resume" : "pause";
  });
  $<HTMLButtonElement>("#btnStep").addEventListener("click", () => stepOnce());
  btnStopReplay.addEventListener("click", stopReplay);

  $<HTMLButtonElement>("#btnPng").addEventListener("click", () => {
    panelEl.toBlob((blob) => {
      if (!blob) return;
      const a = document.createElement("a");
      a.href = URL.createObjectURL(blob);
      a.download = `panel-${new Date().toISOString().replace(/[:.]/g, "-")}.png`;
      a.click();
      URL.revokeObjectURL(a.href);
    }, "image/png");
  });

  $<HTMLButtonElement>("#btnFreeze").addEventListener("click", () => {
    void runFreeze();
  });
  $<HTMLButtonElement>("#btnSaveTrace").addEventListener("click", () => {
    void saveTraceToServer();
  });

  $<HTMLInputElement>("#touchDefectsOn").addEventListener("change", (e) => {
    touchDefectsEnabled = (e.target as HTMLInputElement).checked;
  });
  buildTouchControls();

  $<HTMLButtonElement>("#btnResetTouch").addEventListener("click", () => {
    Object.assign(touchCfg, TOUCHSIM_DEFAULTS);
    touchDefectsEnabled = TOUCH_DEFECTS_DEFAULT;
    $<HTMLInputElement>("#touchDefectsOn").checked = touchDefectsEnabled;
    buildTouchControls();
  });

  $<HTMLButtonElement>("#btnQuit").addEventListener("click", () => {
    void (async () => {
      await fetch("/api/quit", { method: "POST", headers: { "x-rp2350-emulator": "1" } });
      document.body.innerHTML =
        '<div style="display:flex;align-items:center;justify-content:center;height:100vh;font:14px monospace;color:#888">emulator stopped. you can close this window.</div>';
    })();
  });
}

// ---- boot ----
async function boot(): Promise<void> {
  accentColor = getComputedStyle(document.documentElement).getPropertyValue("--accent").trim() || accentColor;
  wireStaticUI();
  await reloadModule("initial load");
  requestAnimationFrame(frame);
}

// Exposed for headless verification (scripts/verify.ts and ad hoc checks),
// same convention the previous emulator used (window.__engine). Not used
// by the UI itself.
(window as unknown as { __debug: unknown }).__debug = {
  reloadModule,
  getEmu: () => emu,
  getDevice: () => device,
  getRecorder: () => recorder,
};

void boot();
