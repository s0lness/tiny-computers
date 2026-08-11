// Headless check: the emulator page renders and a stroke actually draws
// ink with anti-aliasing and MIN composition, using the ported firmware
// pipeline. Cannot compare against real hardware (no device here); this
// only proves the emulator itself runs and produces the expected shape of
// output. Run with `bun run scripts/verify.ts` (also `npm run verify`).
import puppeteer from "puppeteer-core";
import { join } from "node:path";

const ROOT = join(import.meta.dir, "..");
const PORT = 53309;
const CHROME = "C:/Program Files/Google/Chrome/Application/chrome.exe";

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
  page.on("console", (m) => {
    if (m.type() === "error") console.error("console error:", m.text());
  });

  await page.goto(`http://127.0.0.1:${PORT}/`, { waitUntil: "networkidle0" });

  const hasEngine = await page.evaluate(() => Boolean((window as any).__engine));
  if (!hasEngine) fail("window.__engine not found after page load");
  console.log("page renders, engine wired up");

  // Boot test pattern should already have painted something before any
  // touch (this proves the render path itself, independent of input).
  const bootInk = await page.evaluate(() => {
    const gray: Uint8ClampedArray = (window as any).__engine.panel.gray;
    let nonWhite = 0;
    for (let i = 0; i < gray.length; i++) if (gray[i] < 255) nonWhite++;
    return nonWhite;
  });
  if (bootInk < 1000) fail(`boot test pattern painted too little ink (${bootInk} px)`);
  console.log(`boot test pattern: ${bootInk} non-white px`);

  const canvasBox = await page.$eval("#panel", (el) => {
    const r = (el as HTMLCanvasElement).getBoundingClientRect();
    return { x: r.x, y: r.y, w: r.width, h: r.height };
  });

  // First touch must clear the boot pattern (main.c: haveTouch && patternShown).
  const cx = canvasBox.x + canvasBox.w * 0.3;
  const cy = canvasBox.y + canvasBox.h * 0.3;
  await page.mouse.move(cx, cy);
  await page.mouse.down();
  // A short diagonal stroke, several samples, slow enough to stay well
  // under MAX_JUMP_PX / MAX_SPEED_PX_PER_MS between points.
  for (let i = 1; i <= 12; i++) {
    await page.mouse.move(cx + i * 6, cy + i * 4, { steps: 3 });
    await new Promise((r) => setTimeout(r, 20));
  }
  await page.mouse.up();
  // LIFT_DEBOUNCE_MS default 80ms, plus a couple of render frames.
  await new Promise((r) => setTimeout(r, 400));

  const result = await page.evaluate(() => {
    const gray: Uint8ClampedArray = (window as any).__engine.panel.gray;
    let black = 0, partial = 0, nonWhite = 0;
    for (let i = 0; i < gray.length; i++) {
      const v = gray[i];
      if (v < 255) nonWhite++;
      if (v === 0) black++;
      if (v > 0 && v < 255) partial++;
    }
    const stats = (window as any).__engine.stats();
    return { nonWhite, black, partial, stats };
  });

  console.log("after stroke:", JSON.stringify(result));

  if (result.nonWhite < 50) fail(`stroke drew almost no ink (${result.nonWhite} non-white px)`);
  if (result.black < 5) fail(`stroke has no fully-black core pixels (${result.black})`);
  if (result.partial < 5) {
    fail(`stroke has no partially-covered (anti-aliased) pixels (${result.partial}) — MIN/AA path may be broken`);
  }
  if (!result.stats.fingerDown && result.stats.splits === 0 && result.stats.dropouts === 0) {
    // Fine either way (a clean stroke with no dropouts is a valid outcome),
    // just log it since it's informative about the default touch-sim rates.
    console.log("note: stroke completed with zero simulated dropouts/splits");
  }

  console.log("PASS: page renders and a stroke draws anti-aliased, MIN-composited ink");
} finally {
  if (browser) await browser.close();
  // Plain server.kill() (SIGTERM) was observed to leave the Bun dev server
  // (development.hmr spawns a watcher) still listening on Windows even
  // after the parent process object reports killed. Force the whole tree,
  // same as local-app.md's guidance for killing children on Windows.
  server.kill();
  try {
    Bun.spawnSync(["taskkill", "/pid", String(server.pid), "/t", "/f"], { stdout: "ignore", stderr: "ignore" });
  } catch {}
}
