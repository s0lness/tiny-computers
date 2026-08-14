// Pure parsing helpers for devlink's TUNE grammar (tools/README-devlink.md,
// firmware/devlink.c), used by the in-page tuning panel (tunables.ts) to
// read a connected board's own live-tunable state over devlinkClient.ts's
// WebSocket relay.
//
// Deliberately a separate, dependency-free module from tools/dev.ts's own
// TUNE parsing: dev.ts is a Bun CLI, this runs in the browser, and the two
// cannot share an import (no Node/Bun API here, nothing here reaches a
// filesystem or a process). The shapes mirror devlink.c's grammar exactly,
// the same way dev.ts's own TUNE_LIST_RE/TUNE_VALUE_RE/TUNE_RESET_RE do -
// see that file if this ever needs a third implementation kept in sync.
//
// Every matcher below follows the wire protocol's own rule (see
// tools/README-devlink.md, "Wire protocol"): match the *expected shape*,
// not "not a known noise prefix". A line that is not the shape being
// waited for is "skip" (keep waiting), never treated as failure - the same
// discipline dev.ts's expectLine()/readUntilEnd() already apply against
// the real board's shared-port noise.

export type LineVerdict = "keep" | "done" | "skip";

export interface DeviceTuneRow {
  name: string;
  value: number;
  min: number;
  max: number;
  default: number;
}

const TUNE_LIST_LINE_RE = /^TUNE (\S+) (\S+) (\S+) (\S+) (\S+)$/;
const TUNE_VALUE_LINE_RE = /^TUNE (\S+) (\S+)$/;
const TUNE_RESET_LINE_RE = /^TUNE (\S+) (\S+) (\S+)$/;
const ERR_LINE_RE = /^ERR (.*)$/;

export function parseTuneListLine(line: string): DeviceTuneRow | null {
  const m = TUNE_LIST_LINE_RE.exec(line);
  if (!m) return null;
  return { name: m[1], value: Number(m[2]), min: Number(m[3]), max: Number(m[4]), default: Number(m[5]) };
}

export function parseTuneValueLine(line: string): { name: string; value: number } | null {
  const m = TUNE_VALUE_LINE_RE.exec(line);
  return m ? { name: m[1], value: Number(m[2]) } : null;
}

export function parseTuneResetLine(line: string): { name: string; applied: number; previous: number } | null {
  const m = TUNE_RESET_LINE_RE.exec(line);
  return m ? { name: m[1], applied: Number(m[2]), previous: Number(m[3]) } : null;
}

export function parseErrLine(line: string): string | null {
  const m = ERR_LINE_RE.exec(line);
  return m ? m[1] : null;
}

// `TUNE` with no arguments: one TUNE_LIST_LINE_RE row per declared
// tunable, then END - or a single "ERR no tunables" line (no END) on a
// build without SKETCH_LIVE_TUNE.
export function tuneListMatcher(line: string): LineVerdict {
  if (line === "END") return "done";
  if (ERR_LINE_RE.test(line)) return "done";
  if (TUNE_LIST_LINE_RE.test(line)) return "keep";
  return "skip";
}

// `TUNE GET <name>` / `TUNE SET <name> <value>`: one TUNE_VALUE_LINE_RE
// line, or one ERR line.
export function tuneValueMatcher(line: string): LineVerdict {
  if (TUNE_VALUE_LINE_RE.test(line)) return "done";
  if (ERR_LINE_RE.test(line)) return "done";
  return "skip";
}

// `TUNE RESET <name>`: one TUNE_RESET_LINE_RE line, or one ERR line.
export function tuneResetOneMatcher(line: string): LineVerdict {
  if (TUNE_RESET_LINE_RE.test(line)) return "done";
  if (ERR_LINE_RE.test(line)) return "done";
  return "skip";
}
