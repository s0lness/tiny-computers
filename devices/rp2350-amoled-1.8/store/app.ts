// store/app.ts
//
// Local app-store for the RP2350-Touch-AMOLED-1.8. Runs on 127.0.0.1:5320
// only: no wireless on this board, and this is a single-user, single-machine
// tool by nature (see ~/.claude/design/local-app.md).
//
// Shows the two flash slots (see partitions.json), what's installed in
// each (best-effort: see the "active" caveat in getState() below), lists
// installable apps from catalog.json, and installs/uninstalls by shelling
// out to picotool.
//
// IMPORTANT, read README.md "picotool finding" before changing installApp():
// `picotool load -p <partition>` does NOT relocate a file's addresses. The
// bytes get written wherever the ELF/UF2 says they belong, which was fixed
// at link time. So every app must be BUILT for its target slot's real flash
// address, not just copied there — that's why installApp() builds on demand
// (buildApp()) instead of picking a prebuilt .uf2 out of the catalog.
//
// None of the picotool calls in this file have been run against real
// hardware: the board was in active use by someone else while this was
// written (see the task's hard constraints). Treat this as reviewed-but-
// untested and verify the first install/uninstall by hand.

import { unlink } from "node:fs/promises";
import { join } from "node:path";

const ROOT = import.meta.dir;
const REPO_ROOT = join(ROOT, "..");
const PORT = 5320;
const HOSTNAME = "127.0.0.1";
const CSRF_HEADER = "x-rp2350-store";

// Toolchain paths, copied from ../AGENTS.md's documented build recipe (this
// machine is Windows on ARM64 with no host C compiler, hence the prebuilt
// x64 picotool/pioasm and the cmake/ninja borrowed from the ESP-IDF install).
const USERPROFILE = process.env.USERPROFILE ?? "";
const PICO_SDK_PATH = join(USERPROFILE, "pico", "pico-sdk");
const PICO_TOOLCHAIN_PATH = "C:\\Program Files (x86)\\Arm GNU Toolchain arm-none-eabi\\14.2 rel1";
const PICOTOOL_DIR = join(USERPROFILE, "pico", "tools", "picotool-dist", "picotool");
const PICOTOOL_EXE = join(PICOTOOL_DIR, "picotool.exe");
const PIOASM_DIR = join(USERPROFILE, "pico", "tools", "sdk-tools", "pioasm");
const CMAKE_BIN = join(USERPROFILE, ".espressif", "tools", "cmake", "3.30.2", "bin");
const NINJA_BIN = join(USERPROFILE, ".espressif", "tools", "ninja", "1.12.1");

function buildEnv(): Record<string, string> {
  const path = [CMAKE_BIN, NINJA_BIN, join(PICO_TOOLCHAIN_PATH, "bin"), process.env.PATH ?? ""].join(";");
  return { ...process.env, PICO_SDK_PATH, PICO_TOOLCHAIN_PATH, PATH: path } as Record<string, string>;
}

/* -----------------------------------------------------------------------
 * partitions.json / catalog.json
 * -------------------------------------------------------------------- */
type Slot = "a" | "b";
const SLOT_PARTITION_NAME: Record<Slot, string> = { a: "slot_a", b: "slot_b" };

type PartitionEntry = { name: string; id: number; start: number; size: number; families: string[] };

async function loadPartitions(): Promise<PartitionEntry[]> {
  const raw = (await Bun.file(join(ROOT, "partitions.json")).json()) as { partitions: PartitionEntry[] };
  return raw.partitions;
}

async function slotPartition(slot: Slot): Promise<PartitionEntry> {
  const parts = await loadPartitions();
  const p = parts.find((p) => p.name === SLOT_PARTITION_NAME[slot]);
  if (!p) throw new Error(`partitions.json has no ${SLOT_PARTITION_NAME[slot]} entry`);
  return p;
}

async function manifestPartition(): Promise<PartitionEntry> {
  const parts = await loadPartitions();
  const p = parts.find((p) => p.name === "manifest");
  if (!p) throw new Error("partitions.json has no manifest entry");
  return p;
}

const XIP_BASE = 0x10000000;
function xip(storageOffset: number): number {
  return XIP_BASE + storageOffset;
}
function hex(n: number): string {
  return "0x" + n.toString(16).padStart(8, "0");
}

type CatalogApp = { id: string; name: string; description: string; sourceDir: string; target: string };

async function loadCatalog(): Promise<CatalogApp[]> {
  const raw = (await Bun.file(join(ROOT, "catalog.json")).json()) as { apps: CatalogApp[] };
  return raw.apps;
}

/* -----------------------------------------------------------------------
 * Manifest partition: a small, hand-packed binary record of what the store
 * last installed into each slot, and which one it last told the bootrom to
 * prefer. This is the ONLY durable memory of slot contents: the store
 * itself keeps no local database, so this partition is the source of truth
 * across restarts of the store, other picotool use, or a fresh device.
 * -------------------------------------------------------------------- */
const MANIFEST_MAGIC = "RP35AS02"; // 8 ascii bytes; deliberately different from the
// retired store/launcher's "RP35APPS" so a stale 6-slot manifest is never
// misread as valid under the new 2-slot layout.
const MANIFEST_NAME_LEN = 32;
const MANIFEST_ACTIVE_NONE = 0xff;

type ManifestState = {
  valid: boolean;
  active: Slot | null;
  names: { a: string; b: string };
};

// Layout: magic[8] version[1] active[1] reserved[2] name_a[32] name_b[32] = 76 bytes,
// zero-padded up to the partition's full size on write.
function encodeManifest(state: { active: Slot | null; names: { a: string; b: string } }, size: number): Uint8Array {
  const buf = new Uint8Array(size);
  const enc = new TextEncoder();
  buf.set(enc.encode(MANIFEST_MAGIC), 0);
  buf[8] = 1; // version
  buf[9] = state.active === "a" ? 0 : state.active === "b" ? 1 : MANIFEST_ACTIVE_NONE;
  const writeName = (name: string, offset: number) => {
    const bytes = enc.encode(name).slice(0, MANIFEST_NAME_LEN - 1);
    buf.set(bytes, offset); // remainder of the 32 bytes stays zero: NUL-padded
  };
  writeName(state.names.a, 12);
  writeName(state.names.b, 12 + MANIFEST_NAME_LEN);
  return buf;
}

function decodeManifest(buf: Uint8Array): ManifestState {
  const dec = new TextDecoder();
  const magic = dec.decode(buf.slice(0, 8));
  if (magic !== MANIFEST_MAGIC) return { valid: false, active: null, names: { a: "", b: "" } };
  const activeByte = buf[9];
  const active: Slot | null = activeByte === 0 ? "a" : activeByte === 1 ? "b" : null;
  const readName = (offset: number) => {
    const raw = buf.slice(offset, offset + MANIFEST_NAME_LEN);
    const nul = raw.indexOf(0);
    return dec.decode(nul >= 0 ? raw.slice(0, nul) : raw);
  };
  return { valid: true, active, names: { a: readName(12), b: readName(12 + MANIFEST_NAME_LEN) } };
}

/* -----------------------------------------------------------------------
 * picotool wrapper
 * -------------------------------------------------------------------- */
async function runPicotool(args: string[]): Promise<{ ok: boolean; stdout: string; stderr: string }> {
  const proc = Bun.spawn([PICOTOOL_EXE, ...args], { env: buildEnv(), stdout: "pipe", stderr: "pipe" });
  const [stdout, stderr, exitCode] = await Promise.all([
    new Response(proc.stdout).text(),
    new Response(proc.stderr).text(),
    proc.exited,
  ]);
  return { ok: exitCode === 0, stdout, stderr };
}

async function runTool(cmd: string, args: string[], env: Record<string, string>): Promise<{ ok: boolean; log: string }> {
  const proc = Bun.spawn([cmd, ...args], { env, stdout: "pipe", stderr: "pipe" });
  const [out, err, code] = await Promise.all([
    new Response(proc.stdout).text(),
    new Response(proc.stderr).text(),
    proc.exited,
  ]);
  return { ok: code === 0, log: out + err };
}

function tmpPath(prefix: string): string {
  return join(ROOT, `.tmp-${prefix}-${Date.now()}-${Math.random().toString(36).slice(2)}.bin`);
}

/* -----------------------------------------------------------------------
 * Flash reads: occupancy scan (mirrors the retired store/launcher's
 * slot_occupied()) and the manifest read.
 * -------------------------------------------------------------------- */
const PICOBIN_BLOCK_MARKER_START = 0xffffded3; // boot/picobin.h, pico-sdk
const OCCUPANCY_SCAN_BYTES = 4096;

async function readFlashRange(from: number, to: number): Promise<Uint8Array | null> {
  const tmp = tmpPath("read");
  try {
    const res = await runPicotool(["save", "-r", hex(from), hex(to), tmp, "-t", "bin", "-f"]);
    if (!res.ok) return null;
    const bytes = await Bun.file(tmp).arrayBuffer();
    return new Uint8Array(bytes);
  } finally {
    await unlink(tmp).catch(() => {});
  }
}

async function slotOccupied(slot: Slot): Promise<boolean | null> {
  const part = await slotPartition(slot);
  const from = xip(part.start);
  const to = from + OCCUPANCY_SCAN_BYTES;
  const bytes = await readFlashRange(from, to);
  if (!bytes) return null; // device unreachable
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  for (let off = 0; off + 4 <= bytes.length; off += 4) {
    if (view.getUint32(off, true) === PICOBIN_BLOCK_MARKER_START) return true;
  }
  return false;
}

async function readManifest(): Promise<ManifestState | null> {
  const part = await manifestPartition();
  const from = xip(part.start);
  const to = from + part.size;
  const bytes = await readFlashRange(from, to);
  if (!bytes) return null;
  return decodeManifest(bytes);
}

async function writeManifest(state: { active: Slot | null; names: { a: string; b: string } }): Promise<boolean> {
  const part = await manifestPartition();
  const buf = encodeManifest(state, part.size);
  const tmp = tmpPath("write");
  try {
    await Bun.write(tmp, buf);
    const res = await runPicotool(["load", tmp, "-t", "bin", "-p", String(part.id), "-f"]);
    return res.ok;
  } finally {
    await unlink(tmp).catch(() => {});
  }
}

/* -----------------------------------------------------------------------
 * Build on demand. See the file header: this exists because picotool does
 * not relocate a file, so each install links a fresh image for the chosen
 * slot's actual flash address (APP_FLASH_ORIGIN / APP_FLASH_LENGTH — cache
 * vars that firmware/CMakeLists.txt does not understand yet; see
 * README.md's integration diff, which is NOT applied by this repo yet).
 * -------------------------------------------------------------------- */
async function buildApp(app: CatalogApp, slot: Slot): Promise<{ ok: boolean; uf2Path: string; log: string }> {
  const part = await slotPartition(slot);
  const origin = hex(xip(part.start));
  const length = hex(part.size);
  const sourceDir = join(REPO_ROOT, app.sourceDir);
  // NOT firmware/build: that is the human's interactive dev build dir
  // (see ../AGENTS.md's "Running it" section) and must never be touched by
  // the store, since a live debugging session may have it open.
  const buildDir = join(sourceDir, `build-slot-${slot}`);

  const env = buildEnv();
  const configure = await runTool("cmake", [
    "-S", sourceDir,
    "-B", buildDir,
    "-G", "Ninja",
    `-Dpicotool_DIR=${PICOTOOL_DIR}`,
    `-Dpioasm_DIR=${PIOASM_DIR}`,
    `-DAPP_FLASH_ORIGIN=${origin}`,
    `-DAPP_FLASH_LENGTH=${length}`,
  ], env);
  if (!configure.ok) return { ok: false, uf2Path: "", log: configure.log };

  const build = await runTool("cmake", ["--build", buildDir], env);
  if (!build.ok) return { ok: false, uf2Path: "", log: configure.log + "\n" + build.log };

  const uf2Path = join(buildDir, `${app.target}.uf2`);
  if (!(await Bun.file(uf2Path).exists())) {
    return { ok: false, uf2Path: "", log: configure.log + "\n" + build.log + `\nbuild did not produce ${uf2Path}` };
  }
  return { ok: true, uf2Path, log: configure.log + "\n" + build.log };
}

/* -----------------------------------------------------------------------
 * Install / uninstall
 * -------------------------------------------------------------------- */
async function installApp(appId: string, slot: Slot): Promise<{ ok: boolean; message: string }> {
  const catalog = await loadCatalog();
  const app = catalog.find((a) => a.id === appId);
  if (!app) return { ok: false, message: `unknown app "${appId}"` };

  const built = await buildApp(app, slot);
  if (!built.ok) return { ok: false, message: "build failed:\n" + built.log };

  const part = await slotPartition(slot);
  // -x performs the flash-update reboot as part of the load, which is what
  // tells the bootrom to prefer this partition on the next boot (same
  // mechanism as the retired store/launcher's boot_app() and firmware's
  // documented flash command in ../AGENTS.md).
  const flash = await runPicotool(["load", built.uf2Path, "-p", String(part.id), "-f", "-x"]);
  if (!flash.ok) return { ok: false, message: "flash failed:\n" + flash.stdout + flash.stderr };

  const current = (await readManifest()) ?? { valid: false, active: null, names: { a: "", b: "" } };
  const names = { ...current.names, [slot]: app.name };
  const wrote = await writeManifest({ active: slot, names });
  if (!wrote) return { ok: false, message: `flashed "${app.name}" into slot ${slot.toUpperCase()}, but failed to update the manifest` };

  return { ok: true, message: `installed "${app.name}" into slot ${slot.toUpperCase()}` };
}

async function uninstallApp(slot: Slot): Promise<{ ok: boolean; message: string }> {
  const part = await slotPartition(slot);
  const current = (await readManifest()) ?? { valid: false, active: null, names: { a: "", b: "" } };
  const wasActive = current.active === slot;

  const erase = await runPicotool(["erase", "-p", String(part.id), "-f"]);
  if (!erase.ok) return { ok: false, message: "erase failed:\n" + erase.stdout + erase.stderr };

  const names = { ...current.names, [slot]: "" };
  let nextActive = current.active;
  let warning = "";
  if (wasActive) {
    const other: Slot = slot === "a" ? "b" : "a";
    const otherOccupied = await slotOccupied(other);
    if (otherOccupied) {
      nextActive = other;
      // picotool's CLI has no "prefer this partition" reboot outside of a
      // fresh `load -x` (see README.md's picotool finding); re-flashing the
      // sibling just to nudge the bootrom's preference is wasteful, so the
      // manifest is updated but the actual bootrom preference is left
      // alone. Documented here rather than faked as seamless.
      warning = " note: the manifest now marks slot " + other.toUpperCase() + " as active, but the " +
        "bootrom's own boot preference was not changed by this uninstall. Long-press PWR once, or " +
        "power-cycle the board, to actually hand off if it is still booting the erased slot.";
    } else {
      nextActive = null;
    }
  }
  await writeManifest({ active: nextActive, names });

  return { ok: true, message: `erased slot ${slot.toUpperCase()}.` + warning };
}

/* -----------------------------------------------------------------------
 * Aggregate state for the UI.
 * -------------------------------------------------------------------- */
async function getState() {
  const [manifest, occupiedA, occupiedB, catalog, partA, partB] = await Promise.all([
    readManifest(),
    slotOccupied("a"),
    slotOccupied("b"),
    loadCatalog(),
    slotPartition("a"),
    slotPartition("b"),
  ]);
  const deviceReachable = manifest !== null || occupiedA !== null || occupiedB !== null;
  return {
    deviceReachable,
    // "active" reflects the last slot the STORE told the bootrom to prefer
    // (via a flash-update reboot at install time), read back from the
    // manifest partition. It is not a live introspection of what the chip
    // is executing right now: that would need the running app to report
    // back over some channel, which this design deliberately doesn't have
    // (the whole point of appswitch.c is that apps switch by rebooting,
    // not by talking to a host tool).
    active: manifest?.active ?? null,
    slots: {
      a: { name: manifest?.names.a || null, occupied: occupiedA, start: hex(xip(partA.start)), size: partA.size },
      b: { name: manifest?.names.b || null, occupied: occupiedB, start: hex(xip(partB.start)), size: partB.size },
    },
    catalog,
  };
}

/* -----------------------------------------------------------------------
 * Front end. Kept to one inline <script>: this is a small admin page, not
 * worth a bundler step. See README.md for why a fetch() + custom header is
 * used for the mutating calls (local-app.md's CSRF guard: a plain HTML
 * form POST cannot set a custom header, so this locks out any other
 * localhost page).
 * -------------------------------------------------------------------- */
const PAGE = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>app store — rp2350-amoled-1.8</title>
<link rel="stylesheet" href="/family-budget.css">
<style>
  body { padding: 24px; max-width: 780px; margin: 0 auto; }
  h1 { font-size: 15px; text-transform: lowercase; letter-spacing: .07em; color: var(--ink); margin: 0 0 4px; }
  .sub { color: var(--muted); font-size: 12px; margin-bottom: 20px; }
  .slots { display: grid; grid-template-columns: 1fr 1fr; gap: 14px; margin-bottom: 22px; }
  .slot .appname { font-size: 16px; font-weight: 700; margin: 2px 0 10px; }
  .slot .appname.empty { color: var(--faint); font-weight: 400; font-style: italic; }
  .row { display: flex; gap: 8px; align-items: center; margin-top: 10px; }
  .catalog-item { display: flex; justify-content: space-between; align-items: center; gap: 16px; padding: 10px 0; border-bottom: 1px solid var(--border-fine); }
  .catalog-item:last-child { border-bottom: none; }
  .desc { color: var(--muted); font-size: 12px; max-width: 420px; }
  .warn { color: var(--danger); font-size: 12px; margin-bottom: 16px; }
  .footer { margin-top: 28px; display: flex; justify-content: space-between; align-items: center; gap: 16px; }
  #msg { font-size: 12px; margin-top: 10px; white-space: pre-wrap; }
  #msg.err { color: var(--danger); }
  #msg.ok { color: var(--positive); }
</style>
</head>
<body>
  <h1>app store</h1>
  <div class="sub">rp2350-amoled-1.8 — one app per slot, switch on device with a long press of PWR</div>

  <div id="offline" class="warn" style="display:none">no device reachable over USB right now (a picotool call failed). Connect the board and reload.</div>

  <div class="slots">
    <div class="card slot" id="slot-a">
      <h3>slot a</h3>
      <div class="appname">…</div>
      <div class="row"></div>
    </div>
    <div class="card slot" id="slot-b">
      <h3>slot b</h3>
      <div class="appname">…</div>
      <div class="row"></div>
    </div>
  </div>

  <h3>install</h3>
  <div class="card" id="catalog"></div>

  <div id="msg"></div>

  <div class="footer">
    <span class="hint">the manifest reflects the last slot the store told the bootrom to prefer, not literally what is executing right now.</span>
    <button class="btn sec sm" id="quit">quit</button>
  </div>

<script>
const CSRF_HEADER = ${JSON.stringify(CSRF_HEADER)};

function setMsg(text, cls) {
  const el = document.getElementById('msg');
  el.textContent = text;
  el.className = cls || '';
}

async function api(path, opts) {
  opts = opts || {};
  opts.headers = Object.assign({ [CSRF_HEADER]: '1' }, opts.headers || {});
  const res = await fetch(path, opts);
  return res.json();
}

function renderSlot(id, slot, active) {
  const el = document.getElementById(id);
  const nameEl = el.querySelector('.appname');
  const rowEl = el.querySelector('.row');
  if (slot.occupied === null) {
    nameEl.textContent = 'unknown (device unreachable)';
    nameEl.className = 'appname empty';
    rowEl.innerHTML = '';
    return;
  }
  if (!slot.occupied) {
    nameEl.textContent = 'empty';
    nameEl.className = 'appname empty';
    rowEl.innerHTML = '';
    return;
  }
  nameEl.textContent = slot.name || '(unnamed image)';
  nameEl.className = 'appname';
  rowEl.innerHTML = (active ? '<span class="status ok">active</span> ' : '') +
    '<button class="btn danger sm" data-uninstall>uninstall</button>';
  rowEl.querySelector('[data-uninstall]').onclick = async () => {
    const slotLetter = id.slice(-1);
    if (!confirm('erase slot ' + slotLetter.toUpperCase() + '?')) return;
    setMsg('erasing…');
    const r = await api('/api/uninstall', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ slot: slotLetter }),
    });
    setMsg(r.message, r.ok ? 'ok' : 'err');
    refresh();
  };
}

function renderCatalog(catalog) {
  const el = document.getElementById('catalog');
  el.innerHTML = '';
  catalog.forEach((app) => {
    const row = document.createElement('div');
    row.className = 'catalog-item';
    row.innerHTML =
      '<div><b>' + app.name + '</b><div class="desc">' + app.description + '</div></div>' +
      '<div><select data-slot><option value="a">slot a</option><option value="b">slot b</option></select>' +
      '<button class="btn sm" data-install>install</button></div>';
    row.querySelector('[data-install]').onclick = async () => {
      const slot = row.querySelector('[data-slot]').value;
      setMsg('building and flashing ' + app.name + ' into slot ' + slot.toUpperCase() + '… this can take a minute.');
      const r = await api('/api/install', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ appId: app.id, slot }),
      });
      setMsg(r.message, r.ok ? 'ok' : 'err');
      refresh();
    };
    el.appendChild(row);
  });
}

async function refresh() {
  const state = await api('/api/state');
  document.getElementById('offline').style.display = state.deviceReachable ? 'none' : 'block';
  renderSlot('slot-a', state.slots.a, state.active === 'a');
  renderSlot('slot-b', state.slots.b, state.active === 'b');
  renderCatalog(state.catalog);
}

document.getElementById('quit').onclick = async () => {
  await api('/api/quit', { method: 'POST' });
  document.body.innerHTML = '<div style="padding:40px;font:14px monospace;">store stopped. you can close this window.</div>';
};

refresh();
</script>
</body>
</html>`;

/* -----------------------------------------------------------------------
 * Server. Bound to 127.0.0.1 explicitly: Bun.serve({port}) with no
 * hostname listens on every interface, which is not acceptable for a
 * single-user local tool.
 * -------------------------------------------------------------------- */
Bun.serve({
  hostname: HOSTNAME,
  port: PORT,
  async fetch(req) {
    const url = new URL(req.url);

    if (req.method === "GET" && url.pathname === "/") {
      return new Response(PAGE, { headers: { "content-type": "text/html; charset=utf-8" } });
    }

    if (req.method === "GET" && url.pathname === "/family-budget.css") {
      return new Response(Bun.file(join(ROOT, "family-budget.css")), { headers: { "content-type": "text/css" } });
    }

    if (req.method === "GET" && url.pathname === "/api/state") {
      return Response.json(await getState());
    }

    const mutating = req.method === "POST" && ["/api/install", "/api/uninstall", "/api/quit"].includes(url.pathname);
    if (mutating && req.headers.get(CSRF_HEADER) !== "1") {
      // localhost is reachable from any page in any browser tab; the custom
      // header guard closes that off since a cross-origin fetch can't set
      // one without a CORS preflight this server never answers.
      return new Response("missing csrf header", { status: 403 });
    }

    if (req.method === "POST" && url.pathname === "/api/install") {
      const body = (await req.json()) as { appId: string; slot: Slot };
      return Response.json(await installApp(body.appId, body.slot));
    }

    if (req.method === "POST" && url.pathname === "/api/uninstall") {
      const body = (await req.json()) as { slot: Slot };
      return Response.json(await uninstallApp(body.slot));
    }

    if (req.method === "POST" && url.pathname === "/api/quit") {
      setTimeout(() => process.exit(0), 150);
      return Response.json({ ok: true, stopping: true });
    }

    return new Response("not found", { status: 404 });
  },
});

console.log(`app store on http://${HOSTNAME}:${PORT}`);
