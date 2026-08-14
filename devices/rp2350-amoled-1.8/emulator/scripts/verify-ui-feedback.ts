// Headless check for the four owner-reported UI fixes (console pane height,
// mute icon reflecting state, finger-size label/order, and the B/P/S +
// chord toolbox). Needs a real emu.wasm at wasm/dist/emu.wasm (see
// wasm/build.ts); unlike scripts/verify.ts this does not have a
// missing-module fallback path, because every assertion here needs the
// real device chrome to exist.
import puppeteer from "puppeteer-core";
import { join } from "node:path";
import { existsSync } from "node:fs";

const ROOT = join(import.meta.dir, "..");
const PORT = 53311;
const CHROME = "C:/Program Files/Google/Chrome/Application/chrome.exe";
const WASM_FILE = join(ROOT, "wasm", "dist", "emu.wasm");

function fail(msg: string): never {
  console.error(`FAIL: ${msg}`);
  process.exit(1);
}

async function waitForServer(url: string, timeoutMs: number): Promise<void> {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    try {
      const r = await fetch(url);
      if (r.ok) return;
    } catch {}
    await new Promise((r) => setTimeout(r, 150));
  }
  throw new Error(`server did not come up within ${timeoutMs}ms`);
}

if (!existsSync(WASM_FILE)) {
  fail(`${WASM_FILE} does not exist; build it first (bun run wasm/build.ts)`);
}

const server = Bun.spawn(["bun", "run", "server.ts"], {
  cwd: ROOT,
  env: { ...process.env, PORT: String(PORT) },
  stdout: "pipe",
  stderr: "pipe",
});

let browser: Awaited<ReturnType<typeof puppeteer.launch>> | null = null;
try {
  await waitForServer(`http://127.0.0.1:${PORT}/`, 15000);
  console.log("server up");

  browser = await puppeteer.launch({ executablePath: CHROME, headless: true });
  const page = await browser.newPage();
  await page.setViewport({ width: 1400, height: 1000 });
  page.on("pageerror", (e) => console.error("page error:", e));

  await page.goto(`http://127.0.0.1:${PORT}/`, { waitUntil: "domcontentloaded" });
  await new Promise((r) => setTimeout(r, 1200));

  const deviceName = await page.$eval("#deviceName", (el) => el.textContent || "");
  const errorHidden = await page.evaluate(() => document.querySelector("#wasmError")?.classList.contains("hidden") ?? true);
  if (!errorHidden) {
    const errorText = await page.$eval("#wasmError", (el) => el.textContent || "");
    fail(`wasm module exists but the page still shows an error: ${errorText}`);
  }
  console.log(`device came up ("${deviceName}")`);

  // ---- 1. console pane is genuinely taller than the old fixed 100px -----
  const consoleH = await page.$eval("#consolePane", (el) => el.getBoundingClientRect().height);
  console.log(`console pane height: ${consoleH}px`);
  if (consoleH <= 150) fail(`console pane is only ${consoleH}px tall, expected well over the old 100px`);
  console.log("PASS: console pane is taller");

  // ---- 2. mute icon changes with state, in both directions --------------
  // Compared by shape (does it have the crossed-out "muted" lines?), not by
  // raw HTML string: the page's own innerHTML swap re-serialises attribute
  // whitespace differently than the static markup it replaces, which is
  // cosmetic and not what this is checking.
  const isMutedIcon = () => page.$eval("#btnMute", (el) => el.querySelectorAll("line").length === 2);
  const mutedBefore = await isMutedIcon();
  if (mutedBefore) fail("page loaded already showing the muted icon; expected unmuted by default");
  await page.click("#btnMute");
  const mutedAfterClick1 = await isMutedIcon();
  const titleAfterMute = await page.$eval("#btnMute", (el) => el.getAttribute("title") || "");
  if (!mutedAfterClick1) fail("mute icon did not switch to the muted (crossed-out) icon after muting");
  if (!titleAfterMute.toLowerCase().includes("unmute")) fail(`mute title unclear: "${titleAfterMute}"`);
  await page.click("#btnMute");
  const mutedAfterClick2 = await isMutedIcon();
  if (mutedAfterClick2) fail("mute icon did not switch back to the unmuted icon after unmuting");
  console.log("PASS: mute icon reflects state in both directions");

  // ---- 3. finger size: label, then input, then presets, no spinner ------
  const order = await page.evaluate(() => {
    const parent = document.querySelector("#contactMm")!.closest(".side")!;
    const kids = Array.from(parent.querySelectorAll(".field-label, .contact-row, #contactPreset"));
    return kids.map((k) => (k.id || k.className));
  });
  console.log("finger-size DOM order:", order);
  const labelIdx = order.findIndex((c) => c.includes("field-label"));
  const rowIdx = order.findIndex((c) => c.includes("contact-row"));
  const presetIdx = order.findIndex((c) => c === "contactPreset");
  if (!(labelIdx >= 0 && labelIdx < rowIdx && rowIdx < presetIdx)) fail("finger-size order is not label -> input -> presets");
  const labelText = await page.$eval(".field-label", (el) => el.textContent || "");
  if (!/finger size/i.test(labelText)) fail(`finger-size label reads "${labelText}", expected "finger size"`);
  const appearance = await page.$eval("#contactMm", (el) => getComputedStyle(el).getPropertyValue("appearance") || getComputedStyle(el).getPropertyValue("-moz-appearance"));
  console.log(`#contactMm appearance: "${appearance}"`);
  if (!/textfield|none/i.test(appearance)) fail(`#contactMm may show native spinner arrows (appearance: "${appearance}")`);

  // Default is the child preset (touchoverlay.ts), so exercise adult first
  // to actually prove the click changes something, then back to child.
  await page.click('#contactPreset button[data-mm="8"]'); // adult preset
  const mmAfterAdult = await page.$eval("#contactMm", (el) => (el as HTMLInputElement).value);
  if (Number(mmAfterAdult) !== 8) fail(`adult preset did not set 8mm, got "${mmAfterAdult}"`);
  await page.click('#contactPreset button[data-mm="6"]'); // child preset
  const mmAfterChild = await page.$eval("#contactMm", (el) => (el as HTMLInputElement).value);
  if (Number(mmAfterChild) !== 6) fail(`child preset did not set 6mm, got "${mmAfterChild}"`);
  console.log(`PASS: finger size label/order/no-spinner, presets change the value (${mmAfterAdult}mm adult, ${mmAfterChild}mm child)`);

  // ---- 4. B/P/S + chord live in the toolbox, chord opens the app menu ---
  const toolboxButtons = await page.$$eval("#buttonToolbox .toolbox-btn", (els) => els.map((e) => e.textContent));
  console.log("button toolbox:", toolboxButtons);
  if (!toolboxButtons.includes("B") || !toolboxButtons.includes("P")) fail(`expected B and P in the button toolbox, got ${JSON.stringify(toolboxButtons)}`);
  const sensorToolboxHTML = await page.$eval("#sensorToolbox", (el) => el.innerHTML);
  if (!/>\s*S\s*<|shake/i.test(sensorToolboxHTML)) fail("expected the shake sensor's control (S) in the sensor toolbox");
  console.log("PASS: B, P and the shake control are in the toolbox");

  // Old sidebar legend/sensor duplicates must be gone (subtract before add).
  const hasOldShortcutList = await page.evaluate(() => !!document.querySelector("#buttonShortcuts"));
  const hasOldSensorControls = await page.evaluate(() => !!document.querySelector("#sensorControls"));
  if (hasOldShortcutList) fail("#buttonShortcuts still exists; should have been removed as redundant with the toolbox");
  if (hasOldSensorControls) fail("#sensorControls still exists; should have been removed as redundant with the toolbox");
  console.log("PASS: redundant sidebar legend/sensor controls were removed");

  const chordDisabled = await page.$eval("#btnChord", (el) => (el as HTMLButtonElement).disabled);
  if (chordDisabled) fail("chord button is disabled even though this device declares boot and pwr");

  const appBefore = await page.evaluate(() => {
    const dbg = (window as unknown as { __debug: { getEmu(): { emu_app_current?(): number } | null } }).__debug;
    const emu = dbg.getEmu();
    return emu?.emu_app_current ? emu.emu_app_current() : null;
  });
  console.log(`app index before chord: ${appBefore}`);

  await page.click("#btnChord");
  await new Promise((r) => setTimeout(r, 400)); // chord's own ~50ms release delay, plus a couple of ticks

  const appAfterChord = await page.evaluate(() => {
    const dbg = (window as unknown as { __debug: { getEmu(): { emu_app_current?(): number } | null } }).__debug;
    const emu = dbg.getEmu();
    return emu?.emu_app_current ? emu.emu_app_current() : null;
  });
  console.log(`app index after chord: ${appAfterChord}`);
  if (appAfterChord === appBefore) fail("the chord button did not change the running app (expected it to open the app menu)");
  console.log("PASS: the chord button drove the real BOOT+PWR gesture and the app changed");

  console.log("\nALL PASS");
} finally {
  if (browser) await browser.close();
  server.kill();
  try {
    Bun.spawnSync(["taskkill", "/pid", String(server.pid), "/t", "/f"], { stdout: "ignore", stderr: "ignore" });
  } catch {}
}
