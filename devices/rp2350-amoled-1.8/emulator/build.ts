// Produces a plain static dist/ (HTML + one bundled JS + CSS) so the
// emulator can be served by markup/serve.ts, which is a read-only static
// file server and does not bundle TypeScript on the fly the way
// `bun run server.ts` does via Bun's HTML-import dev bundler. Run this
// after editing src/, then point markup at dist/ (see README.md).
import { mkdirSync, rmSync, copyFileSync, readFileSync, writeFileSync, existsSync } from "node:fs";
import { join } from "node:path";

const ROOT = import.meta.dir;
const SRC = join(ROOT, "src");
const DIST = join(ROOT, "dist");

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

const html = readFileSync(join(SRC, "index.html"), "utf8").replace(
  '<script type="module" src="./main.ts"></script>',
  '<script type="module" src="./main.js"></script>'
);
writeFileSync(join(DIST, "index.html"), html);

console.log(`built dist/ (${result.outputs.length} output file(s))`);
