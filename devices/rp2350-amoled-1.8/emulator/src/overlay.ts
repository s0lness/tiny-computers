// Draws the rectangles emu_push_* reported as fading outlines on a
// transparent canvas stacked over the panel. This is the single highest
// value feature in this emulator (see emu_abi.h): a partial-refresh bug is
// a bug about window geometry, and it is invisible unless the windows
// themselves are visible. Never touches the framebuffer; purely a
// debugging aid painted on top of it.

import type { PushRect } from "./panel";
import { PUSH_FADE_MS } from "./constants";

interface TrackedRect extends PushRect {
  bornAt: number;
}

export class PushOverlay {
  private rects: TrackedRect[] = [];
  enabled = true;
  lastCount = 0;
  lastWidth = 0;

  record(rects: PushRect[], nowMs: number): void {
    if (rects.length > 0) {
      this.lastCount = rects.length;
      this.lastWidth = rects[rects.length - 1]!.w;
    }
    if (!this.enabled) return;
    for (const r of rects) this.rects.push({ ...r, bornAt: nowMs });
  }

  // color: any valid CSS color string, read once from the design system's
  // --accent so this stays in the family-budget terracotta rather than a
  // hardcoded hex. Does not clear the canvas: this and TouchOverlay share
  // one overlay layer, and main.ts's frame loop clears it once per frame
  // before both paint.
  paint(ctx: CanvasRenderingContext2D, nowMs: number, color: string): void {
    if (!this.enabled) {
      this.rects.length = 0;
      return;
    }
    this.rects = this.rects.filter((r) => nowMs - r.bornAt < PUSH_FADE_MS);
    ctx.strokeStyle = color;
    ctx.lineWidth = 1;
    for (const r of this.rects) {
      const age = nowMs - r.bornAt;
      ctx.globalAlpha = Math.max(0, 1 - age / PUSH_FADE_MS);
      ctx.strokeRect(r.x + 0.5, r.y + 0.5, Math.max(r.w - 1, 0), Math.max(r.h - 1, 0));
    }
    ctx.globalAlpha = 1;
  }
}
