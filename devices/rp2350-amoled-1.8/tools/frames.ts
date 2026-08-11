/*
 * Split a screen recording of the device into frames so they can be looked at
 * one by one. A still photo cannot show a timing fault: whether a stroke broke
 * into dots, where it lost contact, whether a bridge fired late. Frames can.
 *
 *   bun tools/frames.ts <video> [--fps 10] [--out dir] [--max 40]
 *
 * ffmpeg comes from the imageio-ffmpeg python package rather than the PATH,
 * because this machine has no system ffmpeg and that package ships a portable
 * build with no admin rights needed.
 */
import { spawnSync } from "node:child_process";
import { mkdirSync, readdirSync, rmSync, existsSync } from "node:fs";
import { join, basename } from "node:path";

function ffmpegPath(): string {
  const r = spawnSync(
    "python",
    ["-c", "import imageio_ffmpeg;print(imageio_ffmpeg.get_ffmpeg_exe())"],
    { encoding: "utf8" },
  );
  const p = r.stdout?.trim();
  if (!p || !existsSync(p)) {
    throw new Error(
      "ffmpeg not found. Install it with: python -m pip install imageio-ffmpeg",
    );
  }
  return p;
}

const args = process.argv.slice(2);
const video = args.find((a) => !a.startsWith("--"));
if (!video) {
  console.error("usage: bun tools/frames.ts <video> [--fps N] [--out DIR] [--max N]");
  process.exit(1);
}

function flag(name: string, dflt: string): string {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] ? args[i + 1] : dflt;
}

const fps = flag("fps", "10");
const max = parseInt(flag("max", "40"), 10);
const outDir = flag("out", join("capture", "frames", basename(video).replace(/\.[^.]+$/, "")));

rmSync(outDir, { recursive: true, force: true });
mkdirSync(outDir, { recursive: true });

// Scale down: the panel is 368x448 inside a phone-camera frame, so full
// resolution wastes far more than it reveals.
const r = spawnSync(
  ffmpegPath(),
  ["-i", video, "-vf", `fps=${fps},scale=720:-1`, "-y", join(outDir, "f%03d.jpg")],
  { encoding: "utf8" },
);
if (r.status !== 0) {
  console.error(r.stderr?.slice(-2000));
  process.exit(1);
}

const frames = readdirSync(outDir).filter((f) => f.endsWith(".jpg")).sort();
if (frames.length > max) {
  // Keep an evenly spaced subset rather than the first N, so the whole
  // recording is represented instead of only its opening moment.
  const keep = new Set<string>();
  for (let i = 0; i < max; i++) {
    keep.add(frames[Math.round((i * (frames.length - 1)) / (max - 1))]);
  }
  for (const f of frames) if (!keep.has(f)) rmSync(join(outDir, f));
}

const kept = readdirSync(outDir).filter((f) => f.endsWith(".jpg")).sort();
console.log(`${kept.length} frames (of ${frames.length}) in ${outDir}`);
for (const f of kept) console.log(join(outDir, f));
