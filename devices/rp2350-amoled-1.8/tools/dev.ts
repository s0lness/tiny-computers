#!/usr/bin/env bun
// devlink host CLI: `bun tools/dev.ts <command> ...`
//
// Talks to the RP2350's devlink firmware module (firmware/devlink.c) over
// its USB CDC serial port. See tools/README-devlink.md for the wire
// protocol. This has been run against real hardware; every read loop below
// tolerates the runtime's own debug prints (profiler ticks, app-switch and
// stroke logs, and anything else printed on the same port) by skipping any
// line that does not match the reply shape it is currently waiting for,
// rather than by assuming the first line back is the answer.
//
// Bun has no built-in serial port support on Windows and this repo carries
// no native serial dependency, so this spawns a small PowerShell child
// process that opens the COM port via System.IO.Ports.SerialPort and
// bridges it to its own stdin/stdout: our stdin lines become writes to the
// port, and raw bytes read from the port are written straight to our
// stdout. dev.ts talks to that child, never to the port directly.

import os from "node:os";
import path from "node:path";

const PORT = process.env.DEVLINK_PORT ?? "COM4";
const BAUD = process.env.DEVLINK_BAUD ?? "115200";

// ---------------------------------------------------------------------
// PowerShell serial bridge
// ---------------------------------------------------------------------
//
// Critical gotcha (see AGENTS.md): DtrEnable must be set to $true BEFORE
// Open(), or the device's USB CDC looks dead even though it is running
// fine. Getting that ordering wrong is the single most likely reason this
// would fail on first real use.
//
// Reading and writing happen on separate threads (a background runspace
// for port -> stdout, the main thread for stdin -> port) because
// System.IO.Ports.SerialPort has no natural way to multiplex both
// directions from one thread without either polling or blocking one side.
const BRIDGE_PS1 = `
param(
    [Parameter(Mandatory=$true)][string]$Port,
    [Parameter(Mandatory=$true)][int]$Baud
)

$ErrorActionPreference = "Stop"

$p = New-Object System.IO.Ports.SerialPort $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$p.DtrEnable = $true   # MUST be set before Open(), or the device reads as dead.
$p.ReadTimeout = 500
$p.WriteTimeout = 2000
$p.NewLine = "\`n"

try {
    $p.Open()
} catch {
    [Console]::Error.WriteLine("devlink-bridge: failed to open $Port at $Baud baud: $($_.Exception.Message)")
    exit 1
}

$stdout = [Console]::OpenStandardOutput()

# Serial -> stdout, on a background runspace, so it never blocks the stdin
# read loop below. Writes raw bytes straight to the process's own stdout
# stream (bypassing PowerShell's object pipeline / formatting entirely),
# which is what makes it safe to carry the binary-ish base64 SHOT payload.
$readerScript = {
    param($portRef, $streamRef)
    $buf = New-Object byte[] 4096
    while ($true) {
        try {
            $n = $portRef.Read($buf, 0, $buf.Length)
            if ($n -gt 0) {
                $streamRef.Write($buf, 0, $n)
                $streamRef.Flush()
            }
        } catch [System.TimeoutException] {
            continue
        } catch {
            break
        }
    }
}
$rs = [runspacefactory]::CreateRunspace()
$rs.Open()
$reader = [powershell]::Create()
$reader.Runspace = $rs
[void]$reader.AddScript($readerScript).AddArgument($p).AddArgument($stdout)
$readerHandle = $reader.BeginInvoke()

# stdin -> serial: block reading lines from our own stdin (dev.ts writes one
# command per line) and forward each as a line to the port. EOF on stdin
# (dev.ts closing the pipe) is how dev.ts asks this bridge to shut down.
try {
    $stdin = [Console]::In
    while ($true) {
        $line = $stdin.ReadLine()
        if ($null -eq $line) { break }
        $p.WriteLine($line)
    }
} finally {
    $reader.Stop()
    $reader.Dispose()
    $rs.Close()
    if ($p.IsOpen) { $p.Close() }
}
`;

async function writeBridgeScript(): Promise<string> {
  const p = path.join(os.tmpdir(), "rp2350-devlink-bridge.ps1");
  await Bun.write(p, BRIDGE_PS1);
  return p;
}

// ---------------------------------------------------------------------
// Line-oriented reader over the bridge's stdout
// ---------------------------------------------------------------------

// The shape expectLine()/readLineWithTimeout() actually depend on: "give me
// the next line, or null when the source is done." LineReader (below) is
// one implementation, backed by the PowerShell bridge's own stdout; a
// second one (WsLineSource, further down) is backed by the emulator
// server's /api/devlink socket instead, for when this tool routes THROUGH
// the server rather than opening the port itself (see openBridge()'s own
// comment for when each is used). Every read loop in this file is written
// against this interface, not the concrete class, so it works unchanged
// against either transport.
export interface LineSource {
  readLine(): Promise<string | null>;
}

export class LineReader implements LineSource {
  private reader: ReadableStreamDefaultReader<Uint8Array>;
  private buf = "";
  private decoder = new TextDecoder();

  constructor(stream: ReadableStream<Uint8Array>) {
    this.reader = stream.getReader();
  }

  // Returns the next complete line (without its terminator), or null if the
  // stream ended. A trailing \r (the firmware writes \r\n throughout) is
  // stripped.
  async readLine(): Promise<string | null> {
    for (;;) {
      const nl = this.buf.indexOf("\n");
      if (nl >= 0) {
        let line = this.buf.slice(0, nl);
        this.buf = this.buf.slice(nl + 1);
        if (line.endsWith("\r")) line = line.slice(0, -1);
        return line;
      }
      const { value, done } = await this.reader.read();
      if (done) {
        if (this.buf.length > 0) {
          const line = this.buf;
          this.buf = "";
          return line;
        }
        return null;
      }
      this.buf += this.decoder.decode(value, { stream: true });
    }
  }
}

export async function readLineWithTimeout(
  lines: LineSource,
  timeoutMs: number,
  what = "reply"
): Promise<string> {
  let timer: ReturnType<typeof setTimeout>;
  const timeout = new Promise<never>((_, reject) => {
    timer = setTimeout(
      () =>
        reject(
          new Error(
            `no ${what} within ${timeoutMs}ms (is the device connected on ${PORT}? another process holding the port?)`
          )
        ),
      timeoutMs
    );
  });
  try {
    const line = await Promise.race([lines.readLine(), timeout]);
    if (line === null) throw new Error("bridge closed the connection");
    return line;
  } finally {
    clearTimeout(timer!);
  }
}

// devlink shares its USB CDC port with the runtime's own debug prints (the
// once-a-second profiler line, app-switch logs, sketchpad stroke traces, and
// whatever gets added later): none of that is part of the devlink protocol,
// so a reader waiting for a specific reply must not treat the first line it
// receives as the answer. expectLine() reads lines until one matches the
// shape the caller is waiting for, discarding anything else as noise, all
// within a single overall deadline (noise does not get its own fresh
// timeout budget, or a steady trickle of unrelated prints could stall a
// caller indefinitely).
//
// Matching is done positively against the expected shape (not by
// blacklisting known noise prefixes like "prof"/"switch"/"stroke") so a new
// kind of debug print the firmware starts emitting later is tolerated the
// same way without this file needing to change.
export async function expectLine(
  lines: LineSource,
  shape: RegExp,
  timeoutMs: number,
  what = "reply"
): Promise<string> {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const remaining = deadline - Date.now();
    if (remaining <= 0) {
      throw new Error(
        `no ${what} within ${timeoutMs}ms (is the device connected on ${PORT}? another process holding the port?)`
      );
    }
    const line = await readLineWithTimeout(lines, remaining, what);
    if (shape.test(line)) return line;
    // Not the shape we are waiting for: noise sharing the port. Skip it and
    // keep waiting against the same overall deadline.
  }
}

// A devlink SHOT reply body line is a run of complete base64 4-char groups:
// the firmware only ever wraps a line on a group boundary (DEVLINK_B64_WRAP
// is 76, a multiple of 4), so a real payload line's length is always a
// positive multiple of 4 and every character is in the base64 alphabet,
// with '=' padding possible only in the last 1-2 characters of the very
// last line. A profiler tick or any other debug print landing between two
// payload lines will almost certainly contain a space, a pipe, or an '='
// sign outside that trailing position, and will fail this check, so it is
// safe to treat "not valid base64 shape" as "noise, skip it" rather than
// "corrupted transfer".
const BASE64_LINE_RE = /^[A-Za-z0-9+/]*={0,2}$/;

export function isBase64Line(line: string): boolean {
  return line.length > 0 && line.length % 4 === 0 && BASE64_LINE_RE.test(line);
}

// ---------------------------------------------------------------------
// SHOT decoding: base64 -> RLE bytes -> greyscale pixels
// ---------------------------------------------------------------------

export function decodeRLE(rle: Uint8Array, w: number, h: number): Uint8Array {
  const out = new Uint8Array(w * h);
  let o = 0;
  for (let i = 0; i + 1 < rle.length; i += 2) {
    const value = rle[i];
    const count = rle[i + 1];
    for (let k = 0; k < count && o < out.length; k++) out[o++] = value;
  }
  if (o !== out.length) {
    throw new Error(
      `RLE decoded ${o} pixels, expected ${w * h} (${w}x${h}); payload is corrupt or truncated`
    );
  }
  return out;
}

// ---------------------------------------------------------------------
// Minimal greyscale PNG encoder (8-bit, no palette, one IDAT).
//
// Bun.deflateSync produces raw DEFLATE (RFC 1951), not the zlib-wrapped
// stream (RFC 1950) PNG's IDAT chunk requires, so the 2-byte zlib header and
// 4-byte Adler-32 trailer are added by hand around it. Verified independently
// against Node's zlib.inflateSync during development (see
// tools/README-devlink.md).
// ---------------------------------------------------------------------

function crc32Table(): Uint32Array {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    table[n] = c >>> 0;
  }
  return table;
}
const CRC_TABLE = crc32Table();

function crc32(buf: Uint8Array): number {
  let crc = 0xffffffff;
  for (let i = 0; i < buf.length; i++) {
    crc = CRC_TABLE[(crc ^ buf[i]) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function adler32(data: Uint8Array): number {
  let a = 1;
  let b = 0;
  const MOD = 65521;
  for (let i = 0; i < data.length; i++) {
    a = (a + data[i]) % MOD;
    b = (b + a) % MOD;
  }
  return ((b << 16) | a) >>> 0;
}

function zlibWrap(rawDeflate: Uint8Array, source: Uint8Array): Uint8Array {
  const out = new Uint8Array(2 + rawDeflate.length + 4);
  out[0] = 0x78;
  out[1] = 0x9c; // default compression, 32K window: standard "78 9C" header
  out.set(rawDeflate, 2);
  new DataView(out.buffer).setUint32(2 + rawDeflate.length, adler32(source), false);
  return out;
}

function pngChunk(type: string, data: Uint8Array): Uint8Array {
  const typeBytes = new TextEncoder().encode(type);
  const body = new Uint8Array(typeBytes.length + data.length);
  body.set(typeBytes, 0);
  body.set(data, typeBytes.length);
  const out = new Uint8Array(4 + body.length + 4);
  const dv = new DataView(out.buffer);
  dv.setUint32(0, data.length, false);
  out.set(body, 4);
  dv.setUint32(4 + body.length, crc32(body), false);
  return out;
}

function concatBytes(chunks: Uint8Array[]): Uint8Array {
  const total = chunks.reduce((s, c) => s + c.length, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const c of chunks) {
    out.set(c, off);
    off += c.length;
  }
  return out;
}

export function encodeGreyPNG(pixels: Uint8Array, w: number, h: number): Uint8Array {
  if (pixels.length !== w * h) {
    throw new Error(`pixel buffer is ${pixels.length} bytes, expected ${w * h} for ${w}x${h}`);
  }
  // One filter-type byte (0 = None) per scanline, then the raw grey bytes.
  const raw = new Uint8Array((w + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w + 1)] = 0;
    raw.set(pixels.subarray(y * w, y * w + w), y * (w + 1) + 1);
  }
  const idatData = zlibWrap(Bun.deflateSync(raw), raw);

  const sig = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  const ihdr = new Uint8Array(13);
  const dv = new DataView(ihdr.buffer);
  dv.setUint32(0, w, false);
  dv.setUint32(4, h, false);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 0; // colour type: greyscale
  ihdr[10] = 0; // compression method
  ihdr[11] = 0; // filter method
  ihdr[12] = 0; // interlace: none

  return concatBytes([
    sig,
    pngChunk("IHDR", ihdr),
    pngChunk("IDAT", idatData),
    pngChunk("IEND", new Uint8Array(0)),
  ]);
}

// ---------------------------------------------------------------------
// Bridge lifecycle
// ---------------------------------------------------------------------
//
// The COM port is exclusive: exactly one process can hold it open. The
// emulator's dev server (server.ts, via devlink-host.ts) is now the single
// owner of the real port when it is running - it watches for the board and
// holds the bridge below itself, so this tool no longer opens the port
// directly whenever the server can be routed through instead. This keeps
// every command below working unchanged (they all just call openBridge()),
// keeps this tool usable stand-alone with no server running (falls back to
// opening the port directly, exactly as before this task), and gives a
// useful side effect: a command sent this way also shows up in the
// emulator page's own console, tagged as coming from the device
// (consolelog.ts / main.ts).

export interface Bridge {
  lines: LineSource;
  send(cmd: string): Promise<void>;
  close(): Promise<void>;
}

let activeBridge: Bridge | null = null;

// Opens the PowerShell bridge directly, exactly as this tool always has.
// Exported (and parameterised on port/baud rather than only reading the
// module-level env-derived constants) so the emulator server's DevlinkHost
// (emulator/devlink-host.ts) can reuse the exact same "how to open this
// board's serial port" logic - including the DTR-before-Open() ordering,
// the single most likely way a first real run silently fails - for the
// bridge IT holds, rather than a second, drift-prone copy of this script.
export async function openDirectBridge(port: string = PORT, baud: string = BAUD): Promise<Bridge> {
  const scriptPath = await writeBridgeScript();
  const proc = Bun.spawn(
    [
      "powershell",
      "-NoProfile",
      "-ExecutionPolicy",
      "Bypass",
      "-File",
      scriptPath,
      "-Port",
      port,
      "-Baud",
      baud,
    ],
    { stdin: "pipe", stdout: "pipe", stderr: "pipe" }
  );

  // stderr is captured (not "inherit") so a failed Open() - most commonly
  // another process already holding the port, see the PS1 script's own
  // catch above - can be reported to a JS caller with the OS's own message,
  // not just printed to a terminal nobody but an interactive CLI user is
  // watching (see docs/decisions/0004: DevlinkHost, the caller that most
  // needs this, has to say WHY a port could not be opened, not just that it
  // couldn't). Still relayed live to our own stderr underneath, so
  // `bun tools/dev.ts ...` run by hand looks exactly as it always has.
  let stderrText = "";
  void (async () => {
    const reader = (proc.stderr as ReadableStream<Uint8Array>).getReader();
    const decoder = new TextDecoder();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        const chunk = decoder.decode(value, { stream: true });
        stderrText += chunk;
        process.stderr.write(chunk);
      }
    } catch {
      // stream torn down along with the process; nothing left to relay
    }
  })();

  const lines = new LineReader(proc.stdout as ReadableStream<Uint8Array>);

  const bridge: Bridge = {
    lines,
    async send(cmd: string) {
      proc.stdin.write(cmd + "\n");
      await proc.stdin.flush();
    },
    // The COM port is exclusive: as long as the PowerShell bridge process is
    // still alive, it still holds the port open, and the next invocation of
    // this tool will fail to open it. kill() only requests termination; it
    // does not wait for the process (and with it, the OS handle on the
    // port) to actually go away. So close() waits for proc.exited (bounded,
    // in case the process is somehow wedged) before returning, and every
    // caller awaits close() in its finally block, so the port is genuinely
    // free again by the time this process exits.
    async close() {
      try {
        proc.stdin.end();
      } catch {
        // already closed
      }
      try {
        proc.kill();
      } catch {
        // already dead
      }
      try {
        await Promise.race([proc.exited, Bun.sleep(2000)]);
      } catch {
        // ignore: we tried to wait cleanly, but we are closing regardless
      }
      if (activeBridge === bridge) activeBridge = null;
    },
  };
  activeBridge = bridge;

  // Settle time: the SerialPort has to finish opening (and the device has
  // to notice DTR and be ready to answer) before the first command lands.
  // This also doubles as the window in which a FAILED Open() has time to
  // happen and exit the script: the PS1 script's only path to exiting this
  // early is its own catch-and-exit(1) on Open() failure, so if the process
  // is already gone by the time this sleep is over, the port was never
  // actually opened, and handing back this bridge as if it worked would be
  // exactly the lie docs/decisions/0004 is about - a status that claims
  // more than it knows.
  await Bun.sleep(300);
  if (proc.exitCode !== null) {
    if (activeBridge === bridge) activeBridge = null;
    throw new Error(
      stderrText.trim() || `devlink bridge exited immediately (code ${proc.exitCode}) while opening ${port}`
    );
  }
  return bridge;
}

// ---------------------------------------------------------------------
// Routing through the emulator server, when it is running and holding the
// port itself (see server.ts / devlink-host.ts). A WebSocket stands in for
// the PowerShell bridge's stdin/stdout pair: a "send" message writes a
// command line to the server's own bridge, and a "line" message is one raw
// line back from it (a devlink reply, or noise sharing the port - the
// server does not distinguish, same as the PowerShell bridge never has).
// ---------------------------------------------------------------------

// Read fresh on every call, not captured once at module load: lets a test
// (harness/selftest.ts) point this at a throwaway in-process server by
// setting the env var right before calling openServerBridge(), with no
// need to force a second, separately-cached module instance to see it.
function serverUrl(): string {
  return process.env.DEVLINK_SERVER_URL ?? "http://127.0.0.1:5330";
}

class WsLineSource implements LineSource {
  private queue: string[] = [];
  private waiters: ((v: string | null) => void)[] = [];
  private closed = false;

  constructor(ws: WebSocket) {
    ws.addEventListener("message", (ev) => {
      const data = (ev as unknown as { data?: unknown }).data;
      let msg: unknown;
      try {
        msg = JSON.parse(String(data));
      } catch {
        return;
      }
      const m = msg as { type?: string; text?: string; error?: string };
      if (m.type === "line" && typeof m.text === "string") {
        this.push(m.text);
      } else if (m.type === "error" && typeof m.error === "string") {
        // Surfaced as a synthetic ERR line: the same shape every command
        // below already handles as a failure reply, so routing through the
        // server needs no second failure path.
        this.push(`ERR ${m.error}`);
      }
      // "status" messages: not this tool's concern, ignored here.
    });
    ws.addEventListener("close", () => this.finish());
    ws.addEventListener("error", () => this.finish());
  }

  private push(line: string): void {
    const w = this.waiters.shift();
    if (w) w(line);
    else this.queue.push(line);
  }
  private finish(): void {
    this.closed = true;
    while (this.waiters.length) this.waiters.shift()!(null);
  }

  readLine(): Promise<string | null> {
    if (this.queue.length > 0) return Promise.resolve(this.queue.shift()!);
    if (this.closed) return Promise.resolve(null);
    return new Promise((resolve) => this.waiters.push(resolve));
  }
}

// Returns null (never throws) whenever routing through the server is not
// possible right now - server not running, unreachable, or running but
// holding no bridge of its own - so every caller's fallback is always just
// "open the port directly", exactly what this tool did before the server
// existed. Exported so harness/selftest.ts can exercise both outcomes
// directly, without ever letting a fallback cascade into a real port open.
export async function openServerBridge(): Promise<Bridge | null> {
  const url = serverUrl();
  let statusRes: Response;
  try {
    statusRes = await fetch(`${url}/api/devlink/status`, { signal: AbortSignal.timeout(800) });
  } catch {
    return null; // no server listening on this URL
  }
  if (!statusRes.ok) return null;
  let status: { connected: boolean; port: string | null };
  try {
    status = (await statusRes.json()) as typeof status;
  } catch {
    return null;
  }
  if (!status.connected) return null; // server is up but is not holding a port right now

  const wsUrl = `${url.replace(/^http/, "ws")}/api/devlink`;
  let ws: WebSocket;
  try {
    ws = new WebSocket(wsUrl);
    await new Promise<void>((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error("timed out opening the devlink socket")), 2000);
      ws.addEventListener("open", () => { clearTimeout(timer); resolve(); }, { once: true });
      ws.addEventListener("error", () => { clearTimeout(timer); reject(new Error("could not open the devlink socket")); }, { once: true });
    });
  } catch {
    return null;
  }

  const lines = new WsLineSource(ws);
  const bridge: Bridge = {
    lines,
    async send(cmd: string) {
      ws.send(JSON.stringify({ type: "send", cmd }));
    },
    // Unlike openDirectBridge's close(), this does NOT release the COM
    // port: the server keeps holding it (for the page, and for the next
    // command) regardless of whether this particular tool invocation is
    // done with it. It only disconnects this tool's own socket.
    async close() {
      try {
        ws.close();
      } catch {
        // already closed
      }
      await Promise.race([
        new Promise<void>((resolve) => ws.addEventListener("close", () => resolve(), { once: true })),
        Bun.sleep(500),
      ]);
      if (activeBridge === bridge) activeBridge = null;
    },
  };
  activeBridge = bridge;
  return bridge;
}

// The one function every command below calls. Tries the server first
// (fast to rule out: an unreachable 127.0.0.1 port fails in milliseconds
// under Bun, see AGENTS.md's loopback note), falls back to opening the
// port directly - so this tool works identically whether or not the
// emulator's dev server happens to be running.
async function openBridge(): Promise<Bridge> {
  const server = await openServerBridge();
  if (server) return server;
  return openDirectBridge();
}

process.on("SIGINT", () => {
  activeBridge?.close();
  process.exit(130);
});

// ---------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------

async function cmdPing() {
  const bridge = await openBridge();
  try {
    await bridge.send("PING");
    const reply = await expectLine(
      bridge.lines,
      /^(OK devlink \d+ \d+ \d+|ERR .*)$/,
      3000,
      "PING reply"
    );
    console.log(reply);
    if (!reply.startsWith("OK")) process.exitCode = 1;
  } finally {
    await bridge.close();
  }
}

async function cmdApp() {
  const bridge = await openBridge();
  try {
    await bridge.send("APP");
    const reply = await expectLine(bridge.lines, /^(APP -?\d+ \S+|ERR .*)$/, 3000, "APP reply");
    console.log(reply);
    if (!reply.startsWith("APP")) process.exitCode = 1;
  } finally {
    await bridge.close();
  }
}

async function cmdSwitch(args: string[]) {
  const idx = Number(args[0]);
  if (!Number.isFinite(idx)) {
    console.error("usage: bun tools/dev.ts switch <index>");
    process.exit(1);
  }
  const bridge = await openBridge();
  try {
    await bridge.send(`SWITCH ${idx}`);
    const reply = await expectLine(bridge.lines, /^(OK|ERR .*)$/, 3000, "SWITCH reply");
    console.log(reply);
    if (!reply.startsWith("OK")) process.exitCode = 1;
  } finally {
    await bridge.close();
  }
}

async function cmdShot(outPath: string | undefined) {
  if (!outPath) {
    console.error("usage: bun tools/dev.ts shot <out.png>");
    process.exit(1);
  }
  const bridge = await openBridge();
  try {
    await bridge.send("SHOT");
    const header = await expectLine(
      bridge.lines,
      /^(SHOT \d+ \d+ \d+|ERR .*)$/,
      5000,
      "SHOT header"
    );
    const m = /^SHOT (\d+) (\d+) (\d+)$/.exec(header);
    if (!m) {
      console.error(`unexpected reply to SHOT: ${header}`);
      process.exitCode = 1;
      return;
    }
    const w = Number(m[1]);
    const h = Number(m[2]);
    const rleByteCount = Number(m[3]);

    // A profiler tick (or any other debug print) can land in the middle of
    // the base64 stream, between two real payload lines. Every non-noise
    // line here is a run of complete base64 groups (see isBase64Line's
    // comment), so a line that does not have that shape is skipped rather
    // than appended, and does not corrupt the accumulated stream.
    let b64 = "";
    for (;;) {
      const line = await readLineWithTimeout(bridge.lines, 5000, "SHOT body");
      if (line === "END") break;
      if (!isBase64Line(line)) continue;
      b64 += line;
    }

    const rleBytes = new Uint8Array(Buffer.from(b64, "base64"));
    if (rleBytes.length !== rleByteCount) {
      console.error(
        `warning: base64 decoded to ${rleBytes.length} RLE bytes, header said ${rleByteCount}`
      );
    }
    const pixels = decodeRLE(rleBytes, w, h);
    const png = encodeGreyPNG(pixels, w, h);
    await Bun.write(outPath, png);
    console.log(`wrote ${outPath} (${w}x${h}, ${png.length} bytes)`);
  } finally {
    await bridge.close();
  }
}

async function cmdTap(args: string[]) {
  const [xs, ys] = args;
  const x = Number(xs);
  const y = Number(ys);
  if (!Number.isFinite(x) || !Number.isFinite(y)) {
    console.error("usage: bun tools/dev.ts tap <x> <y>");
    process.exit(1);
  }
  const bridge = await openBridge();
  try {
    await bridge.send(`TAP ${x} ${y}`);
    const reply = await expectLine(bridge.lines, /^(OK|ERR .*)$/, 3000, "TAP reply");
    console.log(reply);
    if (!reply.startsWith("OK")) process.exitCode = 1;
  } finally {
    await bridge.close();
  }
}

async function cmdErase() {
  const bridge = await openBridge();
  try {
    await bridge.send("ERASE");
    const reply = await expectLine(bridge.lines, /^(OK|ERR .*)$/, 3000, "ERASE reply");
    console.log(reply);
    if (!reply.startsWith("OK")) process.exitCode = 1;
  } finally {
    await bridge.close();
  }
}

// KEY takes a name (press/long/short), never a raw bit mask: see
// tools/README-devlink.md's KEY section for why a hex mask at a prompt is
// deliberately not offered.
const KEY_NAMES = ["press", "long", "short", "release"] as const;

async function cmdKey(args: string[]) {
  const name = (args[0] ?? "").toLowerCase();
  if (!KEY_NAMES.includes(name as (typeof KEY_NAMES)[number])) {
    console.error(`usage: bun tools/dev.ts key <${KEY_NAMES.join("|")}>`);
    process.exit(1);
  }
  const bridge = await openBridge();
  try {
    await bridge.send(`KEY ${name.toUpperCase()}`);
    const reply = await expectLine(bridge.lines, /^(OK|ERR .*)$/, 3000, "KEY reply");
    console.log(reply);
    if (!reply.startsWith("OK")) process.exitCode = 1;
  } finally {
    await bridge.close();
  }
}

const BOOT_ACTIONS = ["down", "up", "click"] as const;

async function cmdBoot(args: string[]) {
  const action = (args[0] ?? "").toLowerCase();
  if (!BOOT_ACTIONS.includes(action as (typeof BOOT_ACTIONS)[number])) {
    console.error(`usage: bun tools/dev.ts boot <${BOOT_ACTIONS.join("|")}>`);
    process.exit(1);
  }
  const bridge = await openBridge();
  try {
    await bridge.send(`BOOT ${action.toUpperCase()}`);
    const reply = await expectLine(bridge.lines, /^(OK|ERR .*)$/, 3000, "BOOT reply");
    console.log(reply);
    if (!reply.startsWith("OK")) process.exitCode = 1;
  } finally {
    await bridge.close();
  }
}

// CHORD is the BOOT+PWR app-menu gesture, composed on the firmware side from
// the same BOOT/KEY primitives above (see devlink.c's CHORD case): hold
// BOOT, deliver PWR's long-press verdict, release BOOT one tick later. This
// tests the runtime and the menu app, never the PMIC register read or the
// flash-chip-select borrow a real press goes through first; see
// tools/README-devlink.md's "What injection cannot test" section.
async function cmdChord() {
  const bridge = await openBridge();
  try {
    await bridge.send("CHORD");
    const reply = await expectLine(bridge.lines, /^(OK|ERR .*)$/, 3000, "CHORD reply");
    console.log(reply);
    if (!reply.startsWith("OK")) process.exitCode = 1;
  } finally {
    await bridge.close();
  }
}

async function cmdDraw(args: string[]) {
  if (args.length === 0) {
    console.error("usage: bun tools/dev.ts draw x1,y1 x2,y2 ...");
    process.exit(1);
  }
  const points = args.map((a) => {
    const m = /^(-?\d+),(-?\d+)$/.exec(a);
    if (!m) throw new Error(`bad point "${a}", expected x,y`);
    return { x: Number(m[1]), y: Number(m[2]) };
  });

  const bridge = await openBridge();
  try {
    await bridge.send(`DOWN ${points[0].x} ${points[0].y}`);
    console.log(await expectLine(bridge.lines, /^(OK|ERR .*)$/, 3000, "DOWN reply"));

    for (let i = 1; i < points.length; i++) {
      await Bun.sleep(20); // paced like a real finger, not a single burst
      await bridge.send(`MOVE ${points[i].x} ${points[i].y}`);
      console.log(await expectLine(bridge.lines, /^(OK|ERR .*)$/, 3000, "MOVE reply"));
    }

    await bridge.send("UP");
    console.log(await expectLine(bridge.lines, /^(OK|ERR .*)$/, 3000, "UP reply"));
  } finally {
    await bridge.close();
  }
}

// TUNE: live-adjusts sketch.c's dropout-tolerance constants on a running
// device with no reflash (firmware/devlink.c's TUNE command, gated behind
// the SKETCH_LIVE_TUNE build flag - firmware/CMakeLists.txt). Five shapes:
//
//   tune                     list every declared tunable: name, current,
//                            min, max, default. A row whose current value
//                            differs from its default is marked, so "which
//                            of these am I still overriding" reads at a
//                            glance rather than by comparing two columns.
//   tune get <name>          current value of one tunable
//   tune set <name> <value>  applies value (clamped to [min, max] on the
//                            device), prints what was actually applied
//   tune reset <name>        restores one tunable to its declared default,
//                            prints what it is now AND what it was a
//                            moment ago
//   tune reset               restores every declared tunable to its
//                            default, same before/after reporting per line
//   tune freeze              prints every CURRENT value as a
//                            `#define ..._DEFAULT` line, ready to paste
//                            over sketch.c's own constants once a value is
//                            settled
//
// A device built without SKETCH_LIVE_TUNE answers "ERR no tunables" to
// every one of these - that is not a bug in this tool, it means the running
// firmware does not have the knob compiled in.
const TUNE_LIST_RE = /^TUNE (\S+) (\S+) (\S+) (\S+) (\S+)$/;
const TUNE_VALUE_RE = /^TUNE (\S+) (\S+)$/;
// RESET's reply carries one more field than GET/SET's (name, applied) -
// applied AND previous, per the owner's ask ("a reset that prints the value
// it restored, and ideally what it was before, is worth more than a silent
// one") - so it gets its own shape rather than overloading TUNE_VALUE_RE.
const TUNE_RESET_RE = /^TUNE (\S+) (\S+) (\S+)$/;
const TUNE_ERR_RE = /^ERR .*$/;
const TUNE_FREEZE_LINE_RE = /^#define \S+_DEFAULT \S+f$/;

export async function readUntilEnd(bridge: Bridge, lineOk: (line: string) => boolean, what: string): Promise<string[]> {
  const out: string[] = [];
  for (;;) {
    const line = await readLineWithTimeout(bridge.lines, 3000, what);
    if (line === "END") break;
    if (TUNE_ERR_RE.test(line)) {
      console.log(line);
      return [];
    }
    if (!lineOk(line)) continue; // noise sharing the port, same as expectLine()
    out.push(line);
  }
  return out;
}

async function cmdTuneList(bridge: Bridge) {
  await bridge.send("TUNE");
  const lines = await readUntilEnd(bridge, (l) => TUNE_LIST_RE.test(l), "TUNE list line");
  if (lines.length === 0) {
    console.log("(no tunables declared - device not built with SKETCH_LIVE_TUNE?)");
    return;
  }
  console.log("   name        value      min        max        default");
  for (const line of lines) {
    const m = TUNE_LIST_RE.exec(line)!;
    const [, name, value, min, max, def] = m;
    // Marked rather than left to a value/default column comparison: after
    // ten minutes of turning knobs by feel, "which of these am I still
    // overriding" is the question that actually gets asked, per the
    // owner's own framing of why this matters.
    const overridden = Number(value) !== Number(def);
    const marker = overridden ? " * " : "   ";
    console.log(`${marker}${name.padEnd(11)} ${value.padEnd(10)} ${min.padEnd(10)} ${max.padEnd(10)} ${def}`);
  }
  if (lines.some((l) => Number(TUNE_LIST_RE.exec(l)![2]) !== Number(TUNE_LIST_RE.exec(l)![5]))) {
    console.log("\n* overridden from its default - \"tune reset <name>\" or \"tune reset\" to clear");
  }
}

async function cmdTuneGet(bridge: Bridge, name: string) {
  await bridge.send(`TUNE GET ${name}`);
  const reply = await expectLine(bridge.lines, /^(TUNE \S+ \S+|ERR .*)$/, 3000, "TUNE GET reply");
  console.log(reply);
  if (!reply.startsWith("TUNE")) process.exitCode = 1;
}

async function cmdTuneSet(bridge: Bridge, name: string, value: string) {
  if (!Number.isFinite(Number(value))) {
    console.error(`usage: bun tools/dev.ts tune set <name> <value>`);
    process.exit(1);
  }
  await bridge.send(`TUNE SET ${name} ${value}`);
  const reply = await expectLine(bridge.lines, /^(TUNE \S+ \S+|ERR .*)$/, 3000, "TUNE SET reply");
  console.log(reply);
  if (!reply.startsWith("TUNE")) process.exitCode = 1;
}

function logTuneReset(line: string) {
  const m = TUNE_RESET_RE.exec(line);
  if (!m) { console.log(line); return; }
  const [, name, applied, previous] = m;
  if (applied === previous) {
    console.log(`${name}: already at default (${applied})`);
  } else {
    console.log(`${name}: ${previous} -> ${applied} (default)`);
  }
}

async function cmdTuneResetOne(bridge: Bridge, name: string) {
  await bridge.send(`TUNE RESET ${name}`);
  const reply = await expectLine(bridge.lines, /^(TUNE \S+ \S+ \S+|ERR .*)$/, 3000, "TUNE RESET reply");
  if (reply.startsWith("TUNE")) {
    logTuneReset(reply);
  } else {
    console.log(reply);
    process.exitCode = 1;
  }
}

async function cmdTuneResetAll(bridge: Bridge) {
  await bridge.send("TUNE RESET");
  const lines = await readUntilEnd(bridge, (l) => TUNE_RESET_RE.test(l), "TUNE RESET line");
  if (lines.length === 0) {
    console.log("(no tunables declared - device not built with SKETCH_LIVE_TUNE?)");
    return;
  }
  for (const line of lines) logTuneReset(line);
}

async function cmdTuneFreeze(bridge: Bridge) {
  await bridge.send("TUNE FREEZE");
  const lines = await readUntilEnd(bridge, (l) => TUNE_FREEZE_LINE_RE.test(l), "TUNE FREEZE line");
  if (lines.length === 0) {
    console.log("(no tunables declared - device not built with SKETCH_LIVE_TUNE?)");
    return;
  }
  for (const line of lines) console.log(line);
}

async function cmdTune(args: string[]) {
  const [sub, ...rest] = args;
  const bridge = await openBridge();
  try {
    if (!sub) {
      await cmdTuneList(bridge);
    } else if (sub === "get") {
      if (!rest[0]) {
        console.error("usage: bun tools/dev.ts tune get <name>");
        process.exit(1);
      }
      await cmdTuneGet(bridge, rest[0]);
    } else if (sub === "set") {
      if (!rest[0] || !rest[1]) {
        console.error("usage: bun tools/dev.ts tune set <name> <value>");
        process.exit(1);
      }
      await cmdTuneSet(bridge, rest[0], rest[1]);
    } else if (sub === "reset") {
      if (rest[0]) {
        await cmdTuneResetOne(bridge, rest[0]);
      } else {
        await cmdTuneResetAll(bridge);
      }
    } else if (sub === "freeze") {
      await cmdTuneFreeze(bridge);
    } else {
      console.error("usage: bun tools/dev.ts tune [get <name> | set <name> <value> | reset [name] | freeze]");
      process.exit(1);
    }
  } finally {
    await bridge.close();
  }
}

async function cmdLog(secArg: string | undefined) {
  const seconds = secArg ? Number(secArg) : 10;
  const bridge = await openBridge();
  console.error(`# streaming device output for ${seconds}s (Ctrl+C to stop early)`);
  const deadline = Date.now() + seconds * 1000;
  try {
    while (Date.now() < deadline) {
      const remaining = deadline - Date.now();
      if (remaining <= 0) break;
      let line: string;
      try {
        line = await readLineWithTimeout(bridge.lines, remaining, "device output");
      } catch {
        break;
      }
      console.log(line);
    }
  } finally {
    await bridge.close();
  }
}

function printUsage() {
  console.error(
    [
      "usage: bun tools/dev.ts <command> [args]",
      "",
      "  ping                          check the device is alive",
      "  shot <out.png>                grab a screenshot",
      "  app                           report the running app",
      "  switch <index>                switch to g_apps[index]",
      "  tap <x> <y>                   tap once",
      "  draw x1,y1 x2,y2 ...          down, move through each point, up",
      "  erase                         trigger the wipe animation",
      "  key <press|long|short|release> inject a PMIC key event",
      "  boot <down|up|click>          inject the BOOT button's level or click",
      "  chord                         inject the BOOT+PWR app-menu gesture",
      "  tune                          list live-tunable constants (name, current, min, max, default;",
      "                                overridden values are marked)",
      "  tune get <name>               read one tunable's current value",
      "  tune set <name> <value>       apply a value now, no reflash (clamped on the device)",
      "  tune reset <name>             restore one tunable to its default, prints before -> after",
      "  tune reset                    restore every tunable to its default",
      "  tune freeze                   print current values as #define ..._DEFAULT lines",
      "  log [seconds]                 stream device output (default 10s)",
      "",
      "key/boot/chord test the runtime and the apps, not the PMIC or the BOOT",
      "pad read themselves: see tools/README-devlink.md, \"What injection",
      "cannot test\".",
      "",
      "tune requires a device built with -DSKETCH_LIVE_TUNE=1 (off by default -",
      "see firmware/CMakeLists.txt); a normal build answers every tune",
      "subcommand with \"ERR no tunables\".",
      "",
      "Port/baud: DEVLINK_PORT (default COM4), DEVLINK_BAUD (default 115200).",
    ].join("\n")
  );
}

async function main() {
  const [cmd, ...rest] = Bun.argv.slice(2);
  switch (cmd) {
    case "ping":
      await cmdPing();
      break;
    case "shot":
      await cmdShot(rest[0]);
      break;
    case "app":
      await cmdApp();
      break;
    case "switch":
      await cmdSwitch(rest);
      break;
    case "tap":
      await cmdTap(rest);
      break;
    case "draw":
      await cmdDraw(rest);
      break;
    case "erase":
      await cmdErase();
      break;
    case "key":
      await cmdKey(rest);
      break;
    case "boot":
      await cmdBoot(rest);
      break;
    case "chord":
      await cmdChord();
      break;
    case "tune":
      await cmdTune(rest);
      break;
    case "log":
      await cmdLog(rest[0]);
      break;
    default:
      printUsage();
      process.exit(cmd ? 1 : 0);
  }
}

if (import.meta.main) {
  main();
}
