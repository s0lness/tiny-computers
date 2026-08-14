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
import { makeDraggable, wireButton, createButtonElement, applyRotation, type WiredButton, type ButtonEvents } from "./device";
import { buildSensorControls } from "./sensors";
import { buildTuneControls } from "./tunables";
import { buildAppStrip, type AppStripControl } from "./appstrip";
import { ShortcutRegistry, assignShortcut } from "./shortcuts";
import { ConsoleLog, type LogLine } from "./consolelog";
import { DevlinkClient, type DevlinkStatus } from "./devlinkClient";
import { Recorder, type Trace } from "./recorder";
import { Replayer } from "./replay";
import { postFreeze, canvasToPngBase64, openAnnotationModal, type FreezeBundle } from "./freeze";
import { emptyJournal } from "./journal";
import { TouchSim, type TouchReport } from "./touchsim";
import { type TouchSimConfig, TOUCHSIM_DEFAULTS, TOUCH_DEFECTS_DEFAULT } from "./constants";
import { WindowShakeDetector } from "./windowshake";
import { PuckMotion } from "./puckmotion";
import { SoundPlayer } from "./audio";

const $ = <T extends HTMLElement>(sel: string) => document.querySelector<T>(sel)!;

function errMsg(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

// ---- mute icon: the control must show which state it is in ----
// Owner feedback: toggling mute changed nothing on screen, so a control
// that looks identical either way is a guess, not a toggle. Same
// stroke/size/viewBox as index.html's original speaker icon (feather's
// "volume-2"/"volume-x" pair), swapped wholesale rather than trying to
// morph one path into the other.
const ICON_UNMUTED = `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
  <path d="M4 9v6h4l5 5V4L8 9H4z" />
  <path d="M16 8.5a4.5 4.5 0 0 1 0 7" />
  <path d="M19 6a8 8 0 0 1 0 12" />
</svg>`;
const ICON_MUTED = `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
  <path d="M4 9v6h4l5 5V4L8 9H4z" />
  <line x1="23" y1="9" x2="17" y2="15" />
  <line x1="17" y1="9" x2="23" y2="15" />
</svg>`;
function setMuteIcon(btn: HTMLElement, muted: boolean): void {
  btn.innerHTML = muted ? ICON_MUTED : ICON_UNMUTED;
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
const diagStripEl = $("#diagStrip");
const replayBarEl = $("#replayBar");
const btnStopReplay = $<HTMLButtonElement>("#btnStopReplay");
const btnPause = $<HTMLButtonElement>("#btnPause");
const stageEl = $("#stage");
const devlinkStatusEl = $("#devlinkStatus");

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
// Detection (an unmistakable page indicator, updated with no reload the
// instant a board appears or disappears), single ownership of the real
// serial port (this page never opens it itself, only server.ts's
// DevlinkHost does - see devlinkClient.ts and server.ts), the board's own
// console lines (tagged apart from the emulated firmware's, see
// appendConsoleLine below), and the tuning panel's device column
// (tunables.ts) all come from this one client.
const devlinkClient = new DevlinkClient();
const pushOverlay = new PushOverlay();
const touchOverlay = new TouchOverlay();
const shortcuts = new ShortcutRegistry();
const windowShake = new WindowShakeDetector();
// A second instance of the SAME detector shape, fed from dragging the puck
// itself on the page (device.ts's makeDraggable, onDrag callback) rather
// than the browser window's OS position. This is the reliable shake path:
// see its wiring in wireStaticUI for why the window path cannot be trusted
// on Windows (a real titlebar drag can stop the renderer from getting
// animation frames at all, which no threshold tuning fixes).
const puckDragShake = new WindowShakeDetector();
const puckMotion = new PuckMotion();
const soundPlayer = new SoundPlayer();
let shakeSensorIndex = -1;

// The puck's position, stored as a displacement from the STAGE'S OWN
// CENTRE (0,0 = dead centre) rather than an absolute left/top pixel - see
// positionDevice() for why. Only a drag (device.ts's makeDraggable) ever
// changes these; a resize or a firmware reload re-reads them but never
// writes them.
let deviceOffsetX = 0;
let deviceOffsetY = 0;

// The overlay toggle (disc, trail and coordinate readout together, one
// switch). Default OFF per owner feedback: it was on by default and that
// was wrong, the toggle stays exactly where it is, only the default flips.
let overlayEnabled = false;

// Diagnostics kept for the one-line strip at the bottom of the page
// (main.ts's redesign: coordinates, push counts, reload status and shake
// jolts belong together, not scattered across the chrome).
let lastTouchMapped: { panel: { x: number; y: number }; view: { x: number; y: number } } | null = null;
let lastReloadStatus = "";

const touchCfg: TouchSimConfig = { ...TOUCHSIM_DEFAULTS };
let touchDefectsEnabled = TOUCH_DEFECTS_DEFAULT;
let touchSim: TouchSim | null = null;
let liveTouch: TouchReport = { fingers: 0, x: 0, y: 0 };
let pointerIdDown: number | null = null;

// -90, not 0: the panel framebuffer is portrait, but every landscape app
// (the menu, chrono, timer) draws through gfx.c's rotation into it, per
// gfx.h's mapping landscape (lx, ly) -> panel (PANEL_W-1-ly, lx). Checked
// empirically with a real screenshot of the menu, which has asymmetric text
// (CHRONO / DRAW / TIMER) rather than trusting the geometry by eye: at
// quickDeg=0 the words read sideways bottom-to-top, at +90 they read upside
// down and right-to-left, and only -90 reads left-to-right, right way up.
// A symmetric screen like chrono's "00:00" would have looked identical at
// any of these and hidden the bug.
let quickDeg = -90;
let tiltDeg = 0;

let wiredButtons: WiredButton[] = [];
// Keyed by the declared button id (DeviceButton.id), not array index: this
// is what lets performChord resolve "boot" back to the exact same
// WiredButton a real pointer/keyboard press already drives.
let wiredButtonById = new Map<string, WiredButton>();
// Every DOM element that represents one declared button, id-keyed same as
// wiredButtonById: [bezel element, toolbox chip]. Used by performChord's
// visual flash of PWR below, which deliberately does NOT go through PWR's
// own WiredButton (see performChord's comment on why) and so needs the raw
// elements to add/remove a class on directly - both of them, so the chord
// shows the same hold on the toolbox chip as it does on the bezel.
let buttonElById = new Map<string, HTMLElement[]>();
let appStripControl: AppStripControl | null = null;
let lastAppIndex = 0;

let paused = false;
let replayer: Replayer | null = null;

const pushHistory: { tMs: number; x: number; y: number; w: number; h: number }[] = [];
const PUSH_HISTORY_MAX = 400;

// ---- console pane ----
// Two machines can speak into this one pane: the emulated firmware's own
// printf (source "fw", the historical and still-default case, rendered
// exactly as before this task) and, once a real board is connected, the
// board's own serial output (source "device" - see devlinkClient.ts's
// "line" messages, wired below in wireStaticUI). A "dev" tag plus a
// distinct colour (app.css) marks the latter: two near-identical streams
// of text with no way to tell which machine is speaking would be its own
// bug, the same confusion the tuning panel's side-by-side columns
// (tunables.ts) exist to prevent for the knobs.
function appendConsoleLine(line: LogLine): void {
  const div = document.createElement("div");
  div.className = line.source === "device" ? "console-line console-line-device" : "console-line";
  if (line.source === "device") {
    const tag = document.createElement("span");
    tag.className = "console-line-tag";
    tag.textContent = "dev";
    div.appendChild(tag);
    div.appendChild(document.createTextNode(line.text));
  } else {
    div.textContent = line.text;
  }
  consolePaneEl.appendChild(div);
  while (consolePaneEl.childElementCount > 300) consolePaneEl.removeChild(consolePaneEl.firstChild!);
  consolePaneEl.scrollTop = consolePaneEl.scrollHeight;
}

// ---- devlink status: unmistakable, updated with no reload ----
// Owner's own framing: "quand il faut que ça remarque si il y a un device
// qui est plugé" - plugging or unplugging while the page is open must be
// visible immediately. devlinkClient.ts pushes a status message the instant
// server.ts's DevlinkHost notices a change (see that file's PowerShell
// watcher); this just renders whatever it says. Text, not just a coloured
// dot (the reload-dot's own mistake would be repeating here): a dot alone
// is not "plainly says no device is present", which the connected-UI test
// (scripts/verify-ui-feedback.ts) checks for in words.
//
// Three states, not two (see docs/decisions/0004-the-day-the-instruments-
// lied.md): "device: COM4" must mean commands will actually reach it, so a
// board that is present but not yet openable (another process holding the
// port, or still settling) gets its own honest label instead of being
// folded into either "connected" or "no device" - both of which would be a
// lie here, one way or the other.
function updateDevlinkStatusUI(status: DevlinkStatus): void {
  devlinkStatusEl.classList.toggle("connected", status.state === "connected");
  devlinkStatusEl.classList.toggle("unavailable", status.state === "unavailable");
  if (status.state === "connected") {
    devlinkStatusEl.textContent = `device: ${status.port}`;
    devlinkStatusEl.title = `a board is connected on ${status.port} - tools/dev.ts routes through this page's server instead of opening the port itself`;
  } else if (status.state === "unavailable") {
    devlinkStatusEl.textContent = `${status.port}: unavailable`;
    devlinkStatusEl.title = `a board is present on ${status.port} but the connection is not usable yet: ${status.reason ?? "unknown reason"} - retrying automatically, no restart needed`;
  } else {
    devlinkStatusEl.textContent = "no device";
    devlinkStatusEl.title = "no board detected on this machine's USB (VID 2E8A) - plug one in, no reload needed";
  }
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

// "reloaded HH:MM:SS Nb" folded into the one diagnostics strip (see
// updateDiagStrip) so a stale or failed reload is obvious at a glance
// without a dedicated line of chrome for it.
function updateReloadStatus(byteLength: number): void {
  const t = new Date();
  const pad = (n: number) => String(n).padStart(2, "0");
  lastReloadStatus = `reloaded ${pad(t.getHours())}:${pad(t.getMinutes())}:${pad(t.getSeconds())} ${byteLength.toLocaleString()}b`;
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
// Finger size is a quantity (contact diameter, millimetres); naming only
// "adult"/"child" hides that number and makes it unreachable, which is
// backwards for a tool whose job is showing how big a finger really is
// against a layout. The numeric input is the primary control; the presets
// are one-click shortcuts to a value, not the only values. The px figure
// is the one the layout is actually judged against (it depends on the
// panel's declared geometry, not the millimetres alone), shown as a plain
// number next to the mm input, no sentence around either.
function refreshContactInfo(): void {
  const mmInput = $<HTMLInputElement>("#contactMm");
  // Never stomp what's mid-typed: only sync the field's displayed value
  // when it is not the thing focused right now.
  if (document.activeElement !== mmInput) mmInput.value = String(touchOverlay.contactMm);
  const px = Math.round(touchOverlay.contactMm * touchOverlay.pxPerMm);
  $("#contactPx").textContent = `${px}px`;
  $("#contactPreset")
    .querySelectorAll<HTMLButtonElement>("button")
    .forEach((b) => b.classList.toggle("active", Number(b.dataset.mm) === touchOverlay.contactMm));
}
function wireContactSize(): void {
  const mmInput = $<HTMLInputElement>("#contactMm");
  mmInput.addEventListener("input", () => {
    const v = Number(mmInput.value);
    if (Number.isFinite(v) && v > 0) {
      touchOverlay.contactMm = v;
      refreshContactInfo();
    }
  });

  const el = $("#contactPreset");
  el.innerHTML = "";
  CONTACT_PRESETS.forEach((preset) => {
    const b = document.createElement("button");
    b.textContent = preset.label;
    b.dataset.mm = String(preset.mm);
    b.title = `${preset.mm}mm`;
    b.addEventListener("click", () => {
      touchOverlay.contactMm = preset.mm;
      refreshContactInfo();
    });
    el.appendChild(b);
  });
  refreshContactInfo();
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
  bezelEl.querySelectorAll(".dev-btn").forEach((el) => el.remove());
  wiredButtons = [];
  wiredButtonById = new Map();
  buttonElById = new Map();
  shortcuts.clear();
  const usedKeys = new Set<string>();
  // Owner feedback: the device's own buttons belong with the other
  // physical-manipulation controls (tilt, rotate) at the bottom, not in
  // the sidebar. The bezel button stays too (it is the one place button
  // POSITION is shown, real geometry per emu_abi.h, not decoration); this
  // is a second, always-reachable way to press the same button, built the
  // same generic way (nothing here names "boot" or "pwr"), and pressing
  // either one drives the exact same emu_button() call. The sidebar's old
  // text-only shortcut legend is gone: the toolbox chip IS the legend now,
  // it shows its own key and does something when clicked.
  const buttonToolboxEl = $("#buttonToolbox");
  buttonToolboxEl.innerHTML = "";

  const buttons = d.buttons || [];
  buttons.forEach((btn, index) => {
    const el = createButtonElement(btn.edge, btn.at, bezelEl.clientWidth, bezelEl.clientHeight);
    el.title = `${btn.label} (${btn.edge} @ ${(btn.at * 100).toFixed(0)}%)`;
    bezelEl.appendChild(el);

    // Computed before the chip so the chip's own label can use it below.
    const key = assignShortcut(btn.id, usedKeys);

    // The toolbox chip: a second, always-reachable place to press the same
    // logical button, at toolbar scale.
    const chip = document.createElement("button");
    chip.className = "toolbox-btn";
    chip.textContent = key ? key.toUpperCase() : btn.label.slice(0, 1).toUpperCase();
    chip.title = `${btn.label}${btn.longPressMs ? ` (hold ${btn.longPressMs}ms = long)` : ""}`;
    buttonToolboxEl.appendChild(chip);

    buttonElById.set(btn.id, [el, chip]);

    const events: ButtonEvents = {
      onDown: () => emuButtonDown(index),
      onUp: () => emuButtonUp(index),
      onVerdict: (isLong) => emuButtonVerdict(index, isLong),
    };
    // ONE WiredButton driving BOTH elements: pressed/holding/long state is
    // owned here, once, and applied to the bezel button and the toolbox
    // chip together (see device.ts's wireButton). A pointer down on either
    // element presses both; a pointer up (wherever it lands, per pointer
    // capture) releases both. This is what makes the bezel - the one place
    // button POSITION is real device geometry - a trustworthy picture of
    // the device no matter which control was actually clicked.
    const wired = wireButton([el, chip], events, btn.longPressMs);
    wiredButtons.push(wired);
    wiredButtonById.set(btn.id, wired);

    // Held via the keyboard, which is what makes a two-button gesture
    // (this device's app switch) actually performable: a mouse only has
    // one pointer and can hold at most one button at a time, but two
    // physical keys can be held together on a real keyboard, exactly like
    // two fingers on two side buttons.
    if (key) {
      shortcuts.bindHeld(key, { down: wired.down, up: wired.up });
    }
  });

  if (emu) {
    // Same relocation, same reasoning, as the button toolbox above: the
    // sidebar's #sensorControls is gone, buildSensorControls now fills the
    // toolbox instead. Nothing here changed about what it builds (still
    // one control per declared "event" sensor, still generic).
    buildSensorControls($("#sensorToolbox"), d.sensors || [], emu, shortcuts, usedKeys, (t) => consoleLog.push(t), (sensor) => {
      // The one place a sensor click drives something visible beyond the
      // firmware event itself: "shake" gets the same puck motion a real
      // window shake produces, since there is no window motion behind a
      // click to derive it from otherwise (see puckmotion.ts).
      // Magnitude picked so the peak displacement lands close to
      // PuckMotion's own maxOffset (a clearly visible jolt, not a twitch):
      // for this spring's stiffness, a velocity kick V settles near a peak
      // of V/sqrt(stiffness) px, so 300-400 px/s of kick reads as a real
      // shake rather than a 1-2px flicker (measured with puppeteer: an
      // earlier, gentler kick here was visually indistinguishable from
      // noise).
      if (sensor.id.toLowerCase() === "shake") puckMotion.impulse((Math.random() - 0.5) * 500, (Math.random() - 0.5) * 380);
    }, (_sensor, index, x, y, z) => {
      // A vector sensor's value is STICKY (emu_abi.h), so it belongs in the
      // trace like any other input: a replay that skipped it would run the
      // whole recorded session at the module's boot pose, and an
      // orientation-driven app would do something else entirely. Recorded
      // with the current tick's timestamp, same as every other input event.
      recorder.record({ t: performance.now(), k: "sensorv", i: index, x, y, z });
    });
    appStripControl = buildAppStrip($("#appStrip"), d.apps || [], emu);

    // Live tunables (emu_abi.h's "tunables"): hidden entirely when the
    // loaded module declares none AND no device is connected to reveal any
    // of its own (tunables.ts manages this wrap's visibility itself now,
    // since a connected board can have tunables the emulator build does
    // not, same "the wrap only shows up if there is something to put in
    // it" rule appStripWrap follows for apps just above, just no longer
    // decidable from the emulator's own descriptor alone).
    const tunables = d.tunables || [];
    buildTuneControls($("#tuneToolbox"), $("#tuneToolboxWrap"), tunables, emu, devlinkClient);
  }

  shakeSensorIndex = (d.sensors || []).findIndex((s) => s.id.toLowerCase() === "shake");

  touchEnabled = (d.touch?.points ?? 0) > 0;
  panelEl.style.cursor = touchEnabled ? "crosshair" : "default";

  touchOverlay.pxPerMm = derivePxPerMm(d);
  refreshContactInfo();
  updateChordButton(d);
  positionDevice();
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

// ---- the BOOT+PWR chord: opens/closes the app menu -----------------------
//
// Owner feedback: a one-press control for "press PWR and BOOT at the same
// time", the gesture that opens the real device's app menu. It drives the
// wasm ABI directly, and NOT by holding both buttons down for
// a real 1.5s the way a person would with two fingers: tools/dev.ts's own
// CHORD command (the same gesture, injected at the real device over serial)
// composes it from exactly three primitives - "hold BOOT, deliver PWR's
// long-press verdict, release BOOT one tick later" - and emu_shim.c's
// emu_button()/emu_button_verdict() confirm why: PWR's long/short verdict
// is its own discrete event (KEY_LONG), never bundled with a level toggle
// (unlike BOOT, a plain GPIO). Reproducing that with PWR's own WiredButton
// (bezel or toolbox) would be wrong twice over: it would ALSO emit a level
// toggle PWR never sends on a real chord, and it would need a real 1.5s
// wait (PWR's declared longPressMs) for its internal timer to fire the
// verdict, when the actual gesture is "deliver the verdict", not "wait for
// one". So BOOT goes through its real WiredButton (a plain level, exactly
// what inject_boot(true) does), and PWR's verdict is delivered directly
// through the same emuButtonVerdict() helper the bezel and toolbox already
// use - same ABI calls tools/dev.ts's CHORD produces on real hardware, none
// of the "hold for real time" approximation a two-separate-presses version
// would need.
const CHORD_RELEASE_DELAY_MS = 50; // "one tick later": comfortably longer
                                    // than one requestAnimationFrame (~16ms
                                    // at 60Hz), never a real 1.5s wait.
let chordInFlight = false;

async function performChord(): Promise<void> {
  if (!emu || !device || chordInFlight) return;
  const bootIndex = (device.buttons || []).findIndex((b) => b.id === "boot");
  const pwrIndex = (device.buttons || []).findIndex((b) => b.id === "pwr");
  const bootWired = wiredButtonById.get("boot");
  if (bootIndex < 0 || pwrIndex < 0 || !bootWired) {
    consoleLog.push("chord: this build does not declare both a \"boot\" and a \"pwr\" button");
    return;
  }
  chordInFlight = true;
  const chordBtn = $<HTMLButtonElement>("#btnChord");
  chordBtn.disabled = true;
  const pwrEls = buttonElById.get("pwr") || []; // [bezel element, toolbox chip]
  consoleLog.push("chord: BOOT+PWR (app menu)");
  try {
    bootWired.down(); // real level, same as a bezel/toolbox BOOT press - presses BOTH of boot's elements
    // Visual only: PWR's verdict below carries no level to react to, so
    // this is applied directly to both of PWR's elements (bezel + toolbox
    // chip) rather than going through PWR's own WiredButton. This must be a
    // real hold across the sleep below, not a flicker: it is the one thing
    // that makes the chord's gesture ("BOOT held, PWR's long verdict
    // delivered") legible as a hold rather than a blink.
    for (const el of pwrEls) el.classList.add("pressed", "long");
    emuButtonVerdict(pwrIndex, true); // PWR's long-press verdict, delivered directly
    await sleep(CHORD_RELEASE_DELAY_MS);
    bootWired.up();
  } finally {
    for (const el of pwrEls) el.classList.remove("pressed", "long");
    chordBtn.disabled = false;
    chordInFlight = false;
  }
}

function updateChordButton(d: DeviceDescriptor): void {
  const btn = $<HTMLButtonElement>("#btnChord");
  const has = (d.buttons || []).some((b) => b.id === "boot") && (d.buttons || []).some((b) => b.id === "pwr");
  btn.disabled = !has;
  btn.title = has ? "BOOT+PWR chord: opens/closes the app menu" : "this build does not declare both a \"boot\" and a \"pwr\" button";
}

// ---- position the puck relative to the stage's own centre ---------------
// Owner report: resizing the browser window left the rendered device off
// centre. The earlier code centred the puck once, on first load, by writing
// an absolute left/top pixel - correct for the window size at that moment,
// and never revisited, so any later resize left it pointing at whatever
// that stale pixel now landed on.
//
// The fix is what's stored, not just when this runs: deviceOffsetX/Y (see
// their declaration above) is a displacement FROM THE STAGE'S CENTRE, not
// an absolute position, and this function is the only thing that turns
// that displacement into an actual left/top. Both cases the owner cares
// about fall out of that for free:
//   - never dragged (0,0): every call recomputes "0 away from centre" for
//     whatever the stage's size is right now, i.e. still exactly centred -
//     the same behaviour already used for the bottom bar, which owner
//     pointed at as the model, just expressed per-element instead of via
//     position:fixed (the puck cannot use position:fixed, it has to stay
//     draggable within the stage).
//   - dragged (non-zero): a resize keeps that SAME displacement from the
//     new centre rather than snapping back to it. Deliberately NOT
//     recentring on resize: the puck is a physical object being carried
//     around on the page (see puckmotion.ts's own framing for the same
//     idea), and moving the desk does not teleport an object on it back to
//     the middle, it moves with the desk. Recentring on every resize would
//     also discard a placement the owner chose on purpose (e.g. dragged
//     clear of the sidebar) any time the window so much as changes size,
//     which would be a worse surprise than the bug this fixes.
//
// Called after every buildChrome() (initial load, live reload, and a
// firmware swap that changes the panel's own size) and on every window
// resize (see wireStaticUI); makeDraggable's onOffsetChange (below) is the
// only thing that ever changes what displacement gets applied.
function positionDevice(): void {
  const stageRect = stageEl.getBoundingClientRect();
  const bw = bezelEl.offsetWidth;
  const bh = bezelEl.offsetHeight;
  if (bw === 0 || bh === 0 || stageRect.width === 0) return; // not laid out yet
  const left = stageRect.width / 2 - bw / 2 + deviceOffsetX;
  const top = stageRect.height / 2 - bh / 2 + deviceOffsetY;
  deviceWrapEl.style.left = `${Math.round(left)}px`;
  deviceWrapEl.style.top = `${Math.round(top)}px`;
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
  soundPlayer.resetForReload(newEmu);

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

// ---- readouts: one quiet monospace strip, not scattered chrome ----------
// Coordinates, push counts, reload status and shake jolts are all
// diagnostics, and they read as one instrument when they live in one
// place. The coordinate segment only appears while the overlay (item 1)
// is switched on: it is part of that same toggle, not a separate readout
// that happens to survive turning the overlay off. The pixel format lives
// here too now, not in the device section: it names an internal encoding
// (a raw technical fact), which is exactly what this strip is for.
function updateDiagStrip(): void {
  const parts: string[] = [];
  if (overlayEnabled && lastTouchMapped) {
    parts.push(`${lastTouchMapped.panel.x},${lastTouchMapped.panel.y}`);
  }
  parts.push(`push ${pushOverlay.lastCount}×${pushOverlay.lastWidth}px`);
  // Only for a device that declared sound at all; "suspended" is what
  // makes the autoplay-policy case say so rather than playing nothing
  // silently, per emu_abi.h.
  if (emu?.emu_sound_play_seq) parts.push(`sound ${soundPlayer.status}`);
  // Whichever of the two detectors is actually seeing motion right now:
  // this is the "tell the user whether their shaking is being seen at
  // all" readout, and a person only ever tries one gesture at a time.
  const shakeCount = Math.max(windowShake.lastJoltCount, puckDragShake.lastJoltCount);
  parts.push(`shake ${shakeCount}/${windowShake.cfg.joltMinCount}`);
  if (device) parts.push(device.panel.format);
  parts.push(`${recorder.events.length.toLocaleString()} rec`);
  if (lastReloadStatus) parts.push(lastReloadStatus);
  diagStripEl.textContent = parts.join("   ·   ");
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
    lastTouchMapped = m;
    e.preventDefault();
  });
  panelEl.addEventListener("pointermove", (e) => {
    if (!touchEnabled) return;
    const m = mapClientPoint(e.clientX, e.clientY, panelEl, quickDeg, tiltDeg, panelW, panelH);
    if (pointerIdDown === e.pointerId) {
      liveTouch = { fingers: 1, x: m.panel.x, y: m.panel.y };
      touchSim?.setPointer(true, m.panel.x, m.panel.y);
      lastTouchMapped = m;
    } else if (pointerIdDown === null && !replayer) {
      if (overlayEnabled) touchOverlay.recordHover(m.panel.x, m.panel.y);
      lastTouchMapped = m;
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
  soundPlayer.poll(emu); // right after emu_tick(), per emu_abi.h's sound section
}

function frame(): void {
  if (emu && !paused) stepOnce();
  const now = performance.now();
  overlayCtx.clearRect(0, 0, overlayEl.width, overlayEl.height);
  if (overlayEnabled) {
    pushOverlay.paint(overlayCtx, now, accentColor);
    touchOverlay.paint(overlayCtx, now, accentColor);
  }
  pollWindowShake(now);
  puckMotion.tick(window.screenX, window.screenY, now);
  applyRotation(bezelEl, quickDeg + tiltDeg, puckMotion.offsetX, puckMotion.offsetY);
  updateDiagStrip();
  requestAnimationFrame(frame);
}

// ---- shake ----------------------------------------------------------------
// Two independent triggers feed the same ABI call. Investigated after the
// owner reported window-shaking simply did not work despite measuring fine
// under a scripted (CDP-driven) window move: the firmware-receiving half
// was confirmed working by a direct test (clicking the shake sensor button
// erased a real scribble on the sketchpad, same emu_sensor_event() call a
// jolt would make), which isolates the failure to "does a real drag ever
// get detected at all". A real OS titlebar drag on Windows enters a
// synchronous move loop (WM_ENTERSIZEMOVE) that can stop the renderer from
// being scheduled, so requestAnimationFrame - and with it every per-frame
// screenX/screenY sample - can simply never run while the drag is
// happening, no matter the jolt threshold. This could not be safely
// re-confirmed live in this environment (an attempt to drive a real OS-level
// drag via SendInput risked, and once did, grab an unrelated window rather
// than the test one), but it matches the owner's report exactly: window
// shaking is not a tuning problem, it is a scheduling problem the page has
// no way to work around. It stays wired (it may still work in some
// environments/Chrome versions) but is no longer the recommended path.
//
// The reliable path: dragging the puck ITSELF is ordinary DOM pointer
// input, which is never subject to a native modal loop and always gets
// frames, and arguably reads as a better gesture besides (you are shaking
// the device, not the window it happens to be displayed in). Wired below
// via device.ts's makeDraggable onDrag callback, same jolt-detector shape.
function fireShakeSensor(now: number, source: string): void {
  if (!emu || shakeSensorIndex < 0 || replayer) return;
  emu.emu_sensor_event(shakeSensorIndex);
  recorder.record({ t: now, k: "sensor", i: shakeSensorIndex });
  consoleLog.push(`shake: ${source} accepted`);
}

function pollWindowShake(now: number): void {
  const suppressed = liveTouch.fingers === 1 || pointerIdDown !== null;
  if (windowShake.poll(window.screenX, window.screenY, now, suppressed)) fireShakeSensor(now, "window jolt");
}

function onPuckDrag(clientX: number, clientY: number): void {
  const now = performance.now();
  if (puckDragShake.poll(clientX, clientY, now, liveTouch.fingers === 1)) fireShakeSensor(now, "puck drag");
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
  wireContactSize();

  makeDraggable(
    bezelEl,
    {
      get: () => ({ x: deviceOffsetX, y: deviceOffsetY }),
      set: (x, y) => {
        deviceOffsetX = x;
        deviceOffsetY = y;
        positionDevice();
      },
    },
    onPuckDrag
  );
  bezelEl.title = "drag to move; shake it back and forth to trigger the shake sensor";
  wirePanelInput();
  connectLiveReload();
  wireTraceFile();

  devlinkClient.onStatus(updateDevlinkStatusUI);
  devlinkClient.onLine((text) => consoleLog.push(text, "device"));
  devlinkClient.connect();

  // Owner report: resizing the browser window left the puck off centre.
  // See positionDevice()'s own comment for the fix; this is what makes a
  // resize actually re-run it (positionDevice reads deviceOffsetX/Y and the
  // stage's CURRENT size on every call, so it is safe and cheap to call on
  // every resize event, not just once).
  window.addEventListener("resize", () => positionDevice());

  // AudioContext creation (and resuming a suspended one) must happen
  // inside a real user gesture, per the browser's autoplay policy. Every
  // pointerdown/keydown anywhere on the page qualifies; ensureContext() is
  // idempotent, so wiring it broadly rather than guessing "the first
  // click" is the simplest thing that is still always correct.
  const unlockAudio = () => soundPlayer.ensureContext((text) => consoleLog.push(text));
  document.addEventListener("pointerdown", unlockAudio);
  document.addEventListener("keydown", unlockAudio);

  $<HTMLButtonElement>("#btnMute").addEventListener("click", (e) => {
    const muted = soundPlayer.toggleMute();
    const el = e.currentTarget as HTMLElement;
    el.classList.toggle("active", muted);
    el.title = muted ? "sound muted, click to unmute" : "mute";
    setMuteIcon(el, muted);
  });

  $<HTMLButtonElement>("#btnChord").addEventListener("click", () => {
    void performChord();
  });

  $<HTMLInputElement>("#overlayOn").addEventListener("change", (e) => {
    overlayEnabled = (e.target as HTMLInputElement).checked;
    if (!overlayEnabled) overlayCtx.clearRect(0, 0, overlayEl.width, overlayEl.height);
  });

  $("#rotQuick")
    .querySelectorAll<HTMLButtonElement>("button")
    .forEach((b) => {
      b.addEventListener("click", () => {
        quickDeg = Number(b.dataset.deg);
        $("#rotQuick")
          .querySelectorAll("button")
          .forEach((x) => x.classList.remove("active"));
        b.classList.add("active");
        applyRotation(bezelEl, quickDeg + tiltDeg, puckMotion.offsetX, puckMotion.offsetY);
      });
    });
  $<HTMLInputElement>("#tilt").addEventListener("input", (e) => {
    tiltDeg = Number((e.target as HTMLInputElement).value);
    applyRotation(bezelEl, quickDeg + tiltDeg, puckMotion.offsetX, puckMotion.offsetY);
  });
  // The "active" class on one #rotQuick button is just markup matching
  // quickDeg's actual default above; nothing paints the rotation from CSS
  // alone, so without this call the puck would sit at visual 0deg (wrong,
  // see quickDeg's comment) until the first frame runs.
  applyRotation(bezelEl, quickDeg + tiltDeg);

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
  getSoundPlayer: () => soundPlayer,
  // [bezel element, toolbox chip] for a declared button id, so a headless
  // check can assert the bezel actually moves when the toolbox is clicked
  // (and vice versa) instead of trusting that the two stayed in sync.
  getButtonElements: (id: string) => buttonElById.get(id) || [],
  // The most recent client point mapped through mapClientPoint (rotate.ts),
  // set on every pointerdown/pointermove over the panel - lets a headless
  // check confirm a synthetic click lands at the panel coordinate it aimed
  // for, including after a resize/reposition of the puck (see
  // scripts/verify-ui-feedback.ts).
  getLastTouchMapped: () => lastTouchMapped,
};

void boot();
