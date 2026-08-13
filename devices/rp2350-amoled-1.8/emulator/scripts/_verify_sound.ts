import puppeteer from "puppeteer-core";
const CHROME = "C:/Program Files/Google/Chrome/Application/chrome.exe";
const browser = await puppeteer.launch({ executablePath: CHROME, headless: true });
try {
  const page = await browser.newPage();
  await page.setViewport({ width: 1440, height: 1000 });
  const errors: string[] = [];
  page.on("pageerror", (e) => errors.push(String(e)));
  page.on("console", (m) => { if (m.type() === "error") errors.push(`console.error: ${m.text()}`); });

  // Accelerate performance.now() 8x, injected before any page script runs,
  // so a 30s minimum timer duration completes in real time instead of a
  // real 30s wait. 8x keeps each tick's accelerated dt under the runtime's
  // 250ms clamp even with some frame jitter (16ms*8=128ms typical).
  await page.evaluateOnNewDocument(() => {
    const orig = performance.now.bind(performance);
    const start = orig();
    (performance as any).now = () => start + (orig() - start) * 8;
  });

  await page.goto("http://127.0.0.1:5330/", { waitUntil: "domcontentloaded" });
  await new Promise((r) => setTimeout(r, 1200));

  // switch to timer
  const apps = await page.$$eval(".app-strip-btn", (els) => els.map((e) => e.textContent));
  const idx = apps.findIndex((a) => a === "timer");
  const btns = await page.$$(".app-strip-btn");
  await btns[idx]!.click();
  await new Promise((r) => setTimeout(r, 300));

  // drag a wide arc on the panel to set a duration (real touch path, same
  // as a user would use), then read back what got set from the console.
  const box = await page.$eval("#panel", (el) => { const r = el.getBoundingClientRect(); return { x: r.x, y: r.y, w: r.width, h: r.height }; });
  const cx = box.x + box.w / 2, cy = box.y + box.h / 2;
  const radius = Math.min(box.w, box.h) * 0.35;
  await page.mouse.move(cx + radius, cy);
  await page.mouse.down();
  for (let deg = 0; deg <= 6; deg += 2) {
    const rad = (deg * Math.PI) / 180;
    await page.mouse.move(cx + radius * Math.cos(rad), cy + radius * Math.sin(rad), { steps: 1 });
    await new Promise((r) => setTimeout(r, 10));
  }
  await page.mouse.up();
  await new Promise((r) => setTimeout(r, 200));

  // start it: PWR short press. PWR is a real DOM button on the bezel with
  // its own pointerdown/pointerup handling (device.ts's wireButton).
  const pwrBtn = await page.$(".dev-btn[title^='PWR']");
  if (!pwrBtn) throw new Error("PWR button not found");
  await pwrBtn.click();
  await new Promise((r) => setTimeout(r, 300));

  const startLog = await page.$eval("#consolePane", (el) => {
    const lines = el.textContent || "";
    const m = lines.match(/timer: start, (\d+):(\d+)/);
    return m ? { min: Number(m[1]), sec: Number(m[2]) } : null;
  });
  console.log("timer started at:", startLog);
  if (!startLog) throw new Error("timer did not start (no 'timer: start' log line)");
  const totalSeconds = startLog.min * 60 + startLog.sec;
  const waitMs = Math.ceil((totalSeconds * 1000) / 8) + 1500; // /8 for the acceleration, +buffer
  console.log(`waiting ~${(waitMs / 1000).toFixed(1)}s real time for a ${totalSeconds}s (accelerated) countdown...`);

  const soundBefore = await page.evaluate(() => (window as any).__debug.getSoundPlayer().status);
  console.log("sound status before alarm:", soundBefore);

  await new Promise((r) => setTimeout(r, waitMs));

  const consoleAfter = await page.$eval("#consolePane", (el) => el.textContent);
  const alarmFired = consoleAfter!.includes("alarm") || consoleAfter!.includes("ALARM");
  console.log("console mentions alarm:", alarmFired);

  const soundState = await page.evaluate(() => {
    const sp = (window as any).__debug.getSoundPlayer();
    return { status: sp.status, muted: sp.muted };
  });
  console.log("sound player state after alarm should have fired:", soundState);

  const diag = await page.$eval("#diagStrip", (el) => el.textContent);
  console.log("diag strip:", diag);

  // Inspect the actual sample buffer via the emu ABI directly, to confirm
  // it is real, non-silent PCM (not just that our wrapper thinks it played).
  const bufferInfo = await page.evaluate(() => {
    const emu = (window as any).__debug.getEmu();
    if (!emu.emu_sound_frames) return null;
    const frames = emu.emu_sound_frames();
    const ptr = emu.emu_sound_buffer();
    const rate = emu.emu_sound_sample_rate();
    const arr = new Int16Array(emu.memory.buffer, ptr, frames);
    let max = 0, sumAbs = 0;
    for (let i = 0; i < arr.length; i++) { const v = Math.abs(arr[i]); if (v > max) max = v; sumAbs += v; }
    return { frames, rate, max, meanAbs: sumAbs / arr.length };
  });
  console.log("sample buffer:", bufferInfo);

  // Now dismiss: another PWR short press, per timer.c ("any button, touch
  // or shake"), and confirm the stop path actually cuts the node.
  await pwrBtn.click();
  await new Promise((r) => setTimeout(r, 300));
  const soundAfterDismiss = await page.evaluate(() => (window as any).__debug.getSoundPlayer().status);
  console.log("sound status after dismiss (expect idle/suspended, not playing):", soundAfterDismiss);
  const consoleFinal = await page.$eval("#consolePane", (el) => el.textContent);
  console.log("console mentions dismiss:", consoleFinal!.includes("dismiss"));

  console.log("errors:", errors.length ? errors : "none");
} finally {
  await browser.close();
}
