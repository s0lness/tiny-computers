#!/usr/bin/env python3
"""
cam: point the USB webcam at the physical panel and photograph it.

Half of how this device is verified end to end: devlink (dev.ts) shows what
the framebuffer holds, this shows what the panel actually displays, and
comparing the two is what catches a partial-refresh or panel-timing bug that
a framebuffer screenshot alone cannot (see docs/decisions/0001-push-min-width.md
for a bug exactly this shape).

WHY PYTHON, NOT TYPESCRIPT. This repo's rule is TypeScript only for tooling
(see AGENTS.md), and this file is the one deliberate exception. What was
tried first: `ffmpeg` on PATH (there is none: `where ffmpeg` finds nothing on
this machine, and tools/README-devlink.md documents this project already
avoiding a PATH dependency it cannot rely on for exactly this reason), and a
native Node/Bun DirectShow binding (no usable one exists: tools/dev.ts's own
header comment already ruled out prebuilt native npm bindings on this
machine, Windows on ARM64, "a frequent source of pain" - camera capture
bindings are, if anything, a worse bet than the serial ones that comment was
written about, since far fewer of them ship an arm64 prebuilt at all). OpenCV
(`cv2`, via `pip`) already works here, needs no native npm dependency, and is
what the throwaway script this file replaces was already built on. The
project's rule bans raw JavaScript specifically ("aucun JavaScript brut nulle
part"); it does not mandate TypeScript for every tool regardless of whether a
TypeScript path exists, and here it genuinely does not.

THE FAILURE MODE THIS FILE EXISTS TO PREVENT. cv2.VideoCapture(index) opens
whatever the OS currently enumerates at that index, and the index is not
stable: on this machine the Logitech C920 has been index 1 and, on a later
run, would not open at all while the laptop's own front camera answered as
index 0 instead. A script that grabs an index photographs whatever is at
that index that day - which, once, was the operator's face, not the device.
So this tool selects by NAME, matched against what Windows actually reports
for each camera, and refuses to guess: no match, more than one match, or a
failed open are all loud, immediate failures, never a silent fallback to
some other camera.
"""
import argparse
import re
import sys
from pathlib import Path

try:
    import cv2
except ImportError:
    print("cam: opencv-python is not installed (pip install opencv-python)", file=sys.stderr)
    sys.exit(1)

try:
    from pygrabber.dshow_graph import FilterGraph
except ImportError:
    print(
        "cam: pygrabber is not installed (pip install pygrabber comtypes). "
        "pygrabber enumerates DirectShow device NAMES in the same order "
        "cv2's CAP_DSHOW backend indexes them, which is what lets this tool "
        "select a camera by name instead of by a shifting index.",
        file=sys.stderr,
    )
    sys.exit(1)


def list_devices() -> list[str]:
    return FilterGraph().get_input_devices()


def find_camera_index(pattern: str, devices: list[str]) -> int:
    """
    Matches pattern (a case-insensitive regex) against every enumerated
    device name and returns the index of the SOLE match. Zero matches or
    more than one are both refused rather than guessed at: a script that
    picks "the first match" or "the last one that worked" is exactly the
    kind of silent fallback that once pointed this tool at a laptop's own
    camera instead of the one aimed at the device.
    """
    rx = re.compile(pattern, re.IGNORECASE)
    matches = [(i, name) for i, name in enumerate(devices) if rx.search(name)]

    if not matches:
        available = "\n".join(f"  [{i}] {name}" for i, name in enumerate(devices)) or "  (none enumerated)"
        print(
            f"cam: no camera name matches /{pattern}/i. Available devices:\n{available}\n"
            f"Pass --name with a pattern that matches one of the above.",
            file=sys.stderr,
        )
        sys.exit(1)

    if len(matches) > 1:
        ambiguous = "\n".join(f"  [{i}] {name}" for i, name in matches)
        print(
            f"cam: /{pattern}/i matches more than one camera, refusing to guess which:\n{ambiguous}\n"
            f"Pass --name with a more specific pattern.",
            file=sys.stderr,
        )
        sys.exit(1)

    index, name = matches[0]
    print(f"cam: matched [{index}] {name}", file=sys.stderr)
    return index


def open_camera(index: int, name: str, width: int) -> cv2.VideoCapture:
    cap = cv2.VideoCapture(index, cv2.CAP_DSHOW)
    if not cap.isOpened():
        # FAIL LOUDLY: no retry, no other backend, no other index. An agent
        # that silently fell back to "whatever camera did open" is the exact
        # failure mode this tool exists to rule out.
        print(f"cam: FAILED to open camera [{index}] {name!r}. Not falling back to another camera.", file=sys.stderr)
        sys.exit(1)
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, int(width * 9 / 16))
    cap.set(cv2.CAP_PROP_AUTOFOCUS, 1)
    return cap


def parse_crop(spec: str) -> tuple[int, int, int, int]:
    parts = spec.split(",")
    if len(parts) != 4:
        print(f"cam: --crop wants x,y,w,h, got {spec!r}", file=sys.stderr)
        sys.exit(1)
    try:
        x, y, w, h = (int(p) for p in parts)
    except ValueError:
        print(f"cam: --crop values must be integers, got {spec!r}", file=sys.stderr)
        sys.exit(1)
    return x, y, w, h


def autocrop_to_bright(frame, threshold: int, pad: int):
    """
    Finds the bounding box of every pixel brighter than threshold and crops
    to it, padded. This exists because the device is handheld and moves
    around the desk between sessions, so a fixed --crop rectangle from one
    session is wrong in the next. The panel is a self-lit AMOLED against a
    normally-lit desk, so it is reliably the brightest thing in frame: no
    edge detection or object recognition needed, just a threshold and a
    bounding box, which is cheap, has no model to be wrong about, and is
    exactly what located the panel by hand before this was automated.
    Returns None (caller falls back to the full frame) if nothing in the
    frame is brighter than threshold, e.g. the panel is asleep or out of
    shot entirely.
    """
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    ys, xs = (gray > threshold).nonzero()
    if len(xs) == 0:
        return None
    h, w = frame.shape[:2]
    x0 = max(0, int(xs.min()) - pad)
    y0 = max(0, int(ys.min()) - pad)
    x1 = min(w, int(xs.max()) + 1 + pad)
    y1 = min(h, int(ys.max()) + 1 + pad)
    return frame[y0:y1, x0:x1]


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Photograph the physical panel with a named USB webcam."
    )
    ap.add_argument("out", nargs="?", help="output path (out.png), or out-NN.png per frame with --burst")
    ap.add_argument(
        "--name",
        default="C920|Webcam",
        help='camera name pattern, case-insensitive regex (default: "C920|Webcam")',
    )
    ap.add_argument("--burst", type=int, default=1, help="number of frames to capture (default 1)")
    ap.add_argument("--width", type=int, default=1920, help="requested capture width (default 1920)")
    ap.add_argument("--warmup", type=int, default=12, help="frames to discard before capturing (default 12)")
    ap.add_argument("--crop", help="fixed crop region as x,y,w,h, applied to every saved frame")
    ap.add_argument(
        "--autocrop",
        action="store_true",
        help="crop to the bounding box of pixels brighter than --autocrop-threshold "
        "(finds a self-lit panel regardless of where it sits in frame; mutually "
        "exclusive with --crop)",
    )
    ap.add_argument("--autocrop-threshold", type=int, default=200, help="greyscale cutoff for --autocrop (default 200)")
    ap.add_argument("--autocrop-pad", type=int, default=20, help="pixels of padding around the --autocrop bounding box (default 20)")
    ap.add_argument("--list", action="store_true", help="list enumerated cameras and exit, no capture")
    args = ap.parse_args()

    devices = list_devices()

    if args.list:
        for i, name in enumerate(devices):
            print(f"[{i}] {name}")
        return

    if not args.out:
        ap.error("out is required unless --list is given")
    if args.crop and args.autocrop:
        ap.error("--crop and --autocrop are mutually exclusive")

    index = find_camera_index(args.name, devices)
    crop = parse_crop(args.crop) if args.crop else None

    cap = open_camera(index, devices[index], args.width)
    try:
        # The sensor needs a moment to expose and focus; the first frames
        # off a freshly opened capture are routinely dark or soft.
        for _ in range(args.warmup):
            cap.read()

        out_path = Path(args.out)
        saved = 0
        for i in range(args.burst):
            ok, frame = cap.read()
            if not ok:
                print(f"cam: frame {i}: read failed", file=sys.stderr)
                continue
            if crop:
                x, y, w, h = crop
                frame = frame[y : y + h, x : x + w]
            elif args.autocrop:
                cropped = autocrop_to_bright(frame, args.autocrop_threshold, args.autocrop_pad)
                if cropped is None:
                    print(
                        f"cam: --autocrop found nothing brighter than {args.autocrop_threshold}, "
                        f"saving the full frame instead",
                        file=sys.stderr,
                    )
                else:
                    frame = cropped
            dest = out_path if args.burst == 1 else out_path.with_name(f"{out_path.stem}-{i:02d}{out_path.suffix}")
            cv2.imwrite(str(dest), frame)
            h_actual, w_actual = frame.shape[:2]
            print(f"cam: wrote {dest} ({w_actual}x{h_actual})")
            saved += 1
    finally:
        cap.release()

    if saved == 0:
        print("cam: no frames saved", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
