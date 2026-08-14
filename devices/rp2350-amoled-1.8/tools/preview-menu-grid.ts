/**
 * Pixel-accurate capture of the WHOLE menu screen, from the real compiled
 * firmware (emulator/wasm/dist/emu.wasm), at whatever app count that
 * firmware declares. Two pictures per run: the menu at rest, and the menu
 * with one cell lit under a thumb, because the halo is the entire feedback
 * story of the press-drag-release gesture and a still picture of the grid
 * without it says nothing about how a child knows what she is about to get.
 *
 *   ZIG_EXE=... bun run emulator/wasm/build.ts
 *   bun tools/preview-menu-grid.ts                 # writes preview/menu-grid-<n>.png
 *
 * To see the layouts this device does not have apps for yet, build with the
 * stub fixture first (firmware/apps/stubapps.c):
 *
 *   EMU_EXTRA_DEFINES=-DMENU_STUB_APPS=6  bun run emulator/wasm/build.ts
 *   EMU_EXTRA_DEFINES=-DMENU_STUB_APPS=12 bun run emulator/wasm/build.ts
 *
 * WHAT IS AND IS NOT AN INSTRUMENT HERE (docs/decisions/0010). The pixels
 * come from the firmware's own framebuffer, so this reads downstream of
 * menu.c and cannot agree with a layout bug in it. The layout table it
 * PRINTS is re-derived here from the app count, so that one is upstream and
 * proves nothing on its own - it is there to be read next to the picture,
 * and the picture is the evidence. Nothing here opens a serial port or
 * touches the board.
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

// menu.c's own constants. Re-derived, see the header comment.
const MENU_CELL_H = 112;
const MENU_COLS_MAX = 4;
const MENU_ROWS_MAX = Math.floor(LAND_H / MENU_CELL_H);

function menuRows(n: number): number {
  return Math.min(Math.max(Math.ceil(n / MENU_COLS_MAX), 1), MENU_ROWS_MAX);
}
function rowSpan(n: number, r: number): { first: number; cols: number } {
  const rows = menuRows(n);
  const base = Math.floor(n / rows);
  const extra = n % rows;
  return { first: r * base + Math.min(r, extra), cols: base + (r < extra ? 1 : 0) };
}
function cellRect(n: number, i: number): { bx: number; by: number; bw: number; bh: number } {
  const rows = menuRows(n);
  for (let r = 0; r < rows; r++) {
    const { first, cols } = rowSpan(n, r);
    if (i < first + cols) {
      const c = i - first;
      const w = Math.floor(LAND_W / cols);
      const bx = c * w;
      return { bx, by: r * MENU_CELL_H, bw: c === cols - 1 ? LAND_W - bx : w, bh: MENU_CELL_H };
    }
  }
  throw new Error(`no cell for index ${i} at ${n} apps`);
}

async function loadDevice() {
  const mod = await WebAssembly.compile(readFileSync(WASM_PATH));
  let memory!: WebAssembly.Memory;
  const decoder = new TextDecoder();
  const instance = await WebAssembly.instantiate(mod, {
    env: {
      js_log(ptr: number, len: number) {
        console.log(`    [fw] ${decoder.decode(new Uint8Array(memory.buffer, ptr, len)).trim()}`);
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
  });
  memory = instance.exports.memory as WebAssembly.Memory;
  const exp = instance.exports as any;
  if (exp.emu_init() !== 1) throw new Error("emu_init() failed");
  return {
    tick(ms: number) { exp.emu_tick(ms); },
    appSwitch(i: number) { exp.emu_app_switch(i); },
    appCurrent(): number { return exp.emu_app_current(); },
    // Landscape in, panel out: gfx.h's (lx,ly) -> (PANEL_W-1-ly, lx).
    touchLand(down: boolean, lx: number, ly: number) {
      exp.emu_touch(down ? 1 : 0, PANEL_W - 1 - Math.round(ly), Math.round(lx));
    },
    fbBytes(): Uint8Array {
      return new Uint8Array(memory.buffer, exp.emu_fb(), PANEL_W * PANEL_H * 2).slice();
    },
    appNames(): string[] {
      const b = new Uint8Array(memory.buffer, exp.emu_device());
      let end = 0;
      while (b[end] !== 0) end++;
      const json = JSON.parse(decoder.decode(b.subarray(0, end)));
      return Array.isArray(json.apps) ? (json.apps as string[]) : [];
    },
  };
}

// v = (byte0<<8)|byte1 recovers the pre-swap RGB565 value; gray 0 = ink,
// 255 = paper (gfx.h's px_to_gray convention).
function fbToGray(fb: Uint8Array): Uint8Array {
  const out = new Uint8Array(PANEL_W * PANEL_H);
  for (let i = 0; i < PANEL_W * PANEL_H; i++) {
    const v = (fb[i * 2]! << 8) | fb[i * 2 + 1]!;
    out[i] = ((v >> 5) & 0x3f) << 2;
  }
  return out;
}
function panelGrayToLandscape(panelGray: Uint8Array): Uint8Array {
  const out = new Uint8Array(LAND_W * LAND_H);
  for (let ly = 0; ly < LAND_H; ly++) {
    for (let lx = 0; lx < LAND_W; lx++) {
      out[ly * LAND_W + lx] = panelGray[lx * PANEL_W + (PANEL_W - 1 - ly)]!;
    }
  }
  return out;
}

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
  for (const b of buf) c = crcTable[(c ^ b) & 0xff]! ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
function writePng(path: string, w: number, h: number, gray: Uint8Array) {
  const raw = Buffer.alloc((w + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w + 1)] = 0;
    raw.set(gray.subarray(y * w, y * w + w), y * (w + 1) + 1);
  }
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
  writeFileSync(path, Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr), chunk("IDAT", raw.length ? deflateSync(raw, { level: 9 }) : Buffer.alloc(0)),
    chunk("IEND", Buffer.alloc(0)),
  ]));
}

async function capture(litCell: number | null): Promise<{ gray: Uint8Array; n: number }> {
  const dev = await loadDevice();
  dev.tick(0);
  dev.appSwitch(APP_INDEX_MENU);
  dev.tick(16);
  if (dev.appCurrent() !== APP_INDEX_MENU) throw new Error("did not land in the menu");
  const n = dev.appNames().length;

  if (litCell !== null) {
    const { bx, by, bw, bh } = cellRect(n, litCell);
    const cx = bx + Math.floor(bw / 2), cy = by + Math.floor(bh / 2);
    // Long enough to arm (MENU_ARM_SAMPLES/MENU_ARM_MS) and then to confirm
    // the hover (MENU_HOVER_CONFIRM_MS). Held down: the capture is of the
    // moment BEFORE the release, which is the moment the whole design is
    // about.
    for (let t = 100, i = 0; i < 40; i++, t += 15) { dev.touchLand(true, cx, cy); dev.tick(t); }
  }
  return { gray: panelGrayToLandscape(fbToGray(dev.fbBytes())), n };
}

async function main() {
  const rest = await capture(null);
  const n = rest.n;
  const rows = menuRows(n);
  const band = LAND_H - rows * MENU_CELL_H;

  console.log(`${n} apps: ${rows} row(s), cancel band ${band}px`);
  for (let r = 0; r < rows; r++) {
    const { first, cols } = rowSpan(n, r);
    const { bw } = cellRect(n, first);
    console.log(`  row ${r}: ${cols} cell(s), ${bw} x ${MENU_CELL_H} px` +
      `  (${(bw / 75).toFixed(1)} x ${(MENU_CELL_H / 75).toFixed(1)} child fingertips)`);
  }

  const restPath = join(ROOT, "preview", `menu-grid-${n}.png`);
  writePng(restPath, LAND_W, LAND_H, rest.gray);
  console.log(`wrote ${restPath}`);

  // The lit cell: the last one, so the capture also shows the bottom-right
  // corner of the grid actually being reachable.
  const lit = await capture(n - 1);
  const litPath = join(ROOT, "preview", `menu-grid-${n}-lit.png`);
  writePng(litPath, LAND_W, LAND_H, lit.gray);
  console.log(`wrote ${litPath}`);

  // Cheap correctness check on the capture itself, so a run that produced a
  // blank or unchanged picture says so instead of writing a file nobody
  // looks at twice: the lit capture must differ from the resting one, and it
  // must differ INSIDE the lit cell.
  const { bx, by, bw, bh } = cellRect(n, n - 1);
  let diffInside = 0, diffOutside = 0;
  for (let ly = 0; ly < LAND_H; ly++) {
    for (let lx = 0; lx < LAND_W; lx++) {
      if (rest.gray[ly * LAND_W + lx] === lit.gray[ly * LAND_W + lx]) continue;
      const inside = lx >= bx && lx < bx + bw && ly >= by && ly < by + bh;
      if (inside) diffInside++; else diffOutside++;
    }
  }
  console.log(`halo: ${diffInside} px changed inside the lit cell, ${diffOutside} outside it`);
  if (diffInside === 0) { console.error("FAIL: nothing lit up"); process.exit(1); }
  if (diffOutside !== 0) { console.error("FAIL: the halo painted outside its own cell"); process.exit(1); }
}

main();
