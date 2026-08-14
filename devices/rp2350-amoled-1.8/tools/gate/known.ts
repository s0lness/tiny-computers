// What the gate found on the day it was written, and has not fixed.
//
// A gate that goes red on its first run is a gate somebody comments out by
// the end of the week, and this one went red twice on its first run - both
// times on real defects, in code neither of them belonged to. So there is
// a baseline, with three properties that keep it from becoming a place
// where findings go to die:
//
//   1. Every entry is PRINTED, loudly, on every run. A known finding is
//      not a silent pass; it is a line of output that says the board will
//      show this today.
//   2. Every entry names who decides. Both of these are layout questions
//      with a visible cost, and this gate does not get to spend the
//      owner's pixels for him.
//   3. An entry that no longer reproduces FAILS THE RUN. A stale exemption
//      is how a checker quietly stops covering something, and the
//      clean-input baseline next door has the same rule for the same
//      reason.
//
// An entry is not a way to make a new violation go away. Adding one is an
// edit to this file, in this directory, under this paragraph.

import type { Violation } from "./rules";

export interface KnownFinding {
  rule: string;
  /** Matched against the violation's own detail line. */
  match: RegExp;
  found: string;
  what: string;
  /** Why it is still here, and what deciding it costs. */
  why: string;
  decides: string;
}

export const KNOWN: KnownFinding[] = [
  {
    rule: "pushed-or-invisible",
    match: /^draw\/hold .*: \d+ pixels changed outside/,
    found: "2026-08-15, by this gate, on its first run",
    what:
      "opening the sketchpad's palette whitens the framebuffer's outermost columns (x=0..5, and " +
      "x=366..367) and then pushes only (6,4) 360x438. Anything already drawn in those six columns " +
      "is erased in memory and never erased on the panel, so on hardware the old ink stays under " +
      "the palette. Reproduce: draw anywhere near the left edge, then long-press to open the palette.",
    why:
      "the fix is in firmware/apps/sketch.c, which another agent is editing right now; a second " +
      "hand in that file today would cost more than the bug does. It is the same shape as the " +
      "gutter residue this app has already been fixed for once.",
    decides: "whoever owns sketch.c next",
  },
  {
    rule: "bezel",
    match: /^four\//,
    found: "2026-08-15, by this gate, on its first run",
    what:
      "Connect Four's waiting piece crosses into the hidden band. Its resting centre is " +
      "HOPPER_CY = SAFE_Y0 + HOLE_R (four.c), which puts the top of the disc exactly ON the " +
      "bezel line, and then two things push it past: the idle bob moves it UP by BOB_AMP (5px), " +
      "and the hand-off pop's ease_out_back overshoots to about 1.10 of full size (another 2px " +
      "of radius). Up to 5px of the top of the piece is under the case.",
    why:
      "every fix costs layout. Moving HOPPER_CY down by 5px pushes the piece's bottom to 56, " +
      "into the slab that starts at 52, so the piece would overlap the board. Shrinking or " +
      "inverting the bob contradicts four.c's own comment (\"a cue that rises reads as offering " +
      "itself\"), which was a decision, not an accident. This is a taste call about a game the " +
      "owner has already iterated on twice.",
    decides: "the owner",
  },
];

export interface Partition {
  fresh: Violation[];
  known: { finding: KnownFinding; count: number }[];
  /** Entries that matched nothing: stale exemptions, and a hard failure. */
  stale: KnownFinding[];
}

export function partition(violations: Violation[]): Partition {
  const counts = new Map<KnownFinding, number>();
  const fresh: Violation[] = [];
  for (const v of violations) {
    const hit = KNOWN.find((k) => k.rule === v.rule && k.match.test(v.detail));
    if (hit) counts.set(hit, (counts.get(hit) ?? 0) + 1);
    else fresh.push(v);
  }
  return {
    fresh,
    known: KNOWN.filter((k) => counts.has(k)).map((k) => ({ finding: k, count: counts.get(k)! })),
    stale: KNOWN.filter((k) => !counts.has(k)),
  };
}
