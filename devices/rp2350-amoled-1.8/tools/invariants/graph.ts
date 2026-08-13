// Core1 reachability: a breadth-first search over direct branches (bl, b,
// b.w) from an explicit root set. See docs/decisions/0006 - this is the
// "~150-line graph helper" the plan budgets, shared by rules 0, 2 and 3.
//
// It is unsound by construction (an indirect call site is a dead end the
// BFS cannot follow), which is exactly why rule 0 exists: it requires every
// such site inside the reached set to be resolved or annotated, so the
// unsoundness is a gated, visible list rather than a silent hole.

import type { DisasmFunction, IndirectSite } from "./types";

// 197 symbols in the reviewed build carried these (core1's own i2c helpers
// included); stripping them is required for name-based matching against an
// invariant's blocklist, not for the BFS itself (which matches objdump's
// own already-resolved, suffix-intact target names).
export function stripCloneSuffix(name: string): string {
  return name.replace(/\.(constprop|isra|part)(\.\d+)?/g, "");
}

export interface IndirectSiteAt extends IndirectSite {
  func: string; // raw name of the function containing the site
}

export interface Reachability {
  // Raw symbol names (clone suffixes intact) of every function reached.
  reached: Set<string>;
  // Clone-suffix-stripped names, for matching a blocklist without caring
  // which specific clone of a function was inlined into the build.
  reachedCanonical: Set<string>;
  // Every blx r* site found inside a reached function.
  indirectSites: IndirectSiteAt[];
}

export function reachableFrom(
  functions: Map<string, DisasmFunction>,
  roots: string[]
): Reachability {
  for (const root of roots) {
    if (!functions.has(root)) {
      throw new Error(
        `invariant checker: root "${root}" is not a known function symbol in this build ` +
          `(renamed, inlined, or removed - update the root set in the rules file)`
      );
    }
  }

  const reached = new Set<string>();
  const indirectSites: IndirectSiteAt[] = [];
  const queue = [...roots];

  while (queue.length > 0) {
    const name = queue.shift()!;
    if (reached.has(name)) continue;
    reached.add(name);

    const fn = functions.get(name);
    if (!fn) continue; // a resolved branch target with no function body of its own (e.g. an alias)

    for (const site of fn.indirect) indirectSites.push({ func: fn.name, ...site });
    for (const branch of fn.branches) {
      if (!reached.has(branch.targetName)) queue.push(branch.targetName);
    }
  }

  return {
    reached,
    reachedCanonical: new Set([...reached].map(stripCloneSuffix)),
    indirectSites,
  };
}

// Rules 2 and 3 (docs/decisions/0006) are both shaped "reachable from core1
// must not intersect set Y" - this is the one helper both instantiate.
// Returns the raw (suffix-intact) names actually found, so a violation can
// print the real symbol rather than the canonical name that was searched
// for.
export function reachedNamesMatching(reach: Reachability, canonicalNames: Iterable<string>): string[] {
  const wanted = new Set(canonicalNames);
  const hits: string[] = [];
  for (const raw of reach.reached) {
    if (wanted.has(stripCloneSuffix(raw))) hits.push(raw);
  }
  return hits.sort();
}
