// Self-test for this task's testing infrastructure itself: the loopback
// devlink fixture (fixtures/loopbackLink.ts) and dev.ts's "route through
// the server" fallback logic (tools/dev.ts's openServerBridge). This is
// what `bun run harness:selftest` runs. Nothing here touches a real serial
// port or spawns PowerShell - see loopbackLink.ts's own header for why
// that matters right now (the owner is using the real board).
//
// Two halves:
//   1. Talk to a bare loopback fixture directly, proving it actually
//      speaks devlink's line protocol, INCLUDING the noise-tolerant shape
//      matching the real board's shared port design relies on
//      (tools/README-devlink.md's expectLine()/readUntilEnd()).
//   2. Spin up a minimal in-process WS server mimicking server.ts's
//      /api/devlink(+/status) routes, backed by the same fixture, and
//      prove dev.ts's exported openServerBridge() actually finds and uses
//      it - and, separately, that it cleanly returns null (never throws,
//      never falls through to a real port) when nothing is listening.
import { createLoopbackLink, SKETCH_TUNABLES, MISMATCHED_TUNABLES } from "./fixtures/loopbackLink";
import { expectLine, readUntilEnd, openServerBridge, type Bridge } from "../../tools/dev";
import { DevlinkHost, type DevlinkStatus } from "../devlink-host";

let failures = 0;
function check(label: string, cond: boolean): void {
  if (cond) {
    console.log(`PASS: ${label}`);
  } else {
    failures++;
    console.error(`FAIL: ${label}`);
  }
}
async function checkThrows(label: string, fn: () => Promise<unknown>): Promise<void> {
  try {
    await fn();
    failures++;
    console.error(`FAIL: ${label} (did not throw)`);
  } catch {
    console.log(`PASS: ${label}`);
  }
}

// Waits for a DevlinkHost's status to satisfy `pred`, via onStatus rather
// than polling - the same discipline the browser side already uses
// (main.ts's updateDevlinkStatusUI is itself just an onStatus callback).
function waitForStatus(
  host: DevlinkHost,
  pred: (s: DevlinkStatus) => boolean,
  timeoutMs: number,
  what: string
): Promise<DevlinkStatus> {
  return new Promise((resolve, reject) => {
    if (pred(host.status)) {
      resolve(host.status);
      return;
    }
    const timer = setTimeout(() => reject(new Error(`timed out waiting for ${what}`)), timeoutMs);
    host.onStatus((s) => {
      if (pred(s)) {
        clearTimeout(timer);
        resolve(s);
      }
    });
  });
}

// ===========================================================================
// 1. The fixture, talked to directly
// ===========================================================================

{
  const link = createLoopbackLink({ tunables: SKETCH_TUNABLES });
  const bridge: Bridge = { lines: link, send: (c) => link.send(c), close: () => link.close() };

  await bridge.send("PING");
  const ping = await expectLine(bridge.lines, /^OK devlink \d+ \d+ \d+$/, 500, "PING");
  check("PING replies with the devlink/version/w/h shape", /^OK devlink 1 368 448$/.test(ping));

  // Noise tolerance: inject a line that looks nothing like a devlink reply
  // BEFORE sending the real command, so it is deterministically in the
  // queue ahead of the reply - exactly the "profiler tick lands in the gap
  // before the reply starts" case tools/README-devlink.md documents on the
  // real board.
  link.injectNoise("prof app=chrono switch=15287us | loops=217088/s");
  await bridge.send("PING");
  const pingAfterNoise = await expectLine(bridge.lines, /^OK devlink \d+ \d+ \d+$/, 500, "PING");
  check("a noise line ahead of the reply is skipped, not mistaken for it", /^OK devlink 1 368 448$/.test(pingAfterNoise));

  await bridge.send("TUNE");
  const listLines = await readUntilEnd(bridge, (l) => /^TUNE \S+ \S+ \S+ \S+ \S+$/.test(l), "TUNE list");
  check("TUNE lists exactly the declared tunables", listLines.length === SKETCH_TUNABLES.length);
  check(
    "every TUNE list row starts at its declared default",
    listLines.every((l) => {
      const m = /^TUNE (\S+) (\S+) \S+ \S+ (\S+)$/.exec(l)!;
      return m[2] === m[3];
    })
  );

  await bridge.send("TUNE SET lift 500");
  const setLine = await expectLine(bridge.lines, /^(TUNE \S+ \S+|ERR .*)$/, 500, "TUNE SET");
  check("TUNE SET applies and echoes the new value", setLine === "TUNE lift 500");

  await bridge.send("TUNE SET lift 99999"); // above lift's declared max (1000)
  const clamped = await expectLine(bridge.lines, /^(TUNE \S+ \S+|ERR .*)$/, 500, "TUNE SET (out of range)");
  check("TUNE SET clamps to the declared max rather than applying the raw value", clamped === "TUNE lift 1000");

  await bridge.send("TUNE RESET lift");
  const resetLine = await expectLine(bridge.lines, /^(TUNE \S+ \S+ \S+|ERR .*)$/, 500, "TUNE RESET");
  check("TUNE RESET restores the default and reports what it replaced", resetLine === "TUNE lift 220 1000");

  await bridge.send("TUNE GET nonexistent");
  const unknownGet = await expectLine(bridge.lines, /^(TUNE \S+ \S+|ERR .*)$/, 500, "TUNE GET (unknown name)");
  check('TUNE GET on an unknown name answers "ERR unknown ..."', unknownGet === "ERR unknown nonexistent");

  await bridge.send("FROBNICATE");
  const unknownCmd = await expectLine(bridge.lines, /^(OK.*|ERR .*)$/, 500, "unknown command");
  check('an unrecognised command answers "ERR unknown <cmd>"', unknownCmd === "ERR unknown FROBNICATE");

  await checkThrows("expectLine gives up (throws) when nothing matches within its timeout", () =>
    expectLine(link, /^THIS_SHAPE_NEVER_ARRIVES$/, 150, "impossible reply")
  );

  await bridge.close();
  const afterClose = await link.readLine();
  check("readLine() resolves null once the fixture is closed", afterClose === null);
}

// ---- the deliberately mismatched tunable set (the "different builds"
// edge case the tuning panel has to show, not hide) ------------------------
{
  const link = createLoopbackLink({ tunables: MISMATCHED_TUNABLES });
  await link.send("TUNE");
  const lines = await readUntilEnd({ lines: link, send: (c) => link.send(c), close: () => link.close() }, (l) => /^TUNE \S+ \S+ \S+ \S+ \S+$/.test(l), "TUNE list");
  const names = lines.map((l) => /^TUNE (\S+) /.exec(l)![1]);
  check("the mismatched fixture's TUNE list is genuinely different from the default set", names.join(",") !== SKETCH_TUNABLES.map((t) => t.name).join(","));
  check('the mismatched fixture still lists "lift" (shared knob)', names.includes("lift"));
  check('the mismatched fixture does not list "pendgrace" (dropped)', !names.includes("pendgrace"));
  check('the mismatched fixture lists "jitter" (added, not on the emulator side)', names.includes("jitter"));
  await link.close();
}

// A build with zero declared tunables answers exactly like a real device
// with SKETCH_LIVE_TUNE off (tools/README-devlink.md: "ERR no tunables" to
// every TUNE subcommand).
{
  const link = createLoopbackLink({ tunables: [] });
  await link.send("TUNE");
  const line = await expectLine(link, /^(TUNE.*|ERR .*)$/, 500, "TUNE (no tunables)");
  check('a fixture with no declared tunables answers "ERR no tunables"', line === "ERR no tunables");
  await link.close();
}

// ===========================================================================
// 2. dev.ts's server-routing logic, against a real (in-process) WS server
// ===========================================================================

{
  const FAKE_PORT = 53187;
  const link = createLoopbackLink({ tunables: SKETCH_TUNABLES });
  const wsClients = new Set<import("bun").ServerWebSocket<unknown>>();
  void (async () => {
    for (;;) {
      const line = await link.readLine();
      if (line === null) return;
      const msg = JSON.stringify({ type: "line", text: line });
      for (const ws of wsClients) ws.send(msg);
    }
  })();

  const fakeServer = Bun.serve({
    port: FAKE_PORT,
    hostname: "127.0.0.1",
    websocket: {
      open(ws) {
        wsClients.add(ws);
      },
      close(ws) {
        wsClients.delete(ws);
      },
      message(_ws, message) {
        let msg: unknown;
        try {
          msg = JSON.parse(String(message));
        } catch {
          return;
        }
        const m = msg as { type?: string; cmd?: string };
        if (m.type === "send" && typeof m.cmd === "string") void link.send(m.cmd);
      },
    },
    routes: {
      "/api/devlink/status": {
        GET() {
          return Response.json({ connected: true, port: "FAKE0" });
        },
      },
      "/api/devlink": {
        GET(req, srv) {
          if (srv.upgrade(req)) return;
          return new Response("upgrade failed", { status: 400 });
        },
      },
    },
  });

  try {
    process.env.DEVLINK_SERVER_URL = `http://127.0.0.1:${FAKE_PORT}`;
    const bridge = await openServerBridge();
    check("openServerBridge() finds the fake server and returns a Bridge", bridge !== null);
    if (bridge) {
      await bridge.send("PING");
      const reply = await expectLine(bridge.lines, /^OK devlink \d+ \d+ \d+$/, 1000, "PING via server route");
      check("a command sent through openServerBridge() reaches the fixture and its reply comes back", /^OK devlink 1 368 448$/.test(reply));
      await bridge.close();
    }

    process.env.DEVLINK_SERVER_URL = "http://127.0.0.1:1"; // a port nothing listens on
    const noServer = await openServerBridge();
    check("openServerBridge() returns null (never throws) when the server is unreachable - the safe fallback path", noServer === null);
  } finally {
    fakeServer.stop(true);
    await link.close();
    delete process.env.DEVLINK_SERVER_URL;
  }
}

// ===========================================================================
// 3. DevlinkHost's own bridge-ownership state machine (docs/decisions/0004-
//    the-day-the-instruments-lied.md): "connected" has to mean usable, not
//    just detected, and a failed or lost bridge has to retry on its own,
//    never sit claiming health. Driven entirely through fake mode's
//    scriptable open failures (DevlinkHostOptions.fakeOpenFailures) and the
//    test-only debugDropCurrentBridge() hook - no PowerShell, no real port,
//    same constraint as sections 1-2 above.
// ===========================================================================

{
  const host = new DevlinkHost({ mode: "fake", fakeOpenFailures: 2 });
  await host.start();

  // The very first status a listener sees, right after the scripted open
  // failure, must already be honest: a board (FAKE0) was seen, but it is
  // not usable, and why - never a bare "connected: true" claiming more than
  // is actually known.
  const firstFailure = await waitForStatus(host, (s) => s.state !== "absent", 2000, "the first (scripted) open attempt to fail");
  check("a failed open reports state=unavailable, not connected", firstFailure.state === "unavailable" && firstFailure.connected === false);
  check("a failed open still names the port a board was seen on", firstFailure.port === "FAKE0");
  check("a failed open carries a non-empty reason", typeof firstFailure.reason === "string" && firstFailure.reason.length > 0);

  // The obstacle (2 scripted failures) clears on its own: the SAME host,
  // never restarted, must retry and reach connected with no external call
  // telling it to try again.
  const recovered = await waitForStatus(host, (s) => s.state === "connected", 10_000, "recovery to connected once the scripted failures run out");
  check("the bridge recovers on its own once the obstacle clears, no restart", recovered.connected === true && recovered.port === "FAKE0" && recovered.reason === null);

  // ---- loss detection: the port disappearing has to be noticed by the
  // bridge itself. Fake mode has no watcher polling at all, so this
  // exercises pumpBridgeLines's own EOF handling in isolation - the
  // "reverse" case the task calls out: nobody told it, it noticed. -------
  const dropped = host.debugDropCurrentBridge();
  check("the test hook actually had a live fake bridge to drop", dropped);
  const afterDrop = await waitForStatus(host, (s) => s.state !== "connected", 3000, "loss detection once the port disappears");
  check("a bridge whose port disappears stops claiming to be usable", afterDrop.connected === false);
  check("loss is reported with a reason, not silently", typeof afterDrop.reason === "string" && afterDrop.reason.length > 0);

  // And it recovers again on its own - the owner's stated NORMAL case (the
  // board reflashed constantly), not an edge case, and again no restart.
  const recoveredAgain = await waitForStatus(host, (s) => s.state === "connected", 10_000, "recovery after the port comes back");
  check("the bridge reconnects on its own after the port comes back, still no restart", recoveredAgain.connected === true);

  await host.stop();
}

// A bridge that never stops failing to open must never claim connected -
// the "do not make it vague" boundary from the task: staying unavailable,
// with a reason, forever, is still an honest and distinguishable state, not
// a silent hang or a false positive.
{
  const host = new DevlinkHost({ mode: "fake", fakeOpenFailures: Number.MAX_SAFE_INTEGER });
  await host.start();
  await new Promise((r) => setTimeout(r, 3200)); // long enough for at least one retry to have fired and failed again
  check(
    "a bridge that keeps failing to open never claims connected, across retries",
    host.status.state === "unavailable" && host.status.connected === false && !!host.status.reason
  );
  await host.stop();
}

console.log(failures === 0 ? "\nALL PASS" : `\n${failures} FAILED`);
process.exit(failures === 0 ? 0 : 1);
