// sync-pack.ts: copies this device's runtime, its vendored drivers and its
// wasm shim out of the puck device pack, and writes down which puck commit
// they came from.
//
//   bun run tools/sync-pack.ts            # copy, then report what changed
//   bun run tools/sync-pack.ts --check    # report only, exit 1 if stale
//   bun run tools/sync-pack.ts --pack <path>
//
// WHY THIS EXISTS. In August the runtime, the drivers and the wasm shim were
// copied out of this repo into puck as packs/rp2350-touch-amoled-18/, and
// from that day there were two living copies of the same firmware. Both kept
// moving. By the time anyone diffed them this repo had grown an orientation
// signal, a flash key/value store, a wall clock, a second sound, a bezel
// margin, an arena oracle and a retuned power-off gesture that puck's copy
// had never heard of, while puck's copy had grown a single-app roster build,
// an apps dedup in its device descriptor and a portable zig lookup that this
// one had never heard of. Neither divergence was a decision. Both were just
// two people editing two files.
//
// So there is one living copy now, and it is puck's. This script is how it
// gets here.
//
// WHAT IS SYNCED, AND WHY THAT LINE AND NOT ANOTHER.
//
//   firmware/runtime/     the runtime itself
//   firmware/lib/         the vendored Waveshare drivers, patches included
//   firmware/devlink.[ch] the USB command link, which the runtime wires
//   firmware/bootbtn.[ch] the BOOT read, which borrows the flash chip select
//   emulator/wasm/shim/   the headers that let the real firmware compile for
//                         wasm32-freestanding
//   emulator/wasm/emu_shim.c
//                         the emulator's implementation of everything the
//                         board's silicon does
//
// NOT synced, and each for a stated reason rather than by omission:
//
//   firmware/apps/        this repo's apps. The whole point.
//   firmware/apps/app_roster.inc
//                         the app table and the menu's roster. This is the
//                         seam that made the sync possible at all: the table
//                         used to live inside runtime_core.c, so taking that
//                         file meant taking somebody else's app list.
//   firmware/CMakeLists.txt
//                         names this repo's app sources.
//   emulator/wasm/build.ts
//                         names this repo's app sources AND this repo's
//                         directory topology (puck's writes the module to
//                         its own repo root, ours writes it beside itself).
//                         Two genuinely different files; a change to the
//                         zig invocation has to be made in both, by hand.
//   emulator/wasm/emu_abi.h
//                         the EMULATOR's contract, not the pack's. Puck
//                         keeps its copy at its repo root, deliberately
//                         device-neutral, and the pack compiles against it
//                         rather than owning it.
//   tools/, emulator/src/, emulator/wasm/tests/, docs/
//                         ours.
//
// PATHS INSIDE SYNCED FILES POINT AT THE MONOREPO THIS FIRMWARE CAME FROM,
// which is to say at this repo. A comment in a synced file naming
// `emulator/wasm/build.ts`, `store/`, `tools/gate/` or
// `docs/decisions/0018` resolves here and does not resolve in puck; puck's
// own pack AGENTS.md has a standing section saying exactly that about its
// firmware sources, so this is the documented state of affairs in both
// directions rather than a defect in either. Do not "fix" those paths in
// puck, and do not expect the reverse: a synced file may equally end up
// naming something that only exists there.
//
// IDEMPOTENT: a second run with nothing changed upstream copies the same
// bytes and reports "0 changed". Byte-for-byte, deliberately - the value of
// one living copy is that `diff` between the two trees is empty, and any
// rewriting on the way through (line endings, paths) would destroy exactly
// that. Files are compared before writing so an unchanged file keeps its
// mtime and the build does not redo work for nothing.
import { copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";

const TOOLS_DIR = import.meta.dir; // devices/rp2350-amoled-1.8/tools
const DEVICE_ROOT = resolve(TOOLS_DIR, ".."); // devices/rp2350-amoled-1.8

// The pack. Overridable so this can be run against a clone somewhere else,
// but defaulted rather than required: the checkout next to this one is what
// it is in practice, and a tool that cannot be run without an argument is a
// tool nobody runs.
const DEFAULT_PACK = resolve(DEVICE_ROOT, "..", "..", "..", "puck", "packs", "rp2350-touch-amoled-18");

function parseArgs(argv: string[]): { pack: string; check: boolean } {
  const i = argv.indexOf("--pack");
  return {
    pack: i !== -1 && argv[i + 1] ? resolve(process.cwd(), argv[i + 1]!) : DEFAULT_PACK,
    check: argv.includes("--check"),
  };
}

const { pack: PACK, check: CHECK_ONLY } = parseArgs(process.argv.slice(2));

// One entry per thing that moves. `from` is relative to the pack root, `to`
// to this device root. A directory entry copies every file in it that
// matches `only`, recursively; a file entry copies that one file.
//
// The two roots differ in shape and that is fine: the pack keeps its wasm
// build beside its firmware, this repo keeps it under emulator/. Mapping it
// here rather than reorganising either tree keeps both readable to their own
// readers.
type Entry = { from: string; to: string; dir?: boolean; only?: RegExp };
const ENTRIES: Entry[] = [
  // The runtime. Every source in it, no exceptions: the moment one file is
  // held back "just for now" there are two copies again.
  { from: "firmware/runtime", to: "firmware/runtime", dir: true, only: /\.(c|h|pio)$/ },
  // The vendored Waveshare drivers, with our patches already in them.
  // AGENTS.md's "never re-copy a vendor driver over a patched one" rule is
  // about the WAVESHARE ZIP, not about this: the pack's copies are the
  // patched ones, and vendor-baseline/ still holds the originals to diff
  // against. NOTICE.md comes with them, since it is what records the
  // patches.
  { from: "firmware/lib", to: "firmware/lib", dir: true, only: /\.(c|h|pio|txt|md)$/ },
  { from: "firmware/devlink.c", to: "firmware/devlink.c" },
  { from: "firmware/devlink.h", to: "firmware/devlink.h" },
  { from: "firmware/bootbtn.c", to: "firmware/bootbtn.c" },
  { from: "firmware/bootbtn.h", to: "firmware/bootbtn.h" },
  // The wasm shim: the stand-in headers, and the file that implements
  // everything the board's silicon does.
  { from: "wasm/shim", to: "emulator/wasm/shim", dir: true, only: /\.h$/ },
  { from: "wasm/emu_shim.c", to: "emulator/wasm/emu_shim.c" },
];

function walk(dir: string, only: RegExp | undefined, acc: string[] = [], base = dir): string[] {
  for (const name of readdirSync(dir)) {
    const full = join(dir, name);
    if (statSync(full).isDirectory()) walk(full, only, acc, base);
    else if (!only || only.test(name)) acc.push(relative(base, full));
  }
  return acc;
}

function sameBytes(a: string, b: string): boolean {
  if (!existsSync(b)) return false;
  return readFileSync(a).equals(readFileSync(b));
}

if (!existsSync(PACK)) {
  console.error(`pack not found: ${PACK}`);
  console.error("Pass --pack <path> if the puck checkout is somewhere else. The synced files are");
  console.error("committed in this repo, so a build does NOT need the pack present - only a sync does.");
  process.exit(1);
}

// ---- collect the work ------------------------------------------------------
type Job = { src: string; dst: string; rel: string };
const jobs: Job[] = [];
for (const entry of ENTRIES) {
  const src = join(PACK, entry.from);
  if (!existsSync(src)) {
    console.error(`pack is missing ${entry.from} - is ${PACK} really the rp2350 pack?`);
    process.exit(1);
  }
  if (entry.dir) {
    for (const rel of walk(src, entry.only)) {
      jobs.push({ src: join(src, rel), dst: join(DEVICE_ROOT, entry.to, rel), rel: join(entry.to, rel) });
    }
  } else {
    jobs.push({ src, dst: join(DEVICE_ROOT, entry.to), rel: entry.to });
  }
}

const changed = jobs.filter((j) => !sameBytes(j.src, j.dst));

// ---- provenance ------------------------------------------------------------
// Recorded from the pack's own git, not from this repo's: the question a
// reader has is "which upstream am I carrying", and the answer to that is a
// puck commit. A dirty pack worktree is called out rather than silently
// pinned, because a hash that does not describe the bytes just copied is
// worse than no hash.
function git(args: string[]): string | null {
  const r = Bun.spawnSync(["git", "-C", PACK, ...args], { stdout: "pipe", stderr: "pipe" });
  if (!r.success) return null;
  return new TextDecoder().decode(r.stdout).trim();
}

const packCommit = git(["rev-parse", "HEAD"]);
const packDate = git(["log", "-1", "--format=%cI"]);
const packSubject = git(["log", "-1", "--format=%s"]);
const packDirty = git(["status", "--porcelain", "--", "."]);

function provenanceMarkdown(): string {
  const now = new Date().toISOString().slice(0, 10);
  const dirtyFiles = (packDirty ?? "")
    .split("\n")
    .map((l) => l.trim())
    .filter(Boolean);
  const lines: string[] = [];
  lines.push("# PACK_PROVENANCE");
  lines.push("");
  lines.push("GENERATED by `bun run tools/sync-pack.ts`. Do not edit by hand.");
  lines.push("");
  lines.push("The runtime, the vendored drivers and the wasm shim in this device folder are");
  lines.push("**copies**. They are owned by the puck device pack");
  lines.push("`packs/rp2350-touch-amoled-18`, and the copy here is what makes this repo build");
  lines.push("with no puck checkout present. Edit them THERE and re-run the sync; an edit made");
  lines.push("here is overwritten on the next run and lost without a word.");
  lines.push("");
  lines.push("See `AGENTS.md`, \"The runtime is not ours\", and `tools/sync-pack.ts`'s own header");
  lines.push("comment for what is synced, what is not, and why each line falls where it does.");
  lines.push("");
  lines.push("## Last sync");
  lines.push("");
  lines.push(`| | |`);
  lines.push(`|---|---|`);
  lines.push(`| synced on | ${now} |`);
  lines.push(`| pack path | \`${PACK.replace(/\\/g, "/")}\` |`);
  lines.push(`| puck commit | \`${packCommit ?? "unknown (not a git checkout)"}\` |`);
  lines.push(`| commit date | ${packDate ?? "unknown"} |`);
  lines.push(`| commit subject | ${packSubject ?? "unknown"} |`);
  lines.push(
    `| pack worktree | ${dirtyFiles.length === 0 ? "clean" : `**DIRTY, ${dirtyFiles.length} path(s)** - the commit above does NOT describe what was copied`}`.concat(" |"),
  );
  lines.push("");
  if (dirtyFiles.length > 0) {
    lines.push("The pack had uncommitted changes when this ran, so the bytes below came from a");
    lines.push("working tree, not from that commit. That is fine while a change is in flight and");
    lines.push("a problem the moment anyone tries to reproduce this sync. Uncommitted paths:");
    lines.push("");
    for (const f of dirtyFiles) lines.push(`- \`${f}\``);
    lines.push("");
  }
  lines.push("## Files");
  lines.push("");
  lines.push("Every path below is a copy. None of them may be edited in this repo.");
  lines.push("");
  for (const j of jobs) lines.push(`- \`${j.rel.replace(/\\/g, "/")}\``);
  lines.push("");
  return lines.join("\n");
}

// ---- act -------------------------------------------------------------------
if (CHECK_ONLY) {
  if (changed.length === 0) {
    console.log(`in sync with ${PACK} (${jobs.length} files)`);
    process.exit(0);
  }
  console.error(`${changed.length} of ${jobs.length} synced file(s) differ from the pack:`);
  for (const j of changed) console.error(`  ${j.rel.replace(/\\/g, "/")}`);
  console.error("Run `bun run tools/sync-pack.ts` (and remember the pack is upstream: if the");
  console.error("difference is a fix that was made HERE, move it into the pack first).");
  process.exit(1);
}

for (const j of changed) {
  mkdirSync(dirname(j.dst), { recursive: true });
  copyFileSync(j.src, j.dst);
}

const provPath = join(DEVICE_ROOT, "PACK_PROVENANCE.md");
const prov = provenanceMarkdown();
// Written on every run, not only when a file changed: the sync DATE and the
// pack commit are facts about this run even when the bytes were already
// right, and a reader asking "is this current" is asking about the pin, not
// about whether anything moved.
writeFileSync(provPath, prov, "utf8");

console.log(`synced ${jobs.length} file(s) from ${PACK}`);
console.log(`  ${changed.length} changed, ${jobs.length - changed.length} already identical`);
for (const j of changed) console.log(`  M ${j.rel.replace(/\\/g, "/")}`);
console.log(`  pack commit ${packCommit ?? "unknown"}${packDirty ? " (WORKTREE DIRTY - see PACK_PROVENANCE.md)" : ""}`);
console.log(`  wrote PACK_PROVENANCE.md`);
if (changed.length > 0) {
  console.log("");
  console.log("Rebuild before believing anything: bun run emulator/wasm/build.ts");
}
