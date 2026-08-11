/**
 * Bundles the local tldraw capture app.
 *
 *   bun tools/build-capture.ts
 *
 * Two things here are load-bearing and cost an hour to find, so do not
 * "simplify" them:
 *
 * 1. tldraw's shipped ESM reads bare `process.env.X` (TLDRAW_ENV, GC_API_KEY
 *    and friends). In a browser `process` is undefined, so the module throws on
 *    evaluation and the page renders blank with no visible error. They are
 *    defined below, and index.html additionally stubs `window.process`.
 *
 * 2. The app is written with React.createElement, not JSX. Bun chooses the
 *    dev vs production JSX runtime from its own heuristics, and got it wrong in
 *    both directions here: unminified builds emitted `jsxDEV` calls that React's
 *    production runtime does not export, and minified builds hid the resulting
 *    error behind mangled names. No JSX means no transform to get wrong.
 */

import { join } from "node:path";

const APP = join(import.meta.dir, "capture-app");

const result = await Bun.build({
  entrypoints: [join(APP, "app.ts")],
  outdir: APP,
  target: "browser",
  define: {
    "process.env.NODE_ENV": '"production"',
    "process.env.TLDRAW_ENV": '"production"',
    "process.env.NEXT_PUBLIC_TLDRAW_ENV": '"production"',
    "process.env.VERCEL_PUBLIC_TLDRAW_ENV": '"production"',
    "process.env.NEXT_PUBLIC_GC_API_KEY": '""',
  },
});

if (!result.success) {
  for (const log of result.logs) console.error(log);
  process.exit(1);
}

for (const out of result.outputs) {
  console.log("built", out.path.split(/[\\/]/).pop());
}
