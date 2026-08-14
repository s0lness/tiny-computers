// Headless check for what's actually verifiable without the real wasm
// module (built separately, see emulator/wasm/emu_abi.h; this repo does
// not build it). What this proves: the page loads, does NOT show a blank
// screen when the module is missing, and reports the failure clearly on
// the page itself, per the emulator's own contract for that case (see
// main.ts, showWasmError). Once a real emu.wasm exists at
// emulator/wasm/dist/emu.wasm, this same script's assertions flip: rerun
// it and it should instead report the device came up (see the inverse
// checks below).
import puppeteer from "puppeteer-core";
import { join } from "node:path";
import { existsSync } from "node:fs";

const ROOT = join(import.meta.dir, "..");
const PORT = 53309;
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

const server = Bun.spawn(["bun", "run", "server.ts"], {
  cwd: ROOT,
  // DEVLINK_MODE=off: this is a headless CI-style check, not a session
  // where a real board should ever be touched. server.ts's own default
  // (DEVLINK_MODE unset) is "real" - it detects and opens whatever board
  // is actually plugged into this machine, which a verify script must
  // never do by accident (see devlink-host.ts / server.ts).
  env: { ...process.env, PORT: String(PORT), DEVLINK_MODE: "off" },
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

  // NOT networkidle0: the page opens a live-reload websocket on boot
  // (main.ts, connectLiveReload) that stays open and reconnects on close by
  // design, so the network never goes idle and that waitUntil would hang
  // forever.
  await page.goto(`http://127.0.0.1:${PORT}/`, { waitUntil: "domcontentloaded" });
  // The boot sequence is async (fetch -> instantiate -> emu_init); give it
  // a moment to settle either way.
  await new Promise((r) => setTimeout(r, 1000));

  const bodyHasContent = await page.evaluate(() => document.body.textContent!.trim().length > 0);
  if (!bodyHasContent) fail("page body is empty (blank screen) after boot attempt");
  console.log("page rendered non-empty content");

  const wasmBuilt = existsSync(WASM_FILE);

  if (!wasmBuilt) {
    console.log(`${WASM_FILE} does not exist -- checking the graceful-failure path`);
    const errorShown = await page.evaluate(() => {
      const el = document.querySelector("#wasmError");
      return el && !el.classList.contains("hidden") && (el.textContent || "").length > 0;
    });
    if (!errorShown) fail("wasm module is missing but #wasmError is not showing a message: this would look like a silent blank/broken page");
    const errorText = await page.$eval("#wasmError", (el) => el.textContent || "");
    console.log(`#wasmError shows: ${errorText.slice(0, 200)}`);
    if (!/wasm/i.test(errorText)) fail("#wasmError message does not even mention wasm; not clear enough to act on");
    console.log("PASS: missing-module failure is surfaced clearly on the page, not a blank screen");
  } else {
    console.log(`${WASM_FILE} exists -- checking the device actually came up`);
    const deviceName = await page.$eval("#deviceName", (el) => el.textContent || "");
    const errorHidden = await page.evaluate(() => document.querySelector("#wasmError")?.classList.contains("hidden") ?? true);
    if (!errorHidden) {
      const errorText = await page.$eval("#wasmError", (el) => el.textContent || "");
      fail(`wasm module exists but the page still shows an error: ${errorText}`);
    }
    console.log(`PASS: device came up ("${deviceName}"), no error banner`);
  }
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
