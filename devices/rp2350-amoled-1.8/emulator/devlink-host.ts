// Server-side owner of the real devlink serial link. Not part of the
// browser bundle (see src/ for that half): this spawns PowerShell and
// touches the filesystem, neither of which exists in a browser, so it
// lives here at the emulator's own root next to server.ts and build.ts,
// the existing convention for "this script runs under Bun, not in the
// page".
//
// Three jobs, matching the task's own framing:
//
//   1. Detection. A persistent PowerShell process (spawned once, not
//      re-spawned per poll - see AGENTS.md's spawn-cost notes) polls WMI
//      every 500ms for a USB device whose PNPDeviceID carries VID 2E8A,
//      and prints a status line only when what it finds actually changes.
//      That is the "notices... to be able to make measurements" half of
//      the owner's ask, and the 500ms figure is this file's answer to "how
//      fast": a plug/unplug is visible on the page within roughly half a
//      second to a second (one poll interval, plus IPC and a WS round
//      trip), with no page reload.
//
//   2. Single ownership. The moment the watcher reports the board
//      present, THIS host - and only this host - opens the real bridge
//      (tools/dev.ts's openDirectBridge(), reused rather than
//      reimplemented: see that file's own comment on why). tools/dev.ts,
//      run as a script, routes its own commands through this host over
//      HTTP/WS instead of opening the port itself whenever this server is
//      running (see dev.ts's openBridge()). Nothing outside this class
//      ever touches the bridge's `lines`/`send` directly.
//
//   3. The board's own console output. Every raw line read off the bridge
//      (reply or shared-port noise alike, exactly as tools/README-devlink.md
//      describes) is handed to onLine() listeners, which server.ts relays
//      to every connected page over /api/devlink.
//
// A fourth mode, "fake", swaps both the watcher and the bridge for the
// in-process loopback fixture (harness/fixtures/loopbackLink.ts) with zero
// PowerShell and zero real hardware involved - see that file's own header
// for why it exists. "off" touches nothing at all. Bun run server.ts picks
// among these from DEVLINK_MODE (see server.ts); every test in this task
// passes "off" or "fake" explicitly, on purpose, never the default "real"
// (the owner is using the board right now).
import os from "node:os";
import path from "node:path";
import { LineReader, openDirectBridge, type Bridge } from "../tools/dev";
import { createLoopbackLink, SKETCH_TUNABLES, MISMATCHED_TUNABLES } from "./harness/fixtures/loopbackLink";

export interface DevlinkStatus {
  connected: boolean;
  port: string | null;
}

export type DevlinkMode = "real" | "fake" | "off";

export interface DevlinkHostOptions {
  mode: DevlinkMode;
  fakeTunables?: "default" | "mismatch";
}

const WATCH_VID = "2E8A";
const WATCH_POLL_MS = 500;

// Prints a compact JSON status line only when the answer actually changes -
// keeps the pipe quiet (and DevlinkHost's own log spam-free) the vast
// majority of the time a board just sits there plugged in or unplugged.
const WATCH_PS1 = `
$ErrorActionPreference = "Stop"

function Get-BoardStatus {
    try {
        $dev = Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction Stop |
            Where-Object { $_.PNPDeviceID -like "*VID_${WATCH_VID}*" -and $_.Name -match '\\(COM(\\d+)\\)' } |
            Select-Object -First 1
    } catch {
        $dev = $null
    }
    if ($dev) {
        $m = [regex]::Match($dev.Name, '\\(COM(\\d+)\\)')
        return @{ connected = $true; port = "COM$($m.Groups[1].Value)" }
    }
    return @{ connected = $false; port = $null }
}

$last = $null
while ($true) {
    $status = Get-BoardStatus
    $key = "$($status.connected):$($status.port)"
    if ($key -ne $last) {
        Write-Output ($status | ConvertTo-Json -Compress)
        $last = $key
    }
    Start-Sleep -Milliseconds ${WATCH_POLL_MS}
}
`;

async function writeWatchScript(): Promise<string> {
  const p = path.join(os.tmpdir(), "rp2350-devlink-watch.ps1");
  await Bun.write(p, WATCH_PS1);
  return p;
}

export class DevlinkHost {
  status: DevlinkStatus = { connected: false, port: null };

  private opts: DevlinkHostOptions;
  private statusListeners = new Set<(s: DevlinkStatus) => void>();
  private lineListeners = new Set<(text: string) => void>();
  private bridge: Bridge | null = null;
  private watcherProc: ReturnType<typeof Bun.spawn> | null = null;
  private watching = false;
  private opening = false; // guards overlapping open attempts on rapid watcher flaps

  constructor(opts: DevlinkHostOptions) {
    this.opts = opts;
  }

  onStatus(cb: (s: DevlinkStatus) => void): void {
    this.statusListeners.add(cb);
  }
  onLine(cb: (text: string) => void): void {
    this.lineListeners.add(cb);
  }

  // Writes one command line to the held bridge, if there is one. Returns
  // false (and writes nothing) when there is no board to send it to -
  // server.ts turns that into an "error" message back to whichever client
  // asked.
  sendRaw(cmd: string): boolean {
    if (!this.bridge) return false;
    void this.bridge.send(cmd);
    return true;
  }

  async start(): Promise<void> {
    if (this.opts.mode === "off") return;
    if (this.opts.mode === "fake") {
      this.attachFake();
      return;
    }
    this.startRealWatcher();
  }

  async stop(): Promise<void> {
    this.watching = false;
    try {
      this.watcherProc?.kill();
    } catch {
      // already dead
    }
    await this.closeBridge();
  }

  private setStatus(next: DevlinkStatus): void {
    this.status = next;
    for (const cb of this.statusListeners) cb(next);
  }

  private emitLine(text: string): void {
    for (const cb of this.lineListeners) cb(text);
  }

  private async closeBridge(): Promise<void> {
    const b = this.bridge;
    this.bridge = null;
    if (b) {
      try {
        await b.close();
      } catch {
        // closing regardless
      }
    }
  }

  // ---- fake mode: the in-process loopback fixture, no PowerShell, no port
  private attachFake(): void {
    const tunables = this.opts.fakeTunables === "mismatch" ? MISMATCHED_TUNABLES : SKETCH_TUNABLES;
    const link = createLoopbackLink({ tunables });
    const bridge: Bridge = {
      lines: link,
      send: (cmd) => link.send(cmd),
      close: () => link.close(),
    };
    this.bridge = bridge;
    this.pumpBridgeLines(bridge);
    this.setStatus({ connected: true, port: "FAKE0" });
  }

  // ---- real mode: spawn the PowerShell watcher, open the real bridge on
  // detection, close it again the moment the board disappears.
  private startRealWatcher(): void {
    this.watching = true;
    void (async () => {
      const scriptPath = await writeWatchScript();
      const proc = Bun.spawn(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", scriptPath], {
        stdout: "pipe",
        stderr: "inherit",
      });
      this.watcherProc = proc;
      const lines = new LineReader(proc.stdout as ReadableStream<Uint8Array>);
      await this.pumpWatcher(lines);
    })();
  }

  private async pumpWatcher(lines: LineReader): Promise<void> {
    while (this.watching) {
      let line: string | null;
      try {
        line = await lines.readLine();
      } catch {
        break;
      }
      if (line === null) break; // watcher process ended
      let parsed: { connected?: boolean; port?: string | null };
      try {
        parsed = JSON.parse(line) as typeof parsed;
      } catch {
        continue; // not a status line (should not happen; skip rather than crash the watcher loop)
      }
      await this.onWatcherStatus({ connected: !!parsed.connected, port: parsed.port ?? null });
    }
  }

  private async onWatcherStatus(next: DevlinkStatus): Promise<void> {
    if (this.opening) return; // a transition is already in flight; the watcher keeps polling and will catch up
    if (next.connected && next.port) {
      if (this.bridge && this.status.port === next.port) return; // already holding it
      this.opening = true;
      try {
        await this.closeBridge(); // in case a stale one is still held for a different port
        const bridge = await openDirectBridge(next.port);
        this.bridge = bridge;
        this.pumpBridgeLines(bridge);
        this.setStatus({ connected: true, port: next.port });
      } catch (err) {
        // Could not open it yet (still settling right after enumeration,
        // or briefly held by something else): report disconnected rather
        // than a half-open state. The watcher keeps polling regardless, so
        // the NEXT status line (even an unchanged one is not re-sent, but
        // a fresh attempt happens whenever this host itself decides to
        // retry) gets another chance - see the retry loop below.
        console.warn(`devlink: board seen on ${next.port} but could not open it yet: ${err instanceof Error ? err.message : String(err)}`);
        this.setStatus({ connected: false, port: null });
        this.scheduleRetry(next.port);
      } finally {
        this.opening = false;
      }
    } else {
      await this.closeBridge();
      this.setStatus({ connected: false, port: null });
    }
  }

  // The watcher only prints a line when its OWN answer changes, so a
  // failed open (the board is present the whole time, from the watcher's
  // point of view) would otherwise never get a second attempt. This is
  // deliberately separate from the watcher's own polling, on a slightly
  // longer interval, so a port that is briefly busy (another process
  // mid-close) gets retried without hammering it.
  private scheduleRetry(port: string): void {
    setTimeout(() => {
      if (this.status.connected || !this.watching) return;
      void this.onWatcherStatus({ connected: true, port });
    }, 1500);
  }

  private pumpBridgeLines(bridge: Bridge): void {
    void (async () => {
      for (;;) {
        let line: string | null;
        try {
          line = await bridge.lines.readLine();
        } catch {
          break;
        }
        if (line === null) break; // bridge closed - device unplugged, or we closed it ourselves
        this.emitLine(line);
      }
    })();
  }
}
