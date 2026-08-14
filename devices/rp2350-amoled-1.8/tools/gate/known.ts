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
