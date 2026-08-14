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
import { createLoopbackLink, SKETCH_TUNABLES, MISMATCHED_TUNABLES, type LoopbackLinkHandle } from "./harness/fixtures/loopbackLink";

// See docs/decisions/0004-the-day-the-instruments-lied.md: an instrument
// that reports health while broken is worse than none. "connected" used to
// mean only "a USB device with VID 2E8A is enumerated" - detection, not
// capability - so two servers racing for the same port could both say
// connected:true while only one could actually talk to the board. This
// type makes the three real situations distinguishable instead:
//   - "absent"      no board on the bus at all.
//   - "unavailable" a board IS present (port is known) but the bridge is
//                   not open and usable right now - and why, see `reason`.
//   - "connected"   open and usable: a command sent now will reach it.
export type DevlinkState = "absent" | "unavailable" | "connected";

export interface DevlinkStatus {
  state: DevlinkState;
  // Mirrors `state === "connected"`, kept as its own field because every
  // existing consumer (tools/dev.ts's openServerBridge, the page's status
  // chip) only ever needed a boolean and predates this richer type.
  connected: boolean;
  // The port a board was last seen on. Set for "unavailable" as well as
  // "connected" (a board IS there, just not usable yet); null only when
  // state is "absent".
  port: string | null;
  // Populated only for "unavailable": why the bridge is not usable right
  // now, in the OS's own words where one is available (openDirectBridge's
  // captured stderr) - so "another process is holding it" and "no board
  // plugged in" are never reported as the same thing.
  reason: string | null;
}

const ABSENT_STATUS: DevlinkStatus = { state: "absent", connected: false, port: null, reason: null };

export type DevlinkMode = "real" | "fake" | "off";

export interface DevlinkHostOptions {
  mode: DevlinkMode;
  fakeTunables?: "default" | "mismatch";
  // Test-only (harness/selftest.ts, scripts/verify-ui-feedback.ts via
  // DEVLINK_FAKE_OPEN_FAILURES): the fake bridge's first N open attempts
  // throw a synthetic error instead of succeeding, standing in for a real
  // "access denied" from another process already holding the port - so the
  // retry/recovery logic below can be proven with no real port and no
  // second server. Ignored outside mode: "fake". Default 0 (fake mode's
  // historical behaviour: connects immediately, first try).
  fakeOpenFailures?: number;
}

const WATCH_VID = "2E8A";
const WATCH_POLL_MS = 500;
// How long a failed (or lost) bridge waits before trying again. Separate
// from the watcher's own 500ms poll on purpose (see scheduleRetry's own
// comment): this is retrying OUR open attempt, not re-detecting the board.
const RETRY_MS = 1500;

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
  status: DevlinkStatus = ABSENT_STATUS;

  private opts: DevlinkHostOptions;
  private statusListeners = new Set<(s: DevlinkStatus) => void>();
  private lineListeners = new Set<(text: string) => void>();
  private bridge: Bridge | null = null;
  private watcherProc: ReturnType<typeof Bun.spawn> | null = null;
  private watching = false;
  private opening = false; // guards overlapping open attempts on rapid watcher flaps
  // fake mode only: the loopback link backing the currently held bridge, so
  // debugDropCurrentBridge() (test-only, see its own comment) has something
  // to end out from under pumpBridgeLines without going through the normal
  // intentional-close path.
  private fakeLink: LoopbackLinkHandle | null = null;
  private fakeOpenFailuresRemaining: number;

  constructor(opts: DevlinkHostOptions) {
    this.opts = opts;
    this.fakeOpenFailuresRemaining = opts.fakeOpenFailures ?? 0;
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
      // Fake mode runs through the exact same detect/open/retry/loss-
      // detection state machine real mode does, just fed by a synthetic
      // "board present" event instead of a PowerShell watcher line, and
      // openPort() below routing to the loopback fixture instead of a real
      // serial port. That is deliberate: it is what lets harness/selftest.ts
      // prove the retry and loss-detection logic itself (DEVLINK_MODE=fake,
      // never real hardware) rather than only ever exercising the "happy
      // path, connects instantly" case this used to be limited to.
      this.watching = true;
      void this.onWatcherStatus({ connected: true, port: "FAKE0" });
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

  // Test-only (harness/selftest.ts). Fake mode only. Ends the CURRENTLY
  // HELD fake bridge's underlying link out from under DevlinkHost, WITHOUT
  // going through closeBridge() first - simulating the port disappearing
  // (unplug, or the board resetting mid-flash, the owner's own normal case
  // per docs/decisions/0004) so it is pumpBridgeLines's own loss detection
  // that has to notice, exactly as it would have to for a real board, not a
  // caller that already knows. Returns false (a no-op) outside fake mode or
  // with no bridge currently held, so it can never be mistaken for a way to
  // simulate this against a real port.
  debugDropCurrentBridge(): boolean {
    if (this.opts.mode !== "fake" || !this.fakeLink) return false;
    const link = this.fakeLink;
    this.fakeLink = null;
    void link.close();
    return true;
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

  // The one place that knows HOW to open a port, for whichever mode is
  // running - real mode reuses tools/dev.ts's openDirectBridge() (see this
  // file's header for why: one DTR-before-Open() implementation, not a
  // second drift-prone copy), fake mode hands back the in-process loopback
  // fixture, optionally throwing a scripted failure first (fakeOpenFailures)
  // so the retry/recovery path below can be proven with no PowerShell and
  // no real port. Both throw on failure rather than ever returning a bridge
  // that is not actually usable - see docs/decisions/0004.
  private async openPort(port: string): Promise<Bridge> {
    if (this.opts.mode === "fake") return this.openFakePort(port);
    return openDirectBridge(port);
  }

  private async openFakePort(port: string): Promise<Bridge> {
    if (this.fakeOpenFailuresRemaining > 0) {
      this.fakeOpenFailuresRemaining--;
      throw new Error(
        `fake devlink: simulated open failure on ${port} (scripted by fakeOpenFailures - standing in for a real "access denied" from another process already holding the port)`
      );
    }
    const tunables = this.opts.fakeTunables === "mismatch" ? MISMATCHED_TUNABLES : SKETCH_TUNABLES;
    const link = createLoopbackLink({ tunables });
    this.fakeLink = link;
    return {
      lines: link,
      send: (cmd) => link.send(cmd),
      close: async () => {
        if (this.fakeLink === link) this.fakeLink = null;
        await link.close();
      },
    };
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

  // Detection saying a board is present is necessary but not sufficient for
  // "connected" - see this file's DevlinkStatus comment. This is the one
  // place that turns "board present" into either a genuinely open, usable
  // bridge (state: "connected") or an honest "unavailable" plus why, and
  // schedules the retry that makes a transient failure recover on its own.
  private async onWatcherStatus(next: { connected: boolean; port: string | null }): Promise<void> {
    if (this.opening) return; // a transition is already in flight; the watcher keeps polling and will catch up
    if (next.connected && next.port) {
      if (this.status.state === "connected" && this.status.port === next.port) return; // already holding it, and it works
      this.opening = true;
      try {
        await this.closeBridge(); // in case a stale one is still held for a different port
        const bridge = await this.openPort(next.port);
        this.bridge = bridge;
        this.pumpBridgeLines(bridge);
        this.setStatus({ state: "connected", connected: true, port: next.port, reason: null });
      } catch (err) {
        // Could not open it (still settling right after enumeration, or
        // held by something else - openDirectBridge's captured stderr says
        // which, verbatim, in `reason`): report exactly that, a board
        // present but not yet usable, rather than either a silent
        // half-open state or "no device" (a completely different problem
        // for whoever is looking at the page). scheduleRetry keeps this
        // from being permanent - see its own comment.
        const reason = err instanceof Error ? err.message : String(err);
        console.warn(`devlink: board seen on ${next.port} but could not open it: ${reason}`);
        this.setStatus({ state: "unavailable", connected: false, port: next.port, reason });
        this.scheduleRetry(next.port);
      } finally {
        this.opening = false;
      }
    } else {
      await this.closeBridge();
      this.setStatus(ABSENT_STATUS);
    }
  }

  // The watcher only prints a line when its OWN answer changes, so a
  // failed open (the board is present the whole time, from the watcher's
  // point of view) would otherwise never get a second attempt - this is
  // what makes a transient conflict transient instead of permanent (see
  // docs/decisions/0004: this was the actual owner-facing bug, not the
  // status text). Deliberately separate from the watcher's own polling, on
  // a slightly longer interval, so a port that is briefly busy (another
  // process mid-close) gets retried without hammering it. Guarded on
  // `this.status.port` rather than just "not connected yet", so a retry
  // scheduled for a port that is no longer the one anything currently cares
  // about (the watcher moved on, or a fresher loss/recovery already fired)
  // quietly does nothing instead of reopening something stale.
  private scheduleRetry(port: string): void {
    setTimeout(() => {
      if (!this.watching) return;
      if (this.status.state === "connected") return; // recovered another way already
      if (this.status.port !== port) return;
      void this.onWatcherStatus({ connected: true, port });
    }, RETRY_MS);
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
      // EOF (or a read error): if this bridge is STILL the one DevlinkHost
      // thinks it holds, nobody asked for it to close. closeBridge() always
      // nulls `this.bridge` out BEFORE awaiting bridge.close(), so an
      // intentional close (quit, watcher says gone, a fresher open
      // replacing this one) never reaches here with `this.bridge ===
      // bridge` still true - only an UNEXPECTED death does: the port went
      // away under us, unplugged or the board resetting mid-flash, which
      // the owner reflashes constantly, so this is the normal case, not an
      // edge case (docs/decisions/0004). Say so, honestly, and let the same
      // retry loop a failed open uses pick it back up - no restart needed.
      if (this.bridge === bridge) {
        this.bridge = null;
        const port = this.status.port;
        console.warn(`devlink: lost the bridge on ${port ?? "?"} (port closed unexpectedly)`);
        this.setStatus({
          state: "unavailable",
          connected: false,
          port,
          reason: "the port closed unexpectedly (unplugged, or the board reset)",
        });
        if (port) this.scheduleRetry(port);
      }
    })();
  }
}
