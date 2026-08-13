// Makes touch visible. Without this, a mouse click is a one-pixel event
// that tells you nothing about whether a real finger could actually hit
// what it's aiming at, and this device is a toy built for a young child, so
// that question matters more here than "does the pixel land".
//
// Draws two things, both on the shared overlay layer (never the
// framebuffer, see panel.ts / main.ts: the framebuffer is the device's own
// memory and nothing here may write to it):
//
//   - a translucent disc at the ACTUAL fingertip diameter, centred on the
//     point last reported to emu_touch (after touchsim, if it's on: a
//     dropout or a stray shows up here exactly as the firmware saw it,
//     which is the point), with the exact reported point marked inside it;
//   - a fading trail behind it, so a swipe is reviewable after the fact
//     and not just while it's happening.
//
// A bare hover (mouse over the panel, button up) draws a thin dashed ring
// instead of the filled disc: "down" and "merely hovering" must never look
// the same, or the overlay lies about what the firmware is receiving.

export interface ContactPreset {
  id: "adult" | "child";
  mm: number;
  label: string;
}

export const CONTACT_PRESETS: ContactPreset[] = [
  { id: "adult", mm: 8, label: "adult" },
  { id: "child", mm: 6, label: "child" },
];

// Fallback used ONLY when the device descriptor doesn't declare its own
// physical panel size. Derived from this project's own reference panel:
// 368x448 pixels over a 1.8" diagonal.
//   diagonal_px  = sqrt(368^2 + 448^2)      ~= 579.8
//   px_per_inch  = diagonal_px / 1.8        ~= 322.1
//   px_per_mm    = px_per_inch / 25.4       ~= 12.68
// Rounded to 12.7. This is a reasonable default for "a small AMOLED touch
// puck", not a claim about any other device; a firmware that cares about
// being measured accurately should declare its own physical size.
export const DEFAULT_PX_PER_MM = 12.7;

interface TrailPoint {
  x: number;
  y: number;
  t: number;
}

const TRAIL_FADE_MS = 1000;

export class TouchOverlay {
  contactMm: number;
  pxPerMm: number;

  private trail: TrailPoint[] = [];
  private down = false;
  private downX = 0;
  private downY = 0;
  private hoverX: number | null = null;
  private hoverY: number | null = null;

  constructor(contactMm = CONTACT_PRESETS[1]!.mm, pxPerMm = DEFAULT_PX_PER_MM) {
    this.contactMm = contactMm;
    this.pxPerMm = pxPerMm;
  }

  // Fed from the tick loop with whatever was actually handed to
  // emu_touch(), NOT raw pointer events: after touchsim, a dropout or a
  // stray belongs in this trail exactly as the firmware received it.
  recordTouch(down: boolean, x: number, y: number, nowMs: number): void {
    this.down = down;
    if (down) {
      this.downX = x;
      this.downY = y;
      this.trail.push({ x, y, t: nowMs });
      this.hoverX = null;
      this.hoverY = null;
    }
  }

  // Fed directly from raw pointer movement, only meaningful while not
  // down: there is no ABI concept of hover, this is purely a page-side aid
  // for lining up a press before making it.
  recordHover(x: number | null, y: number | null): void {
    if (this.down) return;
    this.hoverX = x;
    this.hoverY = y;
  }

  paint(ctx: CanvasRenderingContext2D, nowMs: number, color: string): void {
    this.trail = this.trail.filter((p) => nowMs - p.t < TRAIL_FADE_MS);
    ctx.lineWidth = 2;
    ctx.strokeStyle = color;
    for (let i = 1; i < this.trail.length; i++) {
      const a = this.trail[i - 1]!;
      const b = this.trail[i]!;
      const age = nowMs - b.t;
      ctx.globalAlpha = Math.max(0, 1 - age / TRAIL_FADE_MS) * 0.55;
      ctx.beginPath();
      ctx.moveTo(a.x, a.y);
      ctx.lineTo(b.x, b.y);
      ctx.stroke();
    }
    ctx.globalAlpha = 1;

    const radiusPx = (this.contactMm * this.pxPerMm) / 2;
    if (this.down) {
      ctx.beginPath();
      ctx.fillStyle = color;
      ctx.globalAlpha = 0.3;
      ctx.arc(this.downX, this.downY, radiusPx, 0, Math.PI * 2);
      ctx.fill();
      ctx.globalAlpha = 1;
      ctx.beginPath();
      ctx.fillStyle = color;
      ctx.arc(this.downX, this.downY, 2, 0, Math.PI * 2);
      ctx.fill();
    } else if (this.hoverX !== null && this.hoverY !== null) {
      ctx.beginPath();
      ctx.strokeStyle = color;
      ctx.globalAlpha = 0.7;
      ctx.lineWidth = 1;
      ctx.setLineDash([3, 3]);
      ctx.arc(this.hoverX, this.hoverY, radiusPx, 0, Math.PI * 2);
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.globalAlpha = 1;
    }
  }
}
