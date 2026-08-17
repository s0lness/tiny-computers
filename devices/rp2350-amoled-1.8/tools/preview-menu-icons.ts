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

// ---- menu.c's own layout, mirrored exactly (see that file's "THE LAYOUT"
// section: menu_rows, menu_row_span, cell_rect_land, ICON_W/ICON_H).
// The app count is read back from the device itself in main()
// (emu_menu_app_count(), the menu's OWN roster - docs/decisions/0019, not
// necessarily g_appCount any more), not hardcoded, so this script cannot
// silently drift from an app-table change - see main()'s own comment for the
// time it did exactly that.
//
// UPDATED 2026-08-15, when the menu became a grid: an icon's box is no longer
// at a fixed y, it is centred in its own cell, and a cell's row is decided by
// the app count. Cropping at the old fixed ICON_TOP_MARGIN=24 would have
// produced icons sliced 16px off-centre at four apps and pictures of white
// paper at twelve, which is the same "silently mislabelled capture" failure
// main()'s comment already records.
//
// UPDATED AGAIN 2026-08-17 (decision 0020): a cell can now be taller than
// MENU_CELL_FLOOR, and menu.c grows the icon drawn in it to match
// (menu_icon_size(), a bilinear resample of the native 96px render - see
// that function's own comment in menu.c). Cropping a fixed 96x96 box
// centred in a grown cell would slice the edges off exactly the icons this
// script exists to let someone judge, so CROP_W/CROP_H below are
// menuIconSize()'s own re-derivation, not the ICON_W/ICON_H constant.
const ICON_W = 96;
const ICON_H = 96;
const MENU_CELL_FLOOR = 112;
const MENU_COLS_MAX = 4;
const MENU_TOP_INSET = 10; // gfx.h PANEL_BEZEL_MARGIN_PX
const MENU_CANCEL_FLOOR = 22;
const MENU_AVAIL_H = LAND_H - MENU_TOP_INSET - MENU_CANCEL_FLOOR;
const MENU_ROWS_MAX = Math.floor(MENU_AVAIL_H / MENU_CELL_FLOOR);
const MENU_ICON_PAD = 8;

function menuRows(n: number): number {
  return Math.min(Math.max(Math.ceil(n / MENU_COLS_MAX), 1), MENU_ROWS_MAX);
}

function menuRowSpan(n: number, r: number): { first: number; cols: number } {
  const rows = menuRows(n);
  const base = Math.floor(n / rows);
  const extra = n % rows;
  return { first: r * base + Math.min(r, extra), cols: base + (r < extra ? 1 : 0) };
}

function menuCellH(n: number): number {
  const rows = menuRows(n);
  const byHeight = Math.floor(MENU_AVAIL_H / rows);
  let byWidth = LAND_W;
  for (let r = 0; r < rows; r++) byWidth = Math.min(byWidth, Math.floor(LAND_W / menuRowSpan(n, r).cols));
  let h = Math.min(byHeight, byWidth);
  h = Math.floor(h / 8) * 8;
  return Math.max(h, MENU_CELL_FLOOR);
}

function menuIconSize(cellW: number, cellH: number): number {
  return Math.max(ICON_W, Math.min(cellW, cellH) - 2 * MENU_ICON_PAD);
}

function cellRectLand(i: number, n: number): { bx: number; by: number; bw: number; bh: number } {
  const rows = menuRows(n);
  const cellH = menuCellH(n);
  for (let r = 0; r < rows; r++) {
    const { first, cols } = menuRowSpan(n, r);
    if (i < first + cols) {
      const c = i - first;
      const w = Math.floor(LAND_W / cols);
      const bx = c * w;
      return { bx, by: MENU_TOP_INSET + r * cellH, bw: c === cols - 1 ? LAND_W - bx : w, bh: cellH };
    }
  }
  throw new Error(`no cell for index ${i} at ${n} apps`);
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
    // The app names this firmware actually declares, from emu_device()'s own
    // JSON (emu_abi.h's "apps" key). See main()'s own comment for why this is
    // read rather than written down.
    appNames(): string[] {
      const ptr = exp.emu_device();
      const b = new Uint8Array(memory.buffer, ptr);
      let end = 0;
      while (b[end] !== 0) end++;
      const json = JSON.parse(decoder.decode(b.subarray(0, end)));
      return Array.isArray(json.apps) ? (json.apps as string[]) : [];
    },
    // The MENU's own roster (docs/decisions/0019), not g_appCount: a slot in
    // the grid names an index INTO appNames() above, and since the roster
    // cut that is no longer the identity mapping (slot 4 is "tables",
    // appNames()[10], not appNames()[4]).
    menuAppCount(): number { return exp.emu_menu_app_count(); },
    menuAppIndex(slot: number): number { return exp.emu_menu_app_index(slot); },
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

  // READ from the firmware, not written down here. This used to be a
  // hardcoded `n = 3` (with a comment two dozen lines up already claiming it
  // was read back from the device, which it was not), and the fourth app
  // (Connect Four, 2026-08-14) narrowed menu.c's columns from 149px to 112px:
  // every crop then came out of the wrong place, and the file called
  // "menu-icon-timer" was a picture of the Connect Four icon. A preview tool
  // that silently mislabels what it captured is worse than one that fails.
  //
  // The filenames keep this directory's existing vocabulary rather than the
  // app's own: the drawing app calls itself "draw" and its captures have
  // always been "menu-icon-sketch-*".
  // appNames() is the FULL app table (g_apps[]/g_appCount) - a name per
  // g_apps[] index. n and the per-slot index below come from the menu's own
  // roster (emu_menu_app_count()/emu_menu_app_index()) instead, since
  // docs/decisions/0019 the two counts differ: the grid shows five slots,
  // not all eleven apps the firmware carries, and slot i is not g_apps[i]
  // any more (slot 4 is "tables", g_apps[10]).
  const appNames = dev.appNames();
  const n = dev.menuAppCount();
  if (n === 0) throw new Error("emu_menu_app_count() returned 0 - cannot place the crops");
  for (let i = 0; i < n; i++) {
    const appIdx = dev.menuAppIndex(i);
    const rawName = appNames[appIdx];
    const name = rawName === "draw" ? "sketch" : rawName === "timer" && coilLabel ? "timer-coil" : rawName;

    const { bx, by, bw, bh } = cellRectLand(i, n);
    // The icon's actual rendered size (decision 0020) - not always ICON_W
    // any more. Cropping at a stale fixed 96 would slice a grown icon's
    // edges off, the exact "silently mislabelled capture" this comment
    // block's header already warns about.
    const size = menuIconSize(bw, bh);
    const iconX = bx + Math.floor((bw - size) / 2);
    const iconY = by + Math.floor((bh - size) / 2);
    const boxNative = crop(landGray, LAND_W, iconX, iconY, size, size);
    const box4x = upscale(boxNative, size, size, 4);

    // The filename CARRIES the size when it is not the old fixed 96, for
    // the same reason: a file called "menu-icon-tables-96.png" that is
    // actually a 128px crop is exactly the kind of mislabelling that has
    // already cost a wrong investigation once in this file's own history.
    const nativeLabel = size === ICON_W ? "96" : String(size);
    const pNative = join(ROOT, "preview", `menu-icon-${name}-${nativeLabel}.png`);
    const p4x = join(ROOT, "preview", `menu-icon-${name}-${nativeLabel}-4x.png`);
    writePng(pNative, size, size, boxNative);
    writePng(p4x, box4x.w, box4x.h, box4x.data);
    console.log(`wrote ${pNative} and ${p4x}`);
  }
}

main();
