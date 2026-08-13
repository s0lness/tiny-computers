// The markup tool's journal vocabulary (C:\Users\sylve\projects\markup,
// src/types.ts), applied to a frozen device screenshot instead of a web
// page, per the owner's instruction to follow that format "where it fits"
// rather than invent a second one.
//
// Reused as-is: a stroke IS a stroke, and a note is typed feedback with
// markup's own three-way split, same letters and same meaning (f = fix,
// q = question, n = new thing).
//
// NOT reused: everything markup's Stroke/Note/Pin carry that only makes
// sense for a DOM page under it (attached element selectors, a captured
// CSS layout frame, viewport/theme, in-place text edits). A frozen panel is
// a bitmap, not a DOM, so those fields would have nothing real to hold and
// are dropped rather than filled with meaningless values.

export type MarkType = "f" | "q" | "n";

export interface Point {
  x: number;
  y: number;
}

export interface Stroke {
  id: number;
  type: MarkType;
  points: Point[];
}

export interface Note {
  id: number;
  type: MarkType;
  text: string;
}

export interface Journal {
  strokes: Stroke[];
  notes: Note[];
}

// Same three colours markup's own README documents: "red ! fix, blue ?
// question, green + new thing".
export const MARK_COLORS: Record<MarkType, string> = {
  f: "#c0392b",
  q: "#2f6fb0",
  n: "#2f8f4f",
};

export const MARK_LABELS: Record<MarkType, string> = {
  f: "fix",
  q: "question",
  n: "new",
};

export function emptyJournal(): Journal {
  return { strokes: [], notes: [] };
}
