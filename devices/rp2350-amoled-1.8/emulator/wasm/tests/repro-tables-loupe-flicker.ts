// repro-tables-loupe-flicker: a held-still finger on the numpad must not
// make the loupe flicker (firmware/apps/tables.c's loupe_update()).
//
// THE BUG. The owner: "quand je reste enfoncé sur une touche pour la voir
// en surbrillance la pastille de surbrillance clignote et flicker, elle
// devrait être visible en entier" - holding a finger still on a key, the
// magnifier flickers instead of standing solidly visible.
//
// WHERE IT LIVED, measured before anything was changed (this project's own
// standing rule, red before green, applies to a mechanism and not only to
// an assertion - see AGENTS.md's "Regression tests"). Driving a held-still
// contact through the emulator and reading emu_push_count()/emu_push_x/y/
// w/h directly for each tick (the settled framebuffer alone cannot show a
// transient - decision 0003's own limit) showed loupe_update() pushing the
// SAME 80x80 rectangle TWICE, back to back, on every single tick, even
// with the touch coordinate completely unchanged: an unconditional erase
// (a white fill, pushed) followed by an unconditional redraw (pushed
// again), regardless of whether the box had moved or the hovered cell had
// changed. Two real DMA pushes a tick, the second painting over the
// first's blank erase, is exactly a flicker at the tick rate. Both pushed
// rectangles were a clean multiple of 8 wide, not truncated, which is what
// rules out the gfx.c row-width push defect another agent was chasing
// elsewhere in this tree at the same time (a photographed vertical-streak
// bug on the sketchpad's palette) - that one corrupts a single push's own
// pixels; this one was two clean pushes firing when zero were needed.
//
// THE FIX (loupe_update()'s own comment in tables.c has the full account):
// skip the tick entirely when nothing the bubble shows would change (same
// visibility, same box x, AND same hovered cell - position alone is not
// enough, because a finger can hold x still while sliding across a ROW
// boundary, changing the cell numpad_hit() names without moving the
// loupe's own box), and when it DOES need to redraw, erase and redraw the
// union of the old and new rectangles as ONE push, not two.
//
//   bun run emulator/wasm/build.ts
//   bun run emulator/wasm/tests/repro-tables-loupe-flicker.ts
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { PANEL_W, PANEL_H, cellCx, cellTouchCy, LOUPE_CY, LOUPE_BOX_H } from "../../../tools/tables-layout";

const WASM_PATH = join(import.meta.dir, "..", "dist", "emu.wasm");
const loupeYMin = LOUPE_CY - LOUPE_BOX_H / 2, loupeYMax = LOUPE_CY + LOUPE_BOX_H / 2;

let passCount = 0, failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
  if (ok) passCount++; else failCount++;
  console.log(`[${ok ? "PASS" : "FAIL"}] ${label}${detail ? " - " + detail : ""}`);
}

async function loadDevice() {
  let memory!: WebAssembly.Memory;
  const dec = new TextDecoder();
  const fwLog: string[] = [];
  const inst = await WebAssembly.instantiate(await WebAssembly.compile(readFileSync(WASM_PATH)), {
    env: {
      js_log(ptr: number, len: number) { fwLog.push(dec.decode(new Uint8Array(memory.buffer, ptr, len))); },
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
  memory = inst.exports.memory as WebAssembly.Memory;
  const e = inst.exports as any;
  if (e.emu_init() !== 1) throw new Error("emu_init() failed");
  const jsonBytes = new Uint8Array(memory.buffer, e.emu_device());
  let end = 0; while (jsonBytes[end] !== 0) end++;
  const apps: string[] = JSON.parse(dec.decode(jsonBytes.subarray(0, end))).apps || [];
  const APP_TABLES = apps.map((a) => a.toLowerCase()).indexOf("tables");
  if (APP_TABLES < 0) throw new Error("no tables app in this emu.wasm");
  e.emu_tick(0);
  e.emu_app_switch(APP_TABLES);
  let t = 16;
  e.emu_tick(t);
  fwLog.length = 0;
  return {
    t,
    tick(nowMs: number) { t = nowMs; e.emu_tick(nowMs); },
    touch(px: number, py: number) { e.emu_touch(1, px, py); },
    pushesThisTick(): { n: number; loupe: number } {
      const n = e.emu_push_count();
      let loupe = 0;
      for (let i = 0; i < n; i++) {
        const y = e.emu_push_y(i), h = e.emu_push_h(i), w = e.emu_push_w(i);
        // The loupe zone is otherwise blank (tables.c's own THE LOUPE
        // comment: "nothing but the loupe is ever drawn there") and its
        // box is 80x80, so a push overlapping that y-band at that width
        // can only be the loupe - the blinking caret is a narrow ~8px
        // column elsewhere, and no other element ever pushes here.
        if (y < loupeYMax && y + h > loupeYMin && w >= 40) loupe++;
      }
      return { n, loupe };
    },
  };
}

async function main() {
  console.log("=== repro: tables.c's loupe must not flicker under a held-still finger ===\n");

  const dev = await loadDevice();
  const px = Math.round(cellCx(7)), py = Math.round(cellTouchCy(7)); // digit 8

  // Arm (ARM_MS=40) and confirm (COMMIT_CONFIRM_MS=72) the hold - past both
  // before measuring, so the loupe has already settled once.
  let t = dev.t;
  for (let i = 0; i < 8; i++) { t += 16; dev.touch(px, py); dev.tick(t); }

  // ---- A: 30 ticks, IDENTICAL coordinate every time - the owner's own
  // scenario ("je reste enfoncé sur une touche"). The loupe must push
  // NOTHING across all 30, because nothing it shows has changed.
  let loupePushes = 0, totalLoupeTicks = 0;
  for (let i = 0; i < 30; i++) {
    t += 16;
    dev.touch(px, py);
    dev.tick(t);
    const { loupe } = dev.pushesThisTick();
    loupePushes += loupe;
    if (loupe > 0) totalLoupeTicks++;
  }
  check("the loupe pushes nothing across 30 ticks of a perfectly held-still finger",
    loupePushes === 0, `${loupePushes} loupe push(es) across ${totalLoupeTicks} tick(s)`);

  // ---- B: sliding to a DIFFERENT ROW in the SAME column (x unchanged,
  // only y moves) must still redraw the loupe with the new digit - the
  // exact case s->loupeCell exists to catch, since the box's own x never
  // moves in this scenario. digitCell(5) below is column-1 like digit 8
  // (same x), a different row.
  const px2 = Math.round(cellCx(4)), py2 = Math.round(cellTouchCy(4)); // digit 5, same x as digit 8, different row
  check("the row-change probe actually keeps x fixed and only moves y (or this test proves nothing)",
    px2 === px && py2 !== py, `px=${px} px2=${px2} py=${py} py2=${py2}`);
  let sawLoupeRedraw = false;
  for (let i = 0; i < 10 && !sawLoupeRedraw; i++) {
    t += 16;
    dev.touch(px2, py2);
    dev.tick(t);
    if (dev.pushesThisTick().loupe > 0) sawLoupeRedraw = true;
  }
  check("but the loupe DOES redraw once the hovered cell actually changes, even with x held still",
    sawLoupeRedraw);

  console.log(`\n${passCount} passed, ${failCount} failed`);
  if (failCount > 0) process.exit(1);
}

main();
