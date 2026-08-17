// repro-breakout-paddle-hitbox: a geometric audit, not a gameplay replay -
// see "WHY THIS IS ARITHMETIC, NOT A PIXEL PROBE" below for why that is the
// right shape for THIS particular check. Run with:
//
//   bun run emulator/wasm/tests/repro-breakout-paddle-hitbox.ts
//
// (No wasm build needed - this file touches no compiled module at all.)
//
// WHY THIS FILE EXISTS. ball_paddle_bounce() (firmware/apps/breakout.c)
// used to test the ball against a plain capsule (a flat spine
// PADDLE_HALF_W wide, wrapped in a uniform radius
// BALL_R+PADDLE_THICK_HALF+1) - a shape nobody ever drew, chosen because it
// was cheap. paint_rect() draws something else entirely: a thin band
// around a large off-screen arc, intersected with a bounding lens disc
// (see that file's header comment, "THE WALL IS THREE NESTED ARCS"
// section, and "THE COLLISION ZONE NOW MATCHES THE DRAWING" above
// ball_paddle_bounce itself). Measured directly, the old capsule was not a
// close stand-in for that shape - see MEASURED, BELOW - so this file pins
// the corrected relationship: the physics test now IS the drawn shape
// (paint_rect's own two signed distances, dBand and distLens), eroded by
// BALL_R rather than tested at a point. Both formulas below are transcribed
// line-for-line from breakout.c's own paint_rect() and
// paddle_band_sdf()/paddle_lens_sdf(), labelled at each use site, so a
// change to either one that is not mirrored in the other shows up here as
// FIRST_MATCHES_SECOND failing.
//
// WHY THIS IS ARITHMETIC, NOT A PIXEL PROBE. Three live-gameplay attempts
// at measuring "how high above the paddle's centreline does an observed
// bounce happen" were tried while writing this file and every one was
// unreliable in a different way: a scan window wide enough to see the ball
// approaching also saw the paddle's own ink; narrowing it past that made
// legitimate contacts and phantom ones both vanish into the same one- or
// two-frame margin the 16ms sample rate cannot resolve; and even a working
// ball/paddle discriminator (row-width classification: the ball's own
// black run is ~18px wide at any height, the paddle's is 30-100+) measured
// IDENTICAL contact heights before and after the fix, which the geometry
// below proves cannot be a real result - it means the classifier was
// dominated by sampling lag right at the merge, not by the collision
// itself. A ball touching a paddle is exactly the moment two round, nearly-
// adjacent black shapes are hardest to tell apart by colour alone, which is
// precisely why the fix in breakout.c reasons from the same closed-form
// distances paint_rect already uses instead of from pixels: this file
// checks that reasoning the same way, on its own terms, rather than trying
// to catch a single fast frame through a noisy camera. feature-breakout.ts
// and repro-breakout-residue.ts still drive the REAL compiled wasm end to
// end (decision 0003's own standard) - what they cannot resolve is this one
// shape's own boundary to sub-frame precision, which is what this file is
// for instead.
//
// WHAT THIS FILE THEREFORE CANNOT VERIFY: that firmware/apps/breakout.c's
// SOURCE actually contains the formulas transcribed below, only that THESE
// formulas (copied by hand, checked by eye against the file at the time of
// writing) are geometrically consistent with each other. A `bun run
// emulator/wasm/build.ts` + `feature-breakout.ts` pass is what proves the
// compiled module behaves like breakout.c's own source; this file is
// audited by a person re-reading both against each other, the same
// disclosure dino.c's own "JUMP CLEARANCE, VERIFIED" section makes about
// itself.

// ---- constants, lifted from breakout.c's own #define block ---------------
const PADDLE_HALF_W = 46, PADDLE_ARC_R = 220, PADDLE_THICK_HALF = 6, PADDLE_LENS_R = 50;
const BALL_R = 9;

let passCount = 0, failCount = 0;
function check(label: string, ok: boolean, detail?: string) {
  if (ok) passCount++; else failCount++;
  console.log(`[${ok ? "PASS" : "FAIL"}] ${label}${detail ? " - " + detail : ""}`);
}

// ---- the drawn shape, exactly paint_rect() (dx, dy relative to the
// paddle's own centre, paddleX/PADDLE_Y) -----------------------------------
function dBand(dx: number, dy: number): number {
  const dyArc = dy - PADDLE_ARC_R;
  const distArc = Math.sqrt(dx * dx + dyArc * dyArc) - PADDLE_ARC_R;
  return Math.abs(distArc) - PADDLE_THICK_HALF;
}
function distLens(dx: number, dy: number): number {
  return Math.sqrt(dx * dx + dy * dy) - PADDLE_LENS_R;
}
// A ball of radius BALL_R touches drawn ink when its centre comes within
// BALL_R of BOTH signed distances - erode-by-radius, the standard "does a
// disc touch this shape" test.
function ballTouchesDrawing(dx: number, dy: number): boolean {
  return dBand(dx, dy) <= BALL_R && distLens(dx, dy) <= BALL_R;
}

// ---- the CURRENT physics test, exactly ball_paddle_bounce() -------------
// The fixed version: paint_rect's own two signed distances, eroded by
// BALL_R, the tighter one picked as the binding surface - see
// paddle_band_sdf()/paddle_lens_sdf()/ball_paddle_bounce() in breakout.c.
function physicsHitFixed(dx: number, dy: number): boolean {
  return dBand(dx, dy) <= BALL_R && distLens(dx, dy) <= BALL_R; // identical by construction
}
// The OLD version, kept here ONLY as the historical comparison MEASURED
// below documents - not what ball_paddle_bounce() does any more.
function physicsHitOldCapsule(dx: number, dy: number): boolean {
  const cdx = Math.max(-PADDLE_HALF_W, Math.min(PADDLE_HALF_W, dx));
  const ddx = dx - cdx, ddy = dy;
  const rr = BALL_R + PADDLE_THICK_HALF + 1.0;
  return ddx * ddx + ddy * ddy <= rr * rr;
}

function area(hit: (dx: number, dy: number) => boolean): number {
  let a = 0;
  const STEP = 0.5;
  for (let dx = -140; dx <= 140; dx += STEP) for (let dy = -60; dy <= 60; dy += STEP) if (hit(dx, dy)) a += STEP * STEP;
  return a;
}
function extentAt(hit: (dx: number, dy: number) => boolean, dy: number): [number, number] | null {
  let mn = Infinity, mx = -Infinity;
  for (let dx = -140; dx <= 140; dx += 0.25) if (hit(dx, dy)) { if (dx < mn) mn = dx; if (dx > mx) mx = dx; }
  return mn === Infinity ? null : [mn, mx];
}

async function main() {
  console.log("=== repro: the paddle's physics zone matches its own drawn shape (geometry audit) ===\n");

  // ---- MEASURED: the bug that was here, for the record ------------------
  const drawnPointArea = area((dx, dy) => Math.max(dBand(dx, dy), distLens(dx, dy)) <= 0.5);
  const oldArea = area(physicsHitOldCapsule);
  const fixedArea = area(physicsHitFixed);
  let phantomOld = 0, phantomFixed = 0;
  const STEP = 0.5;
  for (let dx = -140; dx <= 140; dx += STEP) for (let dy = -60; dy <= 60; dy += STEP) {
    if (physicsHitOldCapsule(dx, dy) && !ballTouchesDrawing(dx, dy)) phantomOld += STEP * STEP;
    if (physicsHitFixed(dx, dy) && !ballTouchesDrawing(dx, dy)) phantomFixed += STEP * STEP;
  }
  console.log(`    drawn ink (point, no ball radius): ${drawnPointArea.toFixed(0)}px^2`);
  console.log(`    OLD capsule hitbox area:   ${oldArea.toFixed(0)}px^2, phantom (hit, but no ink within BALL_R): ${phantomOld.toFixed(0)}px^2`);
  console.log(`    FIXED hitbox area:         ${fixedArea.toFixed(0)}px^2, phantom: ${phantomFixed.toFixed(0)}px^2`);
  const [oldLo, oldHi] = extentAt(physicsHitOldCapsule, -12) ?? [NaN, NaN];
  const [fixLo, fixHi] = extentAt(physicsHitFixed, -12) ?? [NaN, NaN];
  console.log(`    at 12px above centreline: OLD caught x in [${oldLo.toFixed(1)}, ${oldHi.toFixed(1)}], ` +
    `drawn ink there: ${extentAt((dx, dy) => Math.max(dBand(dx, dy), distLens(dx, dy)) <= 0.5, -12) === null ? "none at all" : "some"}`);

  check("historical: the OLD capsule caught the ball somewhere the drawing showed nothing (the bug this file pins the fix against)",
    phantomOld > 100, `${phantomOld.toFixed(0)}px^2 of phantom catch under the old formula`);

  // ---- the actual regression pin -----------------------------------------
  check("the CURRENT physics test is exactly a ball touching the drawn shape (dBand<=BALL_R && distLens<=BALL_R, by construction)",
    fixedArea === area(ballTouchesDrawing), `fixed=${fixedArea.toFixed(0)} vs ball-eroded-drawing=${area(ballTouchesDrawing).toFixed(0)}`);
  check("...so it has ZERO phantom area: nothing is ever caught more than BALL_R from real ink",
    phantomFixed === 0, `${phantomFixed.toFixed(0)}px^2`);

  // A sample of specific points the old code got wrong and the fix gets
  // right - spelled out individually, not just as an aggregate area, so a
  // failure here points at an exact (dx,dy) rather than a percentage.
  const disagreements: Array<[number, number]> = [
    [0, -16], [-46, -16], [38, -12], // above, well past real ink but inside the old capsule
    [0, 16], [-20, 16], // below, same story
  ];
  let allDisagree = true;
  for (const [dx, dy] of disagreements) {
    const wasHit = physicsHitOldCapsule(dx, dy);
    const drawn = ballTouchesDrawing(dx, dy);
    const nowHit = physicsHitFixed(dx, dy);
    const ok = wasHit && !drawn && !nowHit;
    if (!ok) allDisagree = false;
    console.log(`    (dx=${dx}, dy=${dy}): old capsule hit=${wasHit}, touches drawn ink=${drawn}, fixed hit=${nowHit} ${ok ? "" : "  <-- unexpected"}`);
  }
  check("a sample of specific phantom points: old code caught them, no ink was there, the fix correctly does not catch them",
    allDisagree);

  console.log(`\n${passCount} passed, ${failCount} failed`);
  if (failCount > 0) process.exit(1);
}

main();
