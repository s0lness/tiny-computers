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
import { checkBezel, checkPushBudget, checkPushGeometry, findChangesOutside, PushCoverage, pxToGray } from "./rules";
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
