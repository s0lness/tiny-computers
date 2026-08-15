// Does the gate actually fail?
//
// This project's own decision 0004 is about instruments that reported
// health they had not verified, and a checker whose rules all pass is
// indistinguishable from a checker whose rules never fire. So every rule
// gets fed something it must reject, before the gate is allowed to say
// anything about the firmware. It runs in microseconds and is not
// optional: run.ts calls it first, every time.
//
// It tests the PREDICATES, not the firmware. Proving that a real app can
// be made to violate a rule would mean shipping a broken app to test
// against, which is the one thing this whole exercise exists to avoid.

import { KNOWN, partition } from "./known";
import type { Violation } from "./rules";
import { checkArenaHeadroom, checkBezel, checkPushBudget, checkPushGeometry, findChangesOutside, PushCoverage, pxToGray } from "./rules";
import {
  checkAppCount, checkAppHygiene, checkShimPurity, checkTestAppConstants, checkTouchSimSeeding,
  countUnseededTouchSim, diffAbiSurface, diffAppWiring, stripC,
} from "./contracts";
import { LIMITS, PANEL_W, PANEL_H } from "./rules/rp2350-amoled-1.8";

export interface SelftestResult { name: string; ok: boolean; detail: string }

function paper(): Uint16Array {
  return new Uint16Array(PANEL_W * PANEL_H).fill(0xffff);
}

export function selftest(): SelftestResult[] {
  const out: SelftestResult[] = [];
  const t = (name: string, ok: boolean, detail = "") => out.push({ name, ok, detail });

  /* push-geometry */
  t("push-geometry rejects a row length that is not a multiple of 8",
    checkPushGeometry([{ x: 0, y: 0, w: 12, h: 10 }], LIMITS).length === 1);
  t("push-geometry rejects a window that runs off the panel",
    checkPushGeometry([{ x: PANEL_W - 8, y: 0, w: 16, h: 10 }], LIMITS).length === 1);
  t("push-geometry rejects an empty window",
    checkPushGeometry([{ x: 0, y: 0, w: 0, h: 0 }], LIMITS).length === 1);
  t("push-geometry accepts what gfx_push actually produces",
    checkPushGeometry([{ x: 8, y: 4, w: 64, h: 40 }], LIMITS).length === 0);

  /* push-budget */
  const full = { x: 0, y: 0, w: PANEL_W, h: PANEL_H };
  t("push-budget accepts one full panel in a tick",
    checkPushBudget([full], PANEL_W * PANEL_H, LIMITS).length === 0);
  t("push-budget rejects the palette bug's shape (many full panels in one tick)",
    checkPushBudget(Array(8).fill(full), 8 * PANEL_W * PANEL_H, LIMITS).length > 0);
  t("push-budget rejects a tick that issues more pushes than the recorder can hold",
    checkPushBudget(Array(64).fill({ x: 0, y: 0, w: 8, h: 8 }), 64 * 64, LIMITS).some((v) => /pushes in one tick/.test(v.detail)));

  /* pushed-or-invisible */
  {
    const prev = paper();
    const cur = paper();
    cur[107 * PANEL_W + 5] = 0x0000; // one pixel changed at (5,107)
    const cover = new PushCoverage(PANEL_W, PANEL_H);
    cover.paint([{ x: 6, y: 4, w: 360, h: 438 }]); // the real palette push
    const miss = findChangesOutside(prev, cur, cover, PANEL_W, PANEL_H);
    t("pushed-or-invisible catches a pixel one column left of the pushed window",
      miss.count === 1 && miss.first?.x === 5 && miss.first?.y === 107,
      `${miss.count} outside`);
    cover.paint([{ x: 0, y: 0, w: PANEL_W, h: PANEL_H }]);
    t("pushed-or-invisible accepts the same pixel under a full-panel push",
      findChangesOutside(prev, cur, cover, PANEL_W, PANEL_H).count === 0);
  }

  /* bezel */
  {
    const fb = paper();
    t("bezel accepts a blank panel", checkBezel(fb, LIMITS, "selftest").length === 0);
    fb[3 * PANEL_W + 3] = 0x0000; // ink 3px from the corner
    t("bezel catches one ink pixel inside the band", checkBezel(fb, LIMITS, "selftest").length === 1);
    const fb2 = paper();
    fb2[(LIMITS.bezelMarginPx + 1) * PANEL_W + LIMITS.bezelMarginPx + 1] = 0x0000;
    t("bezel accepts ink one pixel past the band", checkBezel(fb2, LIMITS, "selftest").length === 0);
  }

  /* pixel format: the rule reads the framebuffer the way gfx.h does */
  t("pxToGray maps paper to 252 and ink to 0", pxToGray(0xffff) === 252 && pxToGray(0x0000) === 0,
    `${pxToGray(0xffff)} / ${pxToGray(0x0000)}`);

  /* arena-headroom */
  t("arena-headroom rejects an app at 92% of the arena",
    checkArenaHeadroom("selftest", 60300, 65536, 0.9).length === 1);
  t("arena-headroom accepts an app at 81% of the arena",
    checkArenaHeadroom("selftest", 53000, 65536, 0.9).length === 0);
  t("arena-headroom rejects a capacity oracle that read zero",
    checkArenaHeadroom("selftest", 0, 0, 0.9).length === 1);

  /* app-wiring: the three copies of the app table */
  {
    const base = {
      defined: new Map([["chrono", "chrono.c"], ["menu", "menu.c"]]),
      table: [{ token: "chrono", gatedBy: null }],
      wasmAppSources: ["chrono.c", "menu.c", "stubapps.c"],
      boardAppSources: ["chrono.c", "menu.c"],
    };
    t("app-wiring accepts a consistent three-copy table", diffAppWiring(base).length === 0);
    t("app-wiring rejects an app defined but dropped from g_apps[]",
      diffAppWiring({ ...base, defined: new Map([...base.defined, ["level", "level.c"]]) })
        .some((v) => /level\.c defines g_levelApp/.test(v.detail)));
    t("app-wiring rejects an app the board build does not compile",
      diffAppWiring({ ...base, boardAppSources: ["menu.c"] })
        .some((v) => /CMakeLists\.txt does not compile it/.test(v.detail)));
    t("app-wiring rejects an app the wasm build does not compile",
      diffAppWiring({ ...base, wasmAppSources: ["menu.c", "stubapps.c"] })
        .some((v) => /build\.ts SOURCES does not compile it/.test(v.detail)));
  }

  /* abi-parity: the three copies of the emulator ABI */
  {
    const ok = { header: ["emu_tick"], exportsList: ["emu_tick"], module: ["emu_tick"] };
    t("abi-parity accepts three agreeing copies", diffAbiSurface(ok).length === 0);
    t("abi-parity rejects an export missing from emu_abi.h (the emu_sensor_vec3 shape)",
      diffAbiSurface({ header: ["emu_tick"], exportsList: ["emu_tick", "emu_sensor_vec3"], module: ["emu_tick", "emu_sensor_vec3"] })
        .some((v) => /not declared in emu_abi\.h/.test(v.detail)));
    t("abi-parity rejects a declaration never exported",
      diffAbiSurface({ header: ["emu_tick", "emu_arena_used"], exportsList: ["emu_tick"], module: ["emu_tick"] })
        .some((v) => /missing from build\.ts EMU_EXPORTS/.test(v.detail)));
    t("abi-parity rejects a module that disagrees with the tree",
      diffAbiSurface({ header: ["emu_tick"], exportsList: ["emu_tick"], module: ["emu_tick", "emu_old_thing"] })
        .some((v) => /module exports emu_old_thing/.test(v.detail)));
  }

  /* shim-purity: the exact include the level branch shipped */
  t("shim-purity rejects emu_shim.c including an app's header",
    checkShimPurity(`#include "level.h"\n`, ["level.h", "menu.h"]).length === 1);
  t("shim-purity rejects emu_shim.c reaching for g_levelApp",
    checkShimPurity(`extern const app_t g_levelApp;\n`, []).length === 1);
  t("shim-purity accepts a shim that includes only runtime headers",
    checkShimPurity(`#include "sensors.h"\n#include "gfx.h"\n`, ["menu.h"]).length === 0);

  /* app-hygiene: apps read signals, never chips */
  t("app-hygiene rejects an app reading the IMU directly",
    checkAppHygiene("level.c", stripC(`void f(void){ QMI8658_read_acc_xyz(a); }`)).length === 1);
  t("app-hygiene rejects an app writing flash",
    checkAppHygiene("dino.c", stripC(`void save(void){ flash_range_program(o, d, n); }`)).length === 1);
  t("app-hygiene does not accuse a comment",
    checkAppHygiene("ok.c", stripC(`/* core1 owns i2c1 and QMI8658_init lives there */ int x;`)).length === 0);

  /* test-app-index: constants against the firmware's own app list */
  {
    const apps = ["chrono", "draw", "timer", "four"];
    t("test-app-index accepts APP_DRAW = 1",
      checkTestAppConstants("t.ts", "const APP_DRAW = 1;", apps).length === 0);
    t("test-app-index rejects APP_DRAW = 2 (an inserted app reordered the table)",
      checkTestAppConstants("t.ts", "const APP_DRAW = 2;", apps).length === 1);
    t("test-app-index rejects a declared app pinned to an out-of-range index",
      checkTestAppConstants("t.ts", "const APP_FOUR = 9;", apps).length === 1);
    t("test-app-index rejects an undeclared name squatting on a live index",
      checkTestAppConstants("t.ts", "const APP_DINO = 2;", apps).length === 1);
    t("test-app-index ignores a non-index APP_* constant",
      checkTestAppConstants("t.ts", "const APP_ARENA_BYTES = 65536;", apps).length === 0);
  }

  /* test-determinism: unseeded TouchSim */
  t("countUnseededTouchSim counts a three-argument construction",
    countUnseededTouchSim(`const sim = new TouchSim(profile, PANEL_W, PANEL_H);`) === 1);
  t("countUnseededTouchSim accepts a seeded construction",
    countUnseededTouchSim(`const sim = new TouchSim(profile, w, h, seededRng(7));`) === 0);
  t("countUnseededTouchSim is not fooled by a trailing comma (repro-timer-coil's shape)",
    countUnseededTouchSim(`const sim = new TouchSim(\n  { ...D, dropoutsEnabled: true },\n  PANEL_W,\n  PANEL_H,\n);`) === 1);
  t("test-determinism rejects a new unseeded file",
    checkTouchSimSeeding([{ name: "feature-new.ts", src: `new TouchSim(p, 1, 2)` }], {}).length === 1);
  t("test-determinism rejects a stale baseline entry",
    checkTouchSimSeeding([{ name: "a.ts", src: `new TouchSim(p, 1, 2, rng)` }], { "a.ts": "old reason" }).length === 1);

  /* app-count-ceiling */
  t("app-count-ceiling accepts twelve", checkAppCount(12, 12).length === 0);
  t("app-count-ceiling rejects thirteen", checkAppCount(13, 12).length === 1);

  /* the baseline cannot swallow something it does not name */
  {
    const v: Violation = { rule: "bezel", why: "", see: "", detail: "chrono/tap centre: 4 ink pixels" };
    t("a known-findings entry for four does not swallow the same rule in chrono",
      partition([v]).fresh.length === 1);
    t("every known finding names who decides", KNOWN.every((k) => k.decides.length > 0));
  }

  return out;
}

if (import.meta.main) {
  const results = selftest();
  for (const r of results) console.log(`${r.ok ? "PASS" : "FAIL"}  ${r.name}${r.detail ? ` - ${r.detail}` : ""}`);
  const failed = results.filter((r) => !r.ok).length;
  console.log(`\n${results.length - failed} passed, ${failed} failed`);
  if (failed > 0) process.exit(1);
}
