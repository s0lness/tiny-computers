// Wires the ported firmware pipeline (engine.ts / pen.ts / raster.ts /
// touchsim.ts) to the DOM: canvas rendering, pointer input, the slider
// panel, the device shell (drag/rotate/buttons/shake), PNG export, and the
// quit button. No firmware behaviour lives in this file; it only drives
// what the other modules already implement faithfully.

import {
  FIRMWARE_DEFAULTS, TOUCHSIM_DEFAULTS, TUNABLE_META,
  PANEL_W, PANEL_H,
  type Tunables, type TunableKey, type TouchSimConfig,
} from "./constants";
import { Engine } from "./engine";
import { TouchSim } from "./touchsim";
import { mergeDirty, emptyDirty, dirtyIsEmpty, type DirtyRect } from "./raster";
import { makeDraggable, wirePwrButton, wireBootButton } from "./device";

const $ = <T extends HTMLElement>(sel: string) => document.querySelector<T>(sel)!;

// ---- live tunable state (mutable copies; sliders write straight into these) ----
const tunables: Tunables = { ...FIRMWARE_DEFAULTS };
const touchCfg: TouchSimConfig = { ...TOUCHSIM_DEFAULTS };

const touch = new TouchSim(touchCfg);
const engine = new Engine(touch, tunables);

// ---- canvas setup ----
const canvas = $<HTMLCanvasElement>("#panel");
const ctx = canvas.getContext("2d", { willReadFrequently: true })!;
const img = ctx.createImageData(PANEL_W, PANEL_H);
img.data.fill(255);
for (let i = 3; i < img.data.length; i += 4) img.data[i] = 255; // alpha

function renderRect(rect: DirtyRect) {
  if (dirtyIsEmpty(rect)) return;
  engine.panel.blitTo(img, rect);
  const w = rect.maxX - rect.minX + 1;
  const h = rect.maxY - rect.minY + 1;
  ctx.putImageData(img, 0, 0, rect.minX, rect.minY, w, h);
}

// Boot: show the test pattern, exactly like the firmware at power-on.
renderRect(engine.showBootPattern());

// ---- pointer input -> TouchSim's "real" finger state ----
let pointerId: number | null = null;

function panelCoords(e: PointerEvent): { x: number; y: number } {
  const scaleX = PANEL_W / canvas.clientWidth;
  const scaleY = PANEL_H / canvas.clientHeight;
  return { x: e.offsetX * scaleX, y: e.offsetY * scaleY };
}

canvas.addEventListener("pointerdown", (e) => {
  if (engine.isErasing()) return;
  pointerId = e.pointerId;
  canvas.setPointerCapture(e.pointerId);
  const { x, y } = panelCoords(e);
  touch.setPointer(true, x, y);
  e.preventDefault();
});
canvas.addEventListener("pointermove", (e) => {
  if (pointerId !== e.pointerId) return;
  const { x, y } = panelCoords(e);
  touch.setPointer(true, x, y);
});
function releasePointer(e: PointerEvent) {
  if (pointerId !== e.pointerId) return;
  pointerId = null;
  touch.setPointer(false, 0, 0);
}
canvas.addEventListener("pointerup", releasePointer);
canvas.addEventListener("pointercancel", releasePointer);

// ---- the tick loop: polled far faster than any configured report rate,
// mirroring the firmware's ~5000Hz loop against a ~60Hz controller. Render
// is decoupled onto rAF, same separation as the firmware's own poll-vs-push. ----
const pending: DirtyRect = emptyDirty();

setInterval(() => {
  const d = engine.tick(performance.now());
  mergeDirty(pending, d);
}, 3);

function frame() {
  if (!dirtyIsEmpty(pending)) {
    renderRect(pending);
    pending.minX = PANEL_W; pending.minY = PANEL_H; pending.maxX = -1; pending.maxY = -1;
  }
  updateStats();
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);

// ---- stats readout (mirrors the firmware's PROFILE printout) ----
const statsEl = $("#stats");
function updateStats() {
  const s = engine.stats();
  statsEl.textContent =
    `finger down   ${s.fingerDown ? "yes" : "no"}\n` +
    `reports/s     ${s.reportsPerSec}\n` +
    `glitches      ${s.glitches}\n` +
    `dropouts      ${s.dropouts}  (sim-injected ${s.simDropouts})\n` +
    `strays        ${s.strays}  (sim-injected ${s.simStrays})\n` +
    `splits        ${s.splits}`;
}

// ---- device shell: drag, rotate, buttons, shake ----
const deviceWrap = $("#deviceWrap");
const bezel = $("#bezel");
makeDraggable(bezel, deviceWrap);

const rotate = $<HTMLInputElement>("#rotate");
rotate.addEventListener("input", () => {
  bezel.style.transform = `rotate(${rotate.value}deg)`;
});

const offOverlay = $("#offOverlay");
const pwrEventEl = $("#pwrEvent");
let poweredOff = false;

wirePwrButton($("#pwrBtn"), {
  onShort: () => {
    if (poweredOff) { poweredOff = false; offOverlay.classList.add("hidden"); pwrEventEl.textContent = "PWR: woke"; return; }
    pwrEventEl.textContent = "PWR: short press";
  },
  onLong: () => {
    pwrEventEl.textContent = "PWR: long press (1.5s)";
  },
  onHardOff: () => {
    poweredOff = true;
    offOverlay.classList.remove("hidden");
    pwrEventEl.textContent = "PWR: hard power-off (6s)";
  },
});
wireBootButton($("#bootBtn"));

$<HTMLButtonElement>("#btnShake").addEventListener("click", () => {
  void engine.shake(performance.now(), renderRect);
});

// ---- PNG export ----
$<HTMLButtonElement>("#btnPng").addEventListener("click", () => {
  canvas.toBlob((blob) => {
    if (!blob) return;
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = `panel-${new Date().toISOString().replace(/[:.]/g, "-")}.png`;
    a.click();
    URL.revokeObjectURL(a.href);
  }, "image/png");
});

// ---- copy tuned constants back as firmware #defines ----
// LIFT_DEBOUNCE_MS is the one integer-typed constant in main.c (compared
// against a uint32_t millisecond count, no `f` suffix); every other tunable
// is a float there.
const INT_DEFINES = new Set<TunableKey>(["LIFT_DEBOUNCE_MS"]);
$<HTMLButtonElement>("#btnDefines").addEventListener("click", async () => {
  const lines = TUNABLE_META.map((m) => {
    const v = tunables[m.key];
    const literal = INT_DEFINES.has(m.key) ? String(Math.round(v)) : `${v}f`;
    return `#define ${m.key.padEnd(20)} ${literal}`;
  });
  const text = lines.join("\n") + "\n";
  await navigator.clipboard.writeText(text);
  const btn = $<HTMLButtonElement>("#btnDefines");
  const orig = btn.textContent;
  btn.textContent = "copied";
  setTimeout(() => (btn.textContent = orig), 1000);
});

// ---- quit ----
$<HTMLButtonElement>("#btnQuit").addEventListener("click", async () => {
  await fetch("/api/quit", { method: "POST", headers: { "x-rp2350-emulator": "1" } });
  document.body.innerHTML =
    '<div style="display:flex;align-items:center;justify-content:center;height:100vh;font:14px monospace;color:#888">emulator stopped. you can close this window.</div>';
});

// ---- slider panel ----
function sliderRow(container: HTMLElement, meta: (typeof TUNABLE_META)[number]) {
  const row = document.createElement("div");
  row.className = "slider-row";
  const value = tunables[meta.key];
  row.innerHTML = `
    <div class="row-head"><b>${meta.label}</b><span>${formatVal(value)}${meta.unit}</span></div>
    <input type="range" min="${meta.min}" max="${meta.max}" step="${meta.step}" value="${value}" />
    <div class="row-note">${meta.note}</div>
  `;
  const input = row.querySelector("input")!;
  const span = row.querySelector("span")!;
  input.addEventListener("input", () => {
    const v = Number(input.value);
    tunables[meta.key] = v;
    span.textContent = `${formatVal(v)}${meta.unit}`;
  });
  (row as any)._input = input;
  (row as any)._span = span;
  container.appendChild(row);
  return row;
}
function formatVal(v: number): string {
  return Number.isInteger(v) ? String(v) : v.toFixed(3).replace(/0+$/, "").replace(/\.$/, "");
}

const penContainer = $("#penSliders");
const touchContainer = $("#touchSliders");
const sliderRows: HTMLElement[] = [];
for (const meta of TUNABLE_META) {
  const container = meta.group === "pen" ? penContainer : touchContainer;
  sliderRows.push(sliderRow(container, meta));
}

// ---- touch controller (report rate / dropouts / strays) ----
const touchControlsEl = $("#touchControls");
touchControlsEl.innerHTML = `
  <div class="slider-row">
    <div class="row-head"><b>report rate</b><span id="rrVal">${touchCfg.reportRateHz}Hz</span></div>
    <input id="rrInput" type="range" min="10" max="240" step="5" value="${touchCfg.reportRateHz}" />
    <div class="row-note">how often the touch controller answers with a fresh sample</div>
  </div>
  <div class="toggle-row">
    <input id="dropoutsOn" type="checkbox" ${touchCfg.dropoutsEnabled ? "checked" : ""} />
    <label for="dropoutsOn">contact dropouts mid-stroke</label>
  </div>
  <div class="slider-row">
    <div class="row-head"><b>dropout rate</b><span id="dpVal">${touchCfg.dropoutsPerSec}/s</span></div>
    <input id="dpInput" type="range" min="0" max="5" step="0.1" value="${touchCfg.dropoutsPerSec}" />
    <div class="row-note">measured on real hardware: 1 to 3 per second while drawing</div>
  </div>
  <div class="toggle-row">
    <input id="straysOn" type="checkbox" ${touchCfg.straysEnabled ? "checked" : ""} />
    <label for="straysOn">stray contacts at wrong positions</label>
  </div>
  <div class="slider-row">
    <div class="row-head"><b>stray rate</b><span id="stVal">${touchCfg.straysPerSec}/s</span></div>
    <input id="stInput" type="range" min="0" max="2" step="0.05" value="${touchCfg.straysPerSec}" />
    <div class="row-note">single false reports while nothing is touching the panel</div>
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

// Exposed for the headless verify script (scripts/verify.ts) and for
// poking at from the browser console while tuning; not used by the UI
// itself.
(window as any).__engine = engine;
(window as any).__tunables = tunables;
(window as any).__touchCfg = touchCfg;

// ---- reset to firmware defaults ----
$<HTMLButtonElement>("#btnReset").addEventListener("click", () => {
  Object.assign(tunables, FIRMWARE_DEFAULTS);
  Object.assign(touchCfg, TOUCHSIM_DEFAULTS);
  for (let i = 0; i < TUNABLE_META.length; i++) {
    const meta = TUNABLE_META[i]!;
    const row = sliderRows[i]!;
    const input = (row as any)._input as HTMLInputElement;
    const span = (row as any)._span as HTMLElement;
    input.value = String(tunables[meta.key]);
    span.textContent = `${formatVal(tunables[meta.key])}${meta.unit}`;
  }
  $<HTMLInputElement>("#rrInput").value = String(touchCfg.reportRateHz);
  $("#rrVal").textContent = `${touchCfg.reportRateHz}Hz`;
  $<HTMLInputElement>("#dropoutsOn").checked = touchCfg.dropoutsEnabled;
  $<HTMLInputElement>("#dpInput").value = String(touchCfg.dropoutsPerSec);
  $("#dpVal").textContent = `${touchCfg.dropoutsPerSec}/s`;
  $<HTMLInputElement>("#straysOn").checked = touchCfg.straysEnabled;
  $<HTMLInputElement>("#stInput").value = String(touchCfg.straysPerSec);
  $("#stVal").textContent = `${touchCfg.straysPerSec}/s`;
});
