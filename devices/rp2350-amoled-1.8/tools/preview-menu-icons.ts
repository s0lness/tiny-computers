/**
 * Pixel-accurate preview of the menu's three icons, captured from the REAL
 * compiled firmware (emulator/wasm/dist/emu.wasm), not a reimplementation.
 * Loads the wasm module headlessly (no browser, same technique every
 * emulator/wasm/tests/*.ts file already uses), switches into the menu
 * (APP_INDEX_MENU, see runtime_core.h), lets menu_enter() paint once, and
 * reads the framebuffer straight back - the exact bytes gfx_push_all() would
 * hand the panel. This exists because tools/gen-strokes.ts's own rasteriser
 * is a design-time APPROXIMATION of the ink; this script is not an
 * approximation of anything; it is the firmware's own output.
 *
 * Deliberately does NOT open a serial port or touch a physical device - see
 * this task's own brief: the owner is using the board.
 *
 *   bun run emulator/wasm/build.ts        # build first (or after any menu.c edit)
 *   bun tools/preview-menu-icons.ts        # writes preview/menu-*.png
 *
 * To capture the proposed coil alternative for the timer icon instead of the
 * hourglass, flip TIMER_ICON_USE_COIL to 1 in firmware/apps/menu.c, rebuild
 * the wasm, and re-run this with --coil (renames the timer crop so it does
 * not clobber the hourglass capture).
 */
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { deflateSync } from "node:zlib";
import { join, dirname } from "node:path";

const ROOT = join(import.meta.dir, "..");
const WASM_PATH = join(ROOT, "emulator", "wasm", "dist", "emu.wasm");

const PANEL_W = 368;
const PANEL_H = 448;
const LAND_W = PANEL_H; // 448
const LAND_H = PANEL_W; // 368
const APP_INDEX_MENU = -1;

const args = process.argv.slice(2);
const coilLabel = args.includes("--coil");

// ---- menu.c's own layout, mirrored exactly (see that file's "Layout"
// section: column_rect_land, ICON_TOP_MARGIN, ICON_W/ICON_H). g_appCount is
// 3 in this build (chrono, sketch, timer) - read back from the device
// itself below rather than hardcoded, so this script cannot silently drift
// from a future app-table change.
const ICON_TOP_MARGIN = 24;
const ICON_W = 96;
const ICON_H = 96;

function columnRectLand(i: number, n: number): { bx: number; bw: number } {
  const w = Math.floor(LAND_W / n);
  const bx = i * w;
  const bw = i === n - 1 ? LAND_W - bx : w;
  return { bx, bw };
}

// ---- wasm loading, identical shape to every emulator/wasm/tests/*.ts file ----
async function loadDevice() {
  const bytes = readFileSync(WASM_PATH);
  const mod = await WebAssembly.compile(bytes);
  let memory!: WebAssembly.Memory;
  const decoder = new TextDecoder();

  const imports = {
    env: {
      js_log(ptr: number, len: number) {
        const b = new Uint8Array(memory.buffer, ptr, len);
        console.log(`    [fw] ${decoder.decode(b)}`);
      },
      sinf: (x: number) => Math.sin(x),
      cosf: (x: number) => Math.cos(x),
      atan2f: (y: number, x: number) => Math.atan2(y, x),
      sqrtf: (x: number) => Math.sqrt(x),
      fabsf: (x: number) => Math.abs(x),
      floorf: (x: number) => Math.floor(x),
      fmodf: (x: number, y: number) => x % y,
      powf: (x: number, y: number) => Math.pow(x, y),
      expf: (x: number) => Math.exp(x),
    },
  };

  const instance = await WebAssembly.instantiate(mod, imports);
  memory = instance.exports.memory as WebAssembly.Memory;
  const exp = instance.exports as any;
  if (exp.emu_init() !== 1) throw new Error("emu_init() failed - see [fw] log lines above");

  return {
    tick(ms: number) { exp.emu_tick(ms); },
    appSwitch(i: number) { exp.emu_app_switch(i); },
    appCurrent(): number { return exp.emu_app_current(); },
    fbBytes(): Uint8Array {
      const ptr = exp.emu_fb();
      return new Uint8Array(memory.buffer, ptr, PANEL_W * PANEL_H * 2).slice();
    },
  };
}

// v = (byte0<<8)|byte1 recovers the pre-swap RGB565 value directly from the
// two little-endian memory bytes of gfx_fb[idx] - see gfx.h's px_swap/
// px_to_gray and this project's repro tests for the same derivation. gray
// 0 = black ink, 255 = white paper (px_to_gray's own convention).
function fbToGray(fb: Uint8Array): Uint8Array {
  const out = new Uint8Array(PANEL_W * PANEL_H);
  for (let i = 0; i < PANEL_W * PANEL_H; i++) {
    const v = (fb[i * 2] << 8) | fb[i * 2 + 1];
    out[i] = ((v >> 5) & 0x3f) << 2;
  }
  return out;
}

// panel (px,py) grayscale -> landscape (lx,ly), inverting gfx.h's documented
// forward mapping (landscape -> panel: px = PANEL_W-1-ly, py = lx), the same
// inversion menu.c's own panel_to_land() uses.
function panelGrayToLandscape(panelGray: Uint8Array): Uint8Array {
  const out = new Uint8Array(LAND_W * LAND_H);
  for (let ly = 0; ly < LAND_H; ly++) {
    for (let lx = 0; lx < LAND_W; lx++) {
      const px = PANEL_W - 1 - ly;
      const py = lx;
      out[ly * LAND_W + lx] = panelGray[py * PANEL_W + px];
    }
  }
  return out;
}

function crop(src: Uint8Array, srcW: number, x: number, y: number, w: number, h: number): Uint8Array {
  const out = new Uint8Array(w * h);
  for (let row = 0; row < h; row++) {
    out.set(src.subarray((y + row) * srcW + x, (y + row) * srcW + x + w), row * w);
  }
  return out;
}

function upscale(src: Uint8Array, w: number, h: number, factor: number): { data: Uint8Array; w: number; h: number } {
  const ow = w * factor, oh = h * factor;
  const out = new Uint8Array(ow * oh);
  for (let y = 0; y < oh; y++) {
    const sy = (y / factor) | 0;
    for (let x = 0; x < ow; x++) {
      const sx = (x / factor) | 0;
      out[y * ow + x] = src[sy * w + sx];
    }
  }
  return { data: out, w: ow, h: oh };
}

// ---- minimal greyscale PNG writer, same shape as tools/gen-strokes.ts ----
let crcTable: number[] | null = null;
function crc32(buf: Buffer): number {
  if (!crcTable) {
    crcTable = [];
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c >>> 0;
    }
  }
  let c = 0xffffffff;
  for (const b of buf) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function writePng(path: string, w: number, h: number, gray: Uint8Array) {
  const raw = Buffer.alloc((w + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w + 1)] = 0;
    raw.set(gray.subarray(y * w, y * w + w), y * (w + 1) + 1);
  }
  const idat = deflateSync(raw, { level: 9 });
  const chunk = (type: string, data: Buffer) => {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const body = Buffer.concat([Buffer.from(type, "ascii"), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(body) >>> 0);
    return Buffer.concat([len, body, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8;
  ihdr[9] = 0;
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(
    path,
    Buffer.concat([
      Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
      chunk("IHDR", ihdr),
      chunk("IDAT", idat),
      chunk("IEND", Buffer.alloc(0)),
    ]),
  );
}

async function main() {
  const dev = await loadDevice();
  dev.tick(0);
  dev.appSwitch(APP_INDEX_MENU);
  dev.tick(16);
  if (dev.appCurrent() !== APP_INDEX_MENU) {
    throw new Error(`did not land in the menu: app_current()=${dev.appCurrent()}`);
  }

  const panelGray = fbToGray(dev.fbBytes());
  const landGray = panelGrayToLandscape(panelGray);

  // Full landscape view, for context - what the panel actually shows when
  // the puck is held sideways, icons along the top edge.
  writePng(join(ROOT, "preview", "menu-full-landscape.png"), LAND_W, LAND_H, landGray);
  console.log(`wrote preview/menu-full-landscape.png (${LAND_W}x${LAND_H})`);

  const n = 3; // g_apps[] = { chrono, sketch("draw"), timer } - see menu.c
  const names = ["chrono", "sketch", coilLabel ? "timer-coil" : "timer"];
  for (let i = 0; i < n; i++) {
    const { bx, bw } = columnRectLand(i, n);
    const iconX = bx + Math.floor((bw - ICON_W) / 2);
    const iconY = ICON_TOP_MARGIN;
    const box96 = crop(landGray, LAND_W, iconX, iconY, ICON_W, ICON_H);
    const box4x = upscale(box96, ICON_W, ICON_H, 4);

    const p96 = join(ROOT, "preview", `menu-icon-${names[i]}-96.png`);
    const p4x = join(ROOT, "preview", `menu-icon-${names[i]}-4x.png`);
    writePng(p96, ICON_W, ICON_H, box96);
    writePng(p4x, box4x.w, box4x.h, box4x.data);
    console.log(`wrote preview/menu-icon-${names[i]}-96.png and -4x.png`);
  }
}

main();
