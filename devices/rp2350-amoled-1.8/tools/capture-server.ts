/**
 * Serves the local tldraw capture app and receives what gets drawn.
 *
 *   bun tools/capture-server.ts        then open http://127.0.0.1:5310
 *
 * Bound to 127.0.0.1 deliberately: Bun.serve with no hostname listens on every
 * interface, which would put this on the WiFi.
 */

import { mkdirSync, writeFileSync, existsSync } from "node:fs";
import { join } from "node:path";

const ROOT = join(import.meta.dir, "..");
const APP = join(ROOT, "tools", "capture-app");
const OUT = join(ROOT, "capture");
const PORT = 5310;

if (!existsSync(join(APP, "app.js"))) {
  console.error("app.js missing. Run: bun run tools/build-capture.ts");
  process.exit(1);
}

mkdirSync(OUT, { recursive: true });

Bun.serve({
  port: PORT,
  hostname: "127.0.0.1",
  async fetch(req) {
    const url = new URL(req.url);

    if (req.method === "POST" && url.pathname === "/save") {
      const body = await req.json();
      const path = join(OUT, "handwriting.json");
      writeFileSync(path, JSON.stringify(body, null, 2));
      const n = body?.strokes?.length ?? 0;
      const pts = (body?.strokes ?? []).reduce((a: number, s: any) => a + s.points.length, 0);
      console.log(`captured ${n} strokes, ${pts} points -> capture/handwriting.json`);
      return new Response(`${n} strokes, ${pts} points`);
    }

    if (url.pathname === "/app.js") {
      return new Response(Bun.file(join(APP, "app.js")), {
        headers: { "content-type": "text/javascript" },
      });
    }
    if (url.pathname === "/app.css") {
      return new Response(Bun.file(join(APP, "app.css")), {
        headers: { "content-type": "text/css" },
      });
    }
    return new Response(Bun.file(join(APP, "index.html")), {
      headers: { "content-type": "text/html" },
    });
  },
});

console.log(`capture app on http://127.0.0.1:${PORT}`);
console.log("draw inside the dashed box, then press 'Send to device'");
