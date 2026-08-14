// Browser-side half of the device link. Connects to the emulator server's
// /api/devlink socket (see server.ts and devlink-host.ts, the server-side
// owner of the real serial port), which relays:
//   - a "status" message the instant a board appears or disappears
//     (server.ts's DevlinkHost, itself fed by a PowerShell watcher polling
//     for USB VID 2E8A - the "notice a plug/unplug with no reload" half of
//     this task);
//   - a "line" message for every raw line the board's serial port produces
//     (both devlink replies and the runtime's own shared-port debug
//     prints - see tools/README-devlink.md), which main.ts forwards into
//     the same on-page console the emulated firmware's own printf uses,
//     tagged "device" (consolelog.ts) so the two are never mistaken for
//     each other.
//
// This class also lets the page ITSELF speak devlink (the tuning panel's
// "read the board's own TUNE list" - tunables.ts), through request(): send
// one command, collect lines by shape until `matcher` says "done", same
// noise-tolerant discipline tools/dev.ts's expectLine()/readUntilEnd()
// already use against the real board's shared port (devlinkProtocol.ts's
// matchers). Only one request is ever "active" at a time (FIFO queue): the
// tuning panel does not need to interleave two in-flight TUNE calls, and
// serialising means a line that does not match the head request's matcher
// is unambiguously noise, not a race between two pending readers.

// Mirrors devlink-host.ts's DevlinkStatus (server-side, not importable here
// - see this file's own header on why the browser and server halves stay
// separate). Three real situations, not two: "absent" (no board),
// "unavailable" (a board is present but the bridge is not open/usable -
// and why, in `reason`), "connected" (open and usable). `connected` stays
// a plain boolean alongside `state` because it predates the richer type and
// every existing caller (tunables.ts) only ever needed the boolean.
export type DevlinkState = "absent" | "unavailable" | "connected";

export interface DevlinkStatus {
  connected: boolean;
  state: DevlinkState;
  port: string | null;
  reason: string | null;
}

const ABSENT_STATUS: DevlinkStatus = { connected: false, state: "absent", port: null, reason: null };

export type LineVerdict = "keep" | "done" | "skip";

interface PendingRequest {
  matcher: (line: string) => LineVerdict;
  lines: string[];
  resolve: (lines: string[]) => void;
  reject: (err: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

export class DevlinkClient {
  status: DevlinkStatus = ABSENT_STATUS;

  private ws: WebSocket | null = null;
  private statusListeners = new Set<(s: DevlinkStatus) => void>();
  private lineListeners = new Set<(text: string) => void>();
  private pending: PendingRequest[] = [];
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;

  connect(): void {
    try {
      const ws = new WebSocket(`ws://${location.host}/api/devlink`);
      this.ws = ws;
      ws.addEventListener("message", (e) => this.handleMessage(String(e.data)));
      ws.addEventListener("close", () => {
        this.ws = null;
        this.setStatus(ABSENT_STATUS);
        this.failPending(new Error("devlink socket closed"));
        this.reconnectTimer = setTimeout(() => this.connect(), 2000);
      });
      ws.addEventListener("error", () => ws.close());
    } catch {
      // No /api/devlink route to connect to (e.g. served statically through
      // markup, which is read-only): fine, status just stays disconnected,
      // same fallback connectLiveReload already relies on in main.ts.
    }
  }

  // Called once with the current status immediately, then again on every
  // change - so a caller never has to special-case "what is it right now"
  // versus "tell me when it changes".
  onStatus(cb: (s: DevlinkStatus) => void): void {
    this.statusListeners.add(cb);
    cb(this.status);
  }

  onLine(cb: (text: string) => void): void {
    this.lineListeners.add(cb);
  }

  request(cmd: string, matcher: (line: string) => LineVerdict, timeoutMs = 3000): Promise<string[]> {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      return Promise.reject(new Error("not connected to the emulator server's devlink socket"));
    }
    const ws = this.ws;
    return new Promise((resolve, reject) => {
      const entry: PendingRequest = {
        matcher,
        lines: [],
        resolve,
        reject,
        timer: setTimeout(() => {
          const idx = this.pending.indexOf(entry);
          if (idx >= 0) this.pending.splice(idx, 1);
          reject(new Error(`no reply to "${cmd}" within ${timeoutMs}ms`));
        }, timeoutMs),
      };
      this.pending.push(entry);
      ws.send(JSON.stringify({ type: "send", cmd }));
    });
  }

  private setStatus(s: DevlinkStatus): void {
    this.status = s;
    for (const cb of this.statusListeners) cb(s);
  }

  private failPending(err: Error): void {
    const all = this.pending.splice(0, this.pending.length);
    for (const p of all) {
      clearTimeout(p.timer);
      p.reject(err);
    }
  }

  private handleMessage(raw: string): void {
    let msg: unknown;
    try {
      msg = JSON.parse(raw);
    } catch {
      return;
    }
    const m = msg as {
      type?: string;
      connected?: boolean;
      state?: DevlinkState;
      port?: string | null;
      reason?: string | null;
      text?: string;
      error?: string;
    };
    if (m.type === "status") {
      this.setStatus({
        connected: !!m.connected,
        state: m.state ?? (m.connected ? "connected" : "absent"),
        port: m.port ?? null,
        reason: m.reason ?? null,
      });
    } else if (m.type === "line" && typeof m.text === "string") {
      for (const cb of this.lineListeners) cb(m.text);
      this.feedPending(m.text);
    } else if (m.type === "error" && typeof m.error === "string") {
      // Surfaced as a synthetic ERR line to whatever request is waiting -
      // same shape a real "ERR ..." reply already takes (devlinkProtocol.ts's
      // matchers all treat an ERR line as terminal), so a request() caller
      // does not need a second failure path for "the server has no bridge
      // to send this to" versus "the board itself said no".
      this.feedPending(`ERR ${m.error}`);
    }
  }

  private feedPending(line: string): void {
    const req = this.pending[0];
    if (!req) return;
    const verdict = req.matcher(line);
    if (verdict === "skip") return;
    req.lines.push(line);
    if (verdict === "done") {
      this.pending.shift();
      clearTimeout(req.timer);
      req.resolve(req.lines);
    }
  }
}
