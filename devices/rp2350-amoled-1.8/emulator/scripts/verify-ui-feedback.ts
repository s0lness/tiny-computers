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

  // ---- 5. bezel button visibly presses when driven from the toolbox chip
  // (the owner's actual ask: "when clicking on the P or B button it should
  // actually visually press the buttons"). Reads via the __debug hook's
  // getButtonElements(id), which returns [bezel element, toolbox chip] for
  // one declared button id, so this checks the ACTUAL bezel button element,
  // not just the chip that was clicked. -----------------------------------
  async function classesFor(id: string): Promise<{ bezel: string[]; chip: string[] }> {
    return page.evaluate((buttonId) => {
      const dbg = (window as unknown as { __debug: { getButtonElements(id: string): HTMLElement[] } }).__debug;
      const [bezel, chip] = dbg.getButtonElements(buttonId);
      return { bezel: bezel ? Array.from(bezel.classList) : [], chip: chip ? Array.from(chip.classList) : [] };
    }, id);
  }
  async function centerOfToolboxButton(letter: string): Promise<{ x: number; y: number }> {
    const box = await page.evaluate((l) => {
      const el = Array.from(document.querySelectorAll<HTMLElement>("#buttonToolbox .toolbox-btn")).find((b) => b.textContent === l);
      if (!el) return null;
      const r = el.getBoundingClientRect();
      return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
    }, letter);
    if (!box) fail(`no toolbox button labelled "${letter}"`);
    return box;
  }

  const bPos = await centerOfToolboxButton("B");
  await page.mouse.move(bPos.x, bPos.y);
  await page.mouse.down();
  const bootWhileHeld = await classesFor("boot");
  console.log(`BOOT classes while the toolbox B chip is held: bezel=[${bootWhileHeld.bezel.join(" ")}] chip=[${bootWhileHeld.chip.join(" ")}]`);
  if (!bootWhileHeld.bezel.includes("pressed")) fail("bezel BOOT button did not visually press when the toolbox B chip was clicked and held");
  if (!bootWhileHeld.chip.includes("pressed")) fail("toolbox B chip itself did not show pressed while held (test setup problem, not the owner's bug)");

  // Mouse leaving the control while still held must not drop the pressed
  // state early: releasing happens on pointer up, wherever it lands, not on
  // pointer leave (see device.ts's wireButton, pointer capture per element).
  await page.mouse.move(bPos.x + 250, bPos.y + 250);
  const bootAfterLeave = await classesFor("boot");
  if (!bootAfterLeave.bezel.includes("pressed")) fail("bezel BOOT button lost its pressed state when the mouse left the toolbox chip while still held");
  await page.mouse.up();
  const bootAfterUp = await classesFor("boot");
  if (bootAfterUp.bezel.includes("pressed") || bootAfterUp.chip.includes("pressed")) {
    fail(`BOOT stayed pressed after mouse-up off-control (bezel=[${bootAfterUp.bezel.join(" ")}] chip=[${bootAfterUp.chip.join(" ")}])`);
  }
  console.log("PASS: clicking the toolbox B chip presses the bezel BOOT button, and releasing (even off-control) clears both");

  // ---- 6. the chord shows a genuine hold, not a flicker -----------------
  // The chord is the one case with a real 50ms hold (CHORD_RELEASE_DELAY_MS
  // in main.ts) between BOOT going down and coming back up, and it drives
  // PWR's pressed/long classes directly on both of PWR's elements (bezel +
  // toolbox chip) rather than through PWR's own WiredButton. A naive
  // implementation flickers this or only updates one element; this checks
  // both elements, for both buttons, mid-hold and after release.
  await page.click("#btnChord");
  // Read immediately: performChord() runs synchronously up to its first
  // `await sleep(...)`, so by the time page.click() has resolved the
  // synchronous portion (BOOT down, PWR's pressed+long classes) has
  // already run, same reasoning the mute-icon check above already relies
  // on with no extra wait.
  const bootMidHold = await classesFor("boot");
  const pwrMidHold = await classesFor("pwr");
  console.log(`mid-chord: boot bezel=[${bootMidHold.bezel.join(" ")}] chip=[${bootMidHold.chip.join(" ")}]`);
  console.log(`mid-chord: pwr  bezel=[${pwrMidHold.bezel.join(" ")}] chip=[${pwrMidHold.chip.join(" ")}]`);
  if (!bootMidHold.bezel.includes("pressed") || !bootMidHold.chip.includes("pressed")) {
    fail("chord: BOOT is not shown held (pressed) on both the bezel and the toolbox chip mid-chord");
  }
  if (!pwrMidHold.bezel.includes("pressed") || !pwrMidHold.bezel.includes("long")) {
    fail("chord: PWR bezel button does not show the completed long-press verdict (pressed+long) mid-chord");
  }
  if (!pwrMidHold.chip.includes("pressed") || !pwrMidHold.chip.includes("long")) {
    fail("chord: PWR toolbox chip does not show the completed long-press verdict (pressed+long) mid-chord");
  }
  console.log("PASS: the chord shows a genuine hold on BOOT and PWR, on both the bezel and the toolbox");

  await new Promise((r) => setTimeout(r, 400)); // chord's own ~50ms release delay, plus a couple of ticks
  const bootAfterChord = await classesFor("boot");
  const pwrAfterChord = await classesFor("pwr");
  if (bootAfterChord.bezel.includes("pressed") || bootAfterChord.chip.includes("pressed")) {
    fail(`chord: BOOT still shows pressed after the chord finished (bezel=[${bootAfterChord.bezel.join(" ")}] chip=[${bootAfterChord.chip.join(" ")}])`);
  }
  if (pwrAfterChord.bezel.includes("pressed") || pwrAfterChord.bezel.includes("long") || pwrAfterChord.chip.includes("pressed") || pwrAfterChord.chip.includes("long")) {
    fail(`chord: PWR still shows pressed/long after the chord finished (bezel=[${pwrAfterChord.bezel.join(" ")}] chip=[${pwrAfterChord.chip.join(" ")}])`);
  }
  console.log("PASS: the chord's hold clears on both BOOT and PWR, on both the bezel and the toolbox, once it finishes");

  // ---- 7. resizing the browser window keeps the puck centred ------------
  // Owner report, verbatim: "si je resize l'emulateur il faut que le device
  // rendered reste centre, comme tu le fais avec la toolbar de 90 degres."
  // main.ts's positionDevice() stores the puck's position as a displacement
  // from the STAGE's own centre (0,0 until dragged), not an absolute pixel,
  // and recomputes it on every resize - this is what proves that actually
  // holds, at a size well below the first one, not just a fresh reload at
  // whatever size the page happened to load at.
  async function stageBezelOffset(): Promise<{ dx: number; dy: number }> {
    return page.evaluate(() => {
      const stage = document.querySelector("#stage")!.getBoundingClientRect();
      const bezel = document.querySelector("#bezel")!.getBoundingClientRect();
      return {
        dx: bezel.left + bezel.width / 2 - (stage.left + stage.width / 2),
        dy: bezel.top + bezel.height / 2 - (stage.top + stage.height / 2),
      };
    });
  }
  const CENTER_TOLERANCE_PX = 2; // positionDevice rounds to the nearest px

  const centeredAt1400 = await stageBezelOffset();
  console.log(`puck displacement from stage centre at 1400x1000: (${centeredAt1400.dx.toFixed(1)}, ${centeredAt1400.dy.toFixed(1)})`);
  if (Math.abs(centeredAt1400.dx) > CENTER_TOLERANCE_PX || Math.abs(centeredAt1400.dy) > CENTER_TOLERANCE_PX) {
    fail(`puck is not centred at 1400x1000 (offset ${centeredAt1400.dx.toFixed(1)},${centeredAt1400.dy.toFixed(1)})`);
  }

  await page.setViewport({ width: 950, height: 760 }); // materially smaller in both dimensions
  await page.evaluate(() => window.dispatchEvent(new Event("resize"))); // belt-and-braces: force the listener even if the CDP-driven viewport change does not itself dispatch one
  await new Promise((r) => setTimeout(r, 200));

  const centeredAt950 = await stageBezelOffset();
  console.log(`puck displacement from stage centre at 950x760: (${centeredAt950.dx.toFixed(1)}, ${centeredAt950.dy.toFixed(1)})`);
  if (Math.abs(centeredAt950.dx) > CENTER_TOLERANCE_PX || Math.abs(centeredAt950.dy) > CENTER_TOLERANCE_PX) {
    fail(`puck drifted off centre after resizing to 950x760 (offset ${centeredAt950.dx.toFixed(1)},${centeredAt950.dy.toFixed(1)})`);
  }
  console.log("PASS: the puck stays centred across a materially different window size");

  // ---- 8. a dragged puck keeps ITS displacement from centre across a
  // resize, rather than snapping back or drifting ---------------------------
  // The deliberate half of the fix (see positionDevice()'s own comment for
  // the reasoning): dragging is still allowed to place the puck wherever the
  // owner wants, and a resize must not silently undo that.
  const grabPoint = await page.evaluate(() => {
    // The panel canvas (and the buttons) cover almost all of the bezel's own
    // area; only its padding border is bare plastic and actually starts a
    // drag (see device.ts's makeDraggable, e.target !== bezel guard). Probe
    // a handful of points in that border, skipping any device's own button
    // layout, rather than assuming a fixed corner is always clear.
    const bezel = document.querySelector("#bezel")!;
    const r = bezel.getBoundingClientRect();
    const fracs = [0.03, 0.06, 0.1, 0.5, 0.9, 0.94, 0.97];
    for (const fx of fracs) {
      for (const fy of fracs) {
        for (const [x, y] of [
          [r.left + r.width * fx, r.top + r.height * fy],
          [r.right - r.width * fx, r.top + r.height * fy],
        ]) {
          if (document.elementFromPoint(x, y) === bezel) return { x, y };
        }
      }
    }
    return null;
  });
  if (!grabPoint) fail("could not find a point on the bezel itself (bare plastic, not the panel or a button) to start a drag from");

  const DRAG_DX = 60;
  const DRAG_DY = -40;
  await page.mouse.move(grabPoint!.x, grabPoint!.y);
  await page.mouse.down();
  await page.mouse.move(grabPoint!.x + DRAG_DX, grabPoint!.y + DRAG_DY, { steps: 8 });
  await page.mouse.up();

  const draggedOffset = await stageBezelOffset();
  console.log(`puck displacement from stage centre after a (${DRAG_DX},${DRAG_DY}) drag: (${draggedOffset.dx.toFixed(1)}, ${draggedOffset.dy.toFixed(1)})`);
  if (Math.abs(draggedOffset.dx - DRAG_DX) > CENTER_TOLERANCE_PX || Math.abs(draggedOffset.dy - DRAG_DY) > CENTER_TOLERANCE_PX) {
    fail(`drag did not land where dragged (expected ~(${DRAG_DX},${DRAG_DY}), got (${draggedOffset.dx.toFixed(1)},${draggedOffset.dy.toFixed(1)}))`);
  }

  await page.setViewport({ width: 1300, height: 950 });
  await page.evaluate(() => window.dispatchEvent(new Event("resize")));
  await new Promise((r) => setTimeout(r, 200));

  const draggedOffsetAfterResize = await stageBezelOffset();
  console.log(
    `puck displacement from stage centre after resizing to 1300x950: (${draggedOffsetAfterResize.dx.toFixed(1)}, ${draggedOffsetAfterResize.dy.toFixed(1)})`
  );
  if (Math.abs(draggedOffsetAfterResize.dx - DRAG_DX) > CENTER_TOLERANCE_PX || Math.abs(draggedOffsetAfterResize.dy - DRAG_DY) > CENTER_TOLERANCE_PX) {
    fail(
      `dragged puck's displacement from centre did not survive the resize (expected ~(${DRAG_DX},${DRAG_DY}), got (${draggedOffsetAfterResize.dx.toFixed(1)},${draggedOffsetAfterResize.dy.toFixed(1)}))`
    );
  }
  console.log("PASS: a dragged puck keeps its chosen displacement from centre across a resize, instead of snapping back or drifting");

  // ---- 9. touch input still lands where the user aimed --------------------
  // A centring fix that silently offsets touch input would be a much worse
  // bug than the one being fixed, and it would be invisible until someone
  // tried to draw. mapClientPoint (rotate.ts) always reads the panel's LIVE
  // getBoundingClientRect(), so this is really checking that the panel
  // itself actually sits where positionDevice put it - not a property of
  // rotate.ts, which this task did not touch.
  const deviceInfo = await page.evaluate(() => {
    const dbg = (window as unknown as { __debug: { getDevice(): { panel: { w: number; h: number } } | null } }).__debug;
    const d = dbg.getDevice();
    return d ? { w: d.panel.w, h: d.panel.h } : null;
  });
  if (!deviceInfo) fail("no device descriptor loaded; cannot test touch mapping");
  const { w: fbW, h: fbH } = deviceInfo!;
  console.log(`panel framebuffer: ${fbW}x${fbH}`);

  function clampRound(v: number, max: number): number {
    return Math.max(0, Math.min(max - 1, Math.round(v)));
  }

  async function clickAndReadMapped(clientX: number, clientY: number): Promise<{ x: number; y: number }> {
    await page.mouse.move(clientX, clientY);
    await page.mouse.down();
    await page.mouse.up();
    const mapped = await page.evaluate(() => {
      const dbg = (window as unknown as { __debug: { getLastTouchMapped(): { panel: { x: number; y: number } } | null } }).__debug;
      return dbg.getLastTouchMapped();
    });
    if (!mapped) fail("no touch was recorded (getLastTouchMapped() returned null after a click on the panel)");
    return mapped!.panel;
  }

  const TOUCH_TOLERANCE_PX = 2;
  function assertNear(label: string, got: { x: number; y: number }, want: { x: number; y: number }): void {
    console.log(`${label}: got (${got.x},${got.y}), want ~(${want.x},${want.y})`);
    if (Math.abs(got.x - want.x) > TOUCH_TOLERANCE_PX || Math.abs(got.y - want.y) > TOUCH_TOLERANCE_PX) {
      fail(`${label}: touch mapped to (${got.x},${got.y}), expected ~(${want.x},${want.y})`);
    }
  }

  async function panelRect(): Promise<{ left: number; top: number; width: number; height: number }> {
    return page.$eval("#panel", (el) => {
      const r = el.getBoundingClientRect();
      return { left: r.left, top: r.top, width: r.width, height: r.height };
    });
  }

  // (a) a known off-centre panel fraction, at quickDeg=0/tilt=0 so
  // client->panel is a plain unrotated scale, checked again after a further
  // resize (the puck is currently dragged off-centre too, from check 8 -
  // deliberately not undone here, since a dragged puck is a real state the
  // owner draws in just as often as a centred one).
  await page.click('#rotQuick button[data-deg="0"]');
  await page.$eval("#tilt", (el) => {
    (el as HTMLInputElement).value = "0";
    el.dispatchEvent(new Event("input", { bubbles: true }));
  });

  const FRAC_X = 0.25;
  const FRAC_Y = 0.75;
  async function checkFractionalPoint(label: string): Promise<void> {
    const rect = await panelRect();
    const got = await clickAndReadMapped(rect.left + rect.width * FRAC_X, rect.top + rect.height * FRAC_Y);
    const want = { x: clampRound(fbW * FRAC_X, fbW), y: clampRound(fbH * FRAC_Y, fbH) };
    assertNear(label, got, want);
  }
  await checkFractionalPoint("touch at panel (25%,75%) at 1300x950");

  await page.setViewport({ width: 1150, height: 880 });
  await page.evaluate(() => window.dispatchEvent(new Event("resize")));
  await new Promise((r) => setTimeout(r, 200));
  await checkFractionalPoint("touch at panel (25%,75%) after resizing to 1150x880");
  console.log("PASS: a click at a known panel fraction still maps to the expected framebuffer coordinate after a resize");

  // (b) the exact rendered centre always maps to the framebuffer's centre,
  // regardless of rotation: dx=dy=0 from the rendered bounding box's own
  // centre is a fixed point of mapClientPoint's rotation matrix at ANY
  // angle, so this holds at each quick rotation and with a nonzero tilt on
  // top - the two axes (quick rotation, tilt) most likely to reveal a
  // stale/mismeasured panel rect if positionDevice ever got that wrong.
  async function checkCenterPoint(label: string): Promise<void> {
    const rect = await panelRect();
    const got = await clickAndReadMapped(rect.left + rect.width / 2, rect.top + rect.height / 2);
    const want = { x: Math.round(fbW / 2), y: Math.round(fbH / 2) };
    assertNear(label, got, want);
  }
  for (const deg of ["0", "90", "-90", "180"]) {
    await page.click(`#rotQuick button[data-deg="${deg}"]`);
    await checkCenterPoint(`touch at panel centre, quickDeg=${deg} tilt=0`);
  }
  await page.click('#rotQuick button[data-deg="0"]');
  await page.$eval("#tilt", (el) => {
    (el as HTMLInputElement).value = "15";
    el.dispatchEvent(new Event("input", { bubbles: true }));
  });
  await checkCenterPoint("touch at panel centre, quickDeg=0 tilt=15");
  console.log("PASS: a click at the puck's rendered centre always lands on the framebuffer's centre, at every quick rotation and with a nonzero tilt");

  console.log("\nALL PASS");
} finally {
  if (browser) await browser.close();
  server.kill();
  try {
    Bun.spawnSync(["taskkill", "/pid", String(server.pid), "/t", "/f"], { stdout: "ignore", stderr: "ignore" });
  } catch {}
}
