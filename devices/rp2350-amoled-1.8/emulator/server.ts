// rp2350-amoled-1.8 emulator: local server. Static app only, plus /api/quit
// (the only mutating route: there is nothing else to persist server-side,
// tuning state lives in the page and PNG export happens client-side).
// Launched by hand (`bun run server.ts` / `bun dev`); no console to Ctrl+C
// once backgrounded, so the quit button is not optional (local-app.md).
import index from "./src/index.html";

const PORT = Number(process.env.PORT) || 5330;

function guard(req: Request): boolean {
  // A simple cross-origin POST cannot set a custom header without a
  // preflight we never answer, so this closes the localhost-CSRF hole.
  return req.headers.get("x-rp2350-emulator") === "1";
}

function quit(req: Request): Response {
  if (!guard(req)) return new Response("nope", { status: 403 });
  setTimeout(() => process.exit(0), 150);
  return Response.json({ ok: true, stopping: true });
}

const server = Bun.serve({
  port: PORT,
  // Local-only tool: without an explicit hostname Bun listens on every
  // interface (:::5330), reachable by anyone on the same network.
  hostname: "127.0.0.1",
  development: { hmr: true, console: true },
  routes: {
    "/api/quit": { POST: quit },
    "/*": index,
  },
});

console.log(`rp2350-amoled-1.8 emulator -> http://127.0.0.1:${server.port}`);
console.log(`markup annotation: bun build.ts, then bun ../markup/serve.ts --dir "${import.meta.dir}/dist" --port 5332`);
