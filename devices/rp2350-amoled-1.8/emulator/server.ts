// Device emulator: local server. Serves the page, the compiled wasm module
// (built separately, see emulator/wasm/emu_abi.h), and a handful of small
// write routes: /api/quit (unchanged from before), /api/freeze and
// /api/trace (write debugging artifacts to a predictable path an agent can
// read directly), /api/livereload (a websocket the page listens on so
// editing firmware and rebuilding shows up on screen without a manual
// refresh: "fast iteration" is the whole point of this being a local tool),
// and /api/devlink + /api/devlink/status (the real board: detection, the
// single serial-port owner, and the board's own console output - see
// devlink-host.ts for the whole story).
//
// Launched by hand (`bun run server.ts` / `bun dev`); no console to Ctrl+C
// once backgrounded, so the quit button is not optional (local-app.md).
import { watch, existsSync, mkdirSync, writeFileSync, statSync, openSync, readSync, closeSync } from "node:fs";
import { join, basename } from "node:path";
import index from "./src/index.html";
import { DevlinkHost, type DevlinkMode } from "./devlink-host";

const PORT = Number(process.env.PORT) || 5330;

const ROOT = import.meta.dir;
const WASM_DIST_DIR = join(ROOT, "wasm", "dist");
const FREEZES_DIR = join(ROOT, "freezes");
const TRACES_DIR = join(ROOT, "traces");

function guard(req: Request): boolean {
  // A simple cross-origin POST cannot set a custom header without a
  // preflight we never answer, so this closes the localhost-CSRF hole.
  return req.headers.get("x-rp2350-emulator") === "1";
}

function quit(req: Request): Response {
  if (!guard(req)) return new Response("nope", { status: 403 });
  // Releases the real COM port (if this server is holding one) before
  // exiting: devlinkHost.stop() awaits the bridge's own close(), which in
  // turn awaits the PowerShell child's exit (bounded, see tools/dev.ts's
  // openDirectBridge). Quitting without this would leave the port locked
  // by a lingering child process until the OS or a manual kill cleaned it
  // up - exactly the kind of "single owner" bug this task exists to close.
  setTimeout(() => {
    void devlinkHost.stop().finally(() => process.exit(0));
  }, 150);
  return Response.json({ ok: true, stopping: true });
}

// Serves the wasm module (and anything else the wasm build writes next to
// it) straight off disk. Not part of the HTML bundler's asset graph,
// because the page fetches it at runtime by a plain string URL, not a
// static import (see wasm.ts: the URL is a parameter on purpose, so a
// firmware author can point this at a different build), so it needs an
// explicit route rather than relying on Bun's HTML-import bundler to
// notice it.
async function serveWasmFile(req: Request): Promise<Response> {
  const url = new URL(req.url);
  const name = basename(decodeURIComponent(url.pathname.slice("/wasm/".length)));
  const path = join(WASM_DIST_DIR, name);
  const f = Bun.file(path);
  if (!(await f.exists())) {
    return new Response(
      `wasm module not built yet (looked for ${name} in emulator/wasm/dist/). ` +
        `That half of this project builds separately; see emulator/wasm/emu_abi.h.`,
      { status: 404 }
    );
  }
  const type = name.endsWith(".wasm") ? "application/wasm" : "application/octet-stream";
  return new Response(f, { headers: { "content-type": type, "cache-control": "no-store" } });
}

// Freeze bundle: a screenshot exactly as displayed plus a JSON of
// everything around it (device descriptor, current app, recent pushes,
// recent input, recent console lines, and any annotation journal), written
// to a predictable path rather than a browser download, because the
// intended reader is a coding agent working in this repo, not a human
// digging through a Downloads folder. Two writes: a timestamped archive
// (freezes/<id>/) and an always-fresh freezes/latest/ mirror, the same
// history-plus-LATEST convention used elsewhere in this owner's tooling.
//
// A second POST with the same id (the annotation modal saving after the
// plain freeze already landed) overwrites that same directory rather than
// creating a new one.
async function saveFreeze(req: Request): Promise<Response> {
  let body: Record<string, unknown>;
  try {
    body = (await req.json()) as Record<string, unknown>;
  } catch {
    return new Response("bad json", { status: 400 });
  }
  const { panelPngBase64, id: rawId, ...rest } = body;
  if (typeof panelPngBase64 !== "string") return new Response("missing panelPngBase64", { status: 400 });
  const id = typeof rawId === "string" && /^[0-9A-Za-z_-]+$/.test(rawId) ? rawId : new Date().toISOString().replace(/[:.]/g, "-");

  const pngBuf = Buffer.from(panelPngBase64, "base64");
  const bundleText = JSON.stringify({ ...rest, panelPngPath: "panel.png" }, null, 2);

  for (const dir of [join(FREEZES_DIR, id), join(FREEZES_DIR, "latest")]) {
    mkdirSync(dir, { recursive: true });
    writeFileSync(join(dir, "panel.png"), pngBuf);
    writeFileSync(join(dir, "bundle.json"), bundleText);
  }
  console.log(`freeze -> emulator/freezes/${id}/`);
  return Response.json({ id, path: `emulator/freezes/${id}/` });
}

// Full input trace: "a bug becomes a FILE that replays exactly" (see
// recorder.ts / replay.ts). Timestamped archive plus a latest.json mirror.
async function saveTrace(req: Request): Promise<Response> {
  let body: unknown;
  try {
    body = await req.json();
  } catch {
    return new Response("bad json", { status: 400 });
  }
  mkdirSync(TRACES_DIR, { recursive: true });
  const stamp = new Date().toISOString().replace(/[:.]/g, "-");
  const text = JSON.stringify(body, null, 2);
  writeFileSync(join(TRACES_DIR, `${stamp}.json`), text);
  writeFileSync(join(TRACES_DIR, "latest.json"), text);
  console.log(`trace -> emulator/traces/${stamp}.json`);
  return Response.json({ path: `emulator/traces/${stamp}.json` });
}

// ---- live reload: watch the wasm build output, tell connected pages -----
//
// A dev server that reloads on any filesystem event is a trap the moment
// the file is being written by something else (a build tool) rather than
// atomically renamed into place: an event fires the instant the OS creates
// or truncates the file, long before the build has finished writing bytes
// into it. Broadcasting "reload" at that instant tells the page to fetch a
// half-written module, which fails in a way that looks exactly like a
// frozen page (see this task's freeze investigation - that is very likely
// what actually happened). This is not a one-off; it is a standing
// property of watch-and-reload, so it is fixed here rather than papered
// over on the client.
//
// Two independent guards before a client ever hears about a change:
//   1. the file's size and mtime must be STABLE across two consecutive
//      polls (not just "no fs event for N ms" - a build tool can pause
//      between writes for longer than any fixed debounce);
//   2. it must actually start with the wasm magic bytes once stable.
// Neither is a substitute for the client's own validate-then-swap
// (wasm.ts's instantiate(), main.ts's bringUp()): a file can be
// byte-stable and start with \0asm and still fail to compile or
// instantiate, and that failure belongs to the client, which is the only
// side that can actually attempt it against the real import object.
const wsClients = new Set<import("bun").ServerWebSocket<unknown>>();
const WASM_FILE = join(WASM_DIST_DIR, "emu.wasm");

let watchGen = 0;

function looksLikeWasm(path: string): boolean {
  let fd: number;
  try {
    fd = openSync(path, "r");
  } catch {
    return false;
  }
  try {
    const magic = Buffer.alloc(4);
    const n = readSync(fd, magic, 0, 4, 0);
    return n === 4 && magic[0] === 0x00 && magic[1] === 0x61 && magic[2] === 0x73 && magic[3] === 0x6d;
  } catch {
    return false;
  } finally {
    closeSync(fd);
  }
}

// Polls until two consecutive reads see the same size and mtime, or gives
// up after ~24s (a build that slow will fire another fs event on its own
// completion, which starts a fresh wait); null either way means "do not
// broadcast yet".
async function waitForStableFile(path: string, myGen: number): Promise<{ size: number; mtimeMs: number } | null> {
  const POLL_MS = 120;
  let lastSize = -1;
  let lastMtime = -1;
  let stableCount = 0;
  for (let i = 0; i < 200; i++) {
    await new Promise((r) => setTimeout(r, POLL_MS));
    if (myGen !== watchGen) return null; // a newer change superseded this wait
    let st;
    try {
      st = statSync(path);
    } catch {
      stableCount = 0;
      lastSize = -1;
      continue; // mid-write / mid-rename: briefly missing is normal
    }
    if (st.size === lastSize && st.mtimeMs === lastMtime) {
      if (++stableCount >= 2) return { size: st.size, mtimeMs: st.mtimeMs };
    } else {
      stableCount = 0;
      lastSize = st.size;
      lastMtime = st.mtimeMs;
    }
  }
  return null;
}

function onWasmDirChange(): void {
  const myGen = ++watchGen;
  void (async () => {
    const stat = await waitForStableFile(WASM_FILE, myGen);
    if (!stat || myGen !== watchGen) return; // gave up, or a newer change is already being waited on
    if (!looksLikeWasm(WASM_FILE)) {
      console.warn(`${WASM_FILE} settled at ${stat.size} bytes but does not start with the wasm magic bytes; not telling clients to reload`);
      return;
    }
    if (wsClients.size === 0) return;
    console.log(`wasm stable at ${stat.size} bytes, telling ${wsClients.size} connected page(s) to reload`);
    for (const ws of wsClients) ws.send("reload");
  })();
}

function startWasmWatcher(): void {
  try {
    watch(WASM_DIST_DIR, () => onWasmDirChange());
    console.log(`watching ${WASM_DIST_DIR} for changes`);
  } catch {
    // Doesn't exist yet (the wasm build hasn't run once). Poll for it
    // rather than giving up: this dev server is very likely started
    // before the first build finishes.
    const timer = setInterval(() => {
      if (existsSync(WASM_DIST_DIR)) {
        clearInterval(timer);
        startWasmWatcher();
      }
    }, 2000);
  }
}
startWasmWatcher();

// ---- devlink: single owner of the real board's serial port -------------
//
// DEVLINK_MODE defaults to "real" (the owner just plugs a board in and it
// is noticed - see devlink-host.ts's own header for the detection/ownership
// story), which is why this server must NEVER be started this way during
// development or testing on a machine where a real board might be
// connected: "fake" (the in-process loopback fixture, harness/fixtures/
// loopbackLink.ts) or "off" (touches nothing at all) are what every check
// in scripts/verify-ui-feedback.ts passes explicitly.
const DEVLINK_MODE = (process.env.DEVLINK_MODE as DevlinkMode | undefined) ?? "real";
const DEVLINK_FAKE_TUNABLES = process.env.DEVLINK_FAKE_TUNABLES === "mismatch" ? "mismatch" : "default";

const devlinkHost = new DevlinkHost({ mode: DEVLINK_MODE, fakeTunables: DEVLINK_FAKE_TUNABLES });
const devlinkClients = new Set<import("bun").ServerWebSocket<unknown>>();

devlinkHost.onStatus((status) => {
  const msg = JSON.stringify({ type: "status", ...status });
  for (const ws of devlinkClients) ws.send(msg);
});
devlinkHost.onLine((text) => {
  const msg = JSON.stringify({ type: "line", text });
  for (const ws of devlinkClients) ws.send(msg);
});
await devlinkHost.start();

type WsKind = { kind: "devlink" | "livereload" };

const server = Bun.serve<WsKind>({
  port: PORT,
  // Local-only tool: without an explicit hostname Bun listens on every
  // interface (:::5330), reachable by anyone on the same network.
  hostname: "127.0.0.1",
  development: { hmr: true, console: true },
  websocket: {
    open(ws) {
      const kind = (ws.data as WsKind | undefined)?.kind;
      if (kind === "devlink") {
        devlinkClients.add(ws);
        ws.send(JSON.stringify({ type: "status", ...devlinkHost.status }));
      } else {
        wsClients.add(ws);
      }
    },
    close(ws) {
      wsClients.delete(ws);
      devlinkClients.delete(ws);
    },
    message(ws, message) {
      const kind = (ws.data as WsKind | undefined)?.kind;
      if (kind !== "devlink") return; // livereload: the page never sends anything on that socket
      let msg: unknown;
      try {
        msg = JSON.parse(String(message));
      } catch {
        return;
      }
      const m = msg as { type?: string; cmd?: string };
      if (m.type === "send" && typeof m.cmd === "string") {
        if (!devlinkHost.sendRaw(m.cmd)) {
          ws.send(JSON.stringify({ type: "error", error: "no device connected" }));
        }
      }
    },
  },
  routes: {
    "/api/quit": { POST: quit },
    "/api/freeze": { POST: saveFreeze },
    "/api/trace": { POST: saveTrace },
    "/api/livereload": {
      GET(req: Request, srv: import("bun").Server<WsKind>) {
        if (srv.upgrade(req, { data: { kind: "livereload" } })) return;
        return new Response("upgrade failed", { status: 400 });
      },
    },
    // Single owner of the real serial port (devlink-host.ts): this route
    // never opens a port itself, it only relays to/from whatever
    // DevlinkHost already holds. /status is a plain GET so tools/dev.ts can
    // cheaply check "is the server up AND holding a bridge right now"
    // before deciding whether to route a command through the socket below
    // or fall back to opening the port directly (see dev.ts's openBridge).
    "/api/devlink/status": {
      GET() {
        return Response.json(devlinkHost.status);
      },
    },
    "/api/devlink": {
      GET(req: Request, srv: import("bun").Server<WsKind>) {
        if (srv.upgrade(req, { data: { kind: "devlink" } })) return;
        return new Response("upgrade failed", { status: 400 });
      },
    },
    "/wasm/*": serveWasmFile,
    "/*": index,
  },
});

console.log(`device emulator -> http://127.0.0.1:${server.port}`);
console.log(`markup annotation: bun build.ts, then bun ../markup/serve.ts --dir "${import.meta.dir}/dist" --port 5332`);
