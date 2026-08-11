#!/usr/bin/env bun
// devlink host CLI: `bun tools/dev.ts <command> ...`
//
// Talks to the RP2350's devlink firmware module (firmware/devlink.c) over
// its USB CDC serial port. See tools/README-devlink.md for the wire
// protocol and for the important caveat: the serial bridge below has not
// been run against real hardware (the port was in use by another process
// while this was written). Read that file before trusting this blind.
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

export class LineReader {
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
  lines: LineReader,
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

interface Bridge {
  lines: LineReader;
  send(cmd: string): Promise<void>;
  close(): void;
}

let activeBridge: Bridge | null = null;

async function openBridge(): Promise<Bridge> {
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
      PORT,
      "-Baud",
      BAUD,
    ],
    { stdin: "pipe", stdout: "pipe", stderr: "inherit" }
  );

  const lines = new LineReader(proc.stdout as ReadableStream<Uint8Array>);

  const bridge: Bridge = {
    lines,
    async send(cmd: string) {
      proc.stdin.write(cmd + "\n");
      await proc.stdin.flush();
    },
    close() {
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
      if (activeBridge === bridge) activeBridge = null;
    },
  };
  activeBridge = bridge;

  // Settle time: the SerialPort has to finish opening (and the device has
  // to notice DTR and be ready to answer) before the first command lands.
  // Cheap insurance against racing PING against a port that is still
  // mid-open.
  await Bun.sleep(300);
  return bridge;
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
    const reply = await readLineWithTimeout(bridge.lines, 3000, "PING reply");
    console.log(reply);
    if (!reply.startsWith("OK")) process.exitCode = 1;
  } finally {
    bridge.close();
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
    const header = await readLineWithTimeout(bridge.lines, 5000, "SHOT header");
    const m = /^SHOT (\d+) (\d+) (\d+)$/.exec(header);
    if (!m) {
      console.error(`unexpected reply to SHOT: ${header}`);
      process.exitCode = 1;
      return;
    }
    const w = Number(m[1]);
    const h = Number(m[2]);
    const rleByteCount = Number(m[3]);

    let b64 = "";
    for (;;) {
      const line = await readLineWithTimeout(bridge.lines, 5000, "SHOT body");
      if (line === "END") break;
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
    bridge.close();
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
    const reply = await readLineWithTimeout(bridge.lines, 3000, "TAP reply");
    console.log(reply);
    if (!reply.startsWith("OK")) process.exitCode = 1;
  } finally {
    bridge.close();
  }
}

async function cmdErase() {
  const bridge = await openBridge();
  try {
    await bridge.send("ERASE");
    const reply = await readLineWithTimeout(bridge.lines, 3000, "ERASE reply");
    console.log(reply);
    if (!reply.startsWith("OK")) process.exitCode = 1;
  } finally {
    bridge.close();
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
    console.log(await readLineWithTimeout(bridge.lines, 3000, "DOWN reply"));

    for (let i = 1; i < points.length; i++) {
      await Bun.sleep(20); // paced like a real finger, not a single burst
      await bridge.send(`MOVE ${points[i].x} ${points[i].y}`);
      console.log(await readLineWithTimeout(bridge.lines, 3000, "MOVE reply"));
    }

    await bridge.send("UP");
    console.log(await readLineWithTimeout(bridge.lines, 3000, "UP reply"));
  } finally {
    bridge.close();
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
    bridge.close();
  }
}

function printUsage() {
  console.error(
    [
      "usage: bun tools/dev.ts <command> [args]",
      "",
      "  ping                          check the device is alive",
      "  shot <out.png>                grab a screenshot",
      "  tap <x> <y>                   tap once",
      "  draw x1,y1 x2,y2 ...          down, move through each point, up",
      "  erase                         trigger the wipe animation",
      "  log [seconds]                 stream device output (default 10s)",
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
    case "tap":
      await cmdTap(rest);
      break;
    case "draw":
      await cmdDraw(rest);
      break;
    case "erase":
      await cmdErase();
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
