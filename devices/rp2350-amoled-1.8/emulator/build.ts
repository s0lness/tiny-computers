// Produces a plain static dist/ (HTML + one bundled JS + CSS) so the
// emulator can be served by markup/serve.ts, which is a read-only static
// file server and does not bundle TypeScript on the fly the way
// `bun run server.ts` does via Bun's HTML-import dev bundler. Run this
// after editing src/, then point markup at dist/ (see README.md).
//
// This static build is a review copy, not the dev loop: markup's server is
// read-only (see its own comment, "no change to the target site's repo,
// ever"), so /api/quit, /api/freeze, /api/trace and the /api/livereload
// websocket all 404 under it. The page handles that gracefully (freeze and
// trace-save show an inline error instead of throwing; there is simply no
// live-reload connection to make). The wasm module itself is copied in if
// it has already been built, so the page still runs.
import { mkdirSync, rmSync, copyFileSync, readFileSync, writeFileSync, existsSync } from "node:fs";
import { join } from "node:path";

const ROOT = import.meta.dir;
const SRC = join(ROOT, "src");
const DIST = join(ROOT, "dist");
const WASM_SRC = join(ROOT, "wasm", "dist", "emu.wasm");

if (existsSync(DIST)) rmSync(DIST, { recursive: true, force: true });
mkdirSync(DIST, { recursive: true });

const result = await Bun.build({
  entrypoints: [join(SRC, "main.ts")],
  outdir: DIST,
  naming: "main.js",
  target: "browser",
  format: "esm",
  minify: false,
});
if (!result.success) {
  for (const log of result.logs) console.error(log);
  process.exit(1);
}

for (const css of ["family-budget.css", "app.css"]) {
  copyFileSync(join(SRC, css), join(DIST, css));
}

if (existsSync(WASM_SRC)) {
  mkdirSync(join(DIST, "wasm"), { recursive: true });
  copyFileSync(WASM_SRC, join(DIST, "wasm", "emu.wasm"));
  console.log("copied wasm/dist/emu.wasm into dist/wasm/");
} else {
  console.log("wasm/dist/emu.wasm not built yet; dist/ will show the missing-module message until it is");
}

const html = readFileSync(join(SRC, "index.html"), "utf8").replace(
  '<script type="module" src="./main.ts"></script>',
  '<script type="module" src="./main.js"></script>'
);
writeFileSync(join(DIST, "index.html"), html);

console.log(`built dist/ (${result.outputs.length} output file(s))`);
