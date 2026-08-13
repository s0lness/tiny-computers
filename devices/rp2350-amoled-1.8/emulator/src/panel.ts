// Blits the framebuffer to a canvas, one pushed rectangle at a time (never
// the whole panel per frame), so the emulator exercises the same
// partial-refresh path the firmware's own push function does. Which
// rectangles to draw comes from emu_push_* (see readPushes); this file only
// knows how to turn raw framebuffer bytes into canvas pixels once it has a
// rectangle to draw.
//
// Pixel format is whatever emu_device() declared (panel.format). Only
// "rgb565be" is implemented, because it is the only one any known firmware
// declares today; an unrecognised format throws rather than silently
// misrendering; see docs/decisions/0003-emulator-runs-the-real-apps.md for
// why "guess and hope" is the wrong instinct for this project generally.

import type { EmuExports } from "./wasm";

export interface PushRect {
  x: number;
  y: number;
  w: number;
  h: number;
}

export function readPushes(emu: EmuExports): PushRect[] {
  const count = emu.emu_push_count();
  const rects: PushRect[] = [];
  for (let i = 0; i < count; i++) {
    rects.push({ x: emu.emu_push_x(i), y: emu.emu_push_y(i), w: emu.emu_push_w(i), h: emu.emu_push_h(i) });
  }
  return rects;
}

// One pixel from raw framebuffer bytes to (r, g, b), 0..255 each.
export type PixelReader = (raw: number) => [number, number, number];

// Undoes the byte swap "rgb565be" declares: the framebuffer stores RGB565
// with its two bytes in the panel DMA's order, which on every real target
// (little-endian) is the opposite of how a uint16_t is stored. Reading it
// back through a Uint16Array gives the same swapped value the firmware
// computed (wasm linear memory and JS typed arrays are both little-endian),
// so this is the one place that swap gets undone, on purpose: what the page
// displays is the device's actual memory, not a tidied copy of it.
function rgb565be(raw: number): [number, number, number] {
  const v = ((raw & 0xff) << 8) | ((raw >> 8) & 0xff);
  const r5 = (v >> 11) & 0x1f;
  const g6 = (v >> 5) & 0x3f;
  const b5 = v & 0x1f;
  return [(r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)];
}

const PIXEL_READERS: Record<string, PixelReader> = {
  rgb565be,
};

export function pixelReaderFor(format: string): PixelReader {
  const reader = PIXEL_READERS[format];
  if (!reader) {
    throw new Error(
      `unsupported panel pixel format "${format}" (this emulator implements: ${Object.keys(PIXEL_READERS).join(", ")})`
    );
  }
  return reader;
}

// Blits one rectangle from the framebuffer (panelW*panelH raw uint16
// samples starting at fbPtr) onto ctx, at its own (x, y) in panel space.
export function blitRect(
  ctx: CanvasRenderingContext2D,
  memory: WebAssembly.Memory,
  fbPtr: number,
  panelW: number,
  reader: PixelReader,
  rect: PushRect
): void {
  const { x, y, w, h } = rect;
  if (w <= 0 || h <= 0) return;
  const img = ctx.createImageData(w, h);
  // Re-read memory.buffer fresh: WebAssembly.Memory.grow() can detach and
  // replace the underlying ArrayBuffer, so a cached view would go stale.
  const fb = new Uint16Array(memory.buffer, fbPtr, panelW * (y + h));
  let di = 0;
  for (let row = 0; row < h; row++) {
    const rowStart = (y + row) * panelW + x;
    for (let col = 0; col < w; col++) {
      const [r, g, b] = reader(fb[rowStart + col]!);
      img.data[di] = r;
      img.data[di + 1] = g;
      img.data[di + 2] = b;
      img.data[di + 3] = 255;
      di += 4;
    }
  }
  ctx.putImageData(img, x, y);
}

export function blitAll(
  ctx: CanvasRenderingContext2D,
  memory: WebAssembly.Memory,
  fbPtr: number,
  panelW: number,
  panelH: number,
  reader: PixelReader
): void {
  blitRect(ctx, memory, fbPtr, panelW, reader, { x: 0, y: 0, w: panelW, h: panelH });
}
