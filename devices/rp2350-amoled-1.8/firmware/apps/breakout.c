// breakout: a tilted paddle for a two-year-old to bump a self-bouncing ball
// with, whacking a black-and-white wall of round bricks it also bounces
// around by itself - now with three lives, because the owner asked for them
// after living with the no-fail-state version for a while. "Casse brique",
// the owner's own name for it.
//
// =============================================================================
// WHO THIS IS FOR, THE ORIGINAL CALL, AND WHY IT IS NOW OVERRULED
// =============================================================================
//
// The original brief named the constraint directly: a two-year-old cannot
// aim, so a game where the ball is LOST through imprecision is abandoned
// inside a minute. Real breakout's whole tension is exactly that failure -
// miss the paddle, lose the ball, the game ends - which made real breakout
// precisely the wrong game for this hand. So the floor used to be a WALL,
// not a goal: all four edges of the play field bounced the ball, there was
// no life to lose and no game over, and the paddle was "a big, friendly
// obstacle she can slide under the ball with a tilt of her hand" rather than
// a single point of failure. That reasoning was not wrong when it was
// written, and it is not wrong in the abstract now either.
//
// IT IS OVERRULED HERE BECAUSE THE OWNER ASKED FOR IT, EXPLICITLY: three
// lives, losing the ball costs one, zero lives is game over, and game over
// has to be startable again. That is his call to make about his own
// daughter's toy, and the brief this time is not "avoid a fail state" but
// "build one, and make it read clearly." What changed in the code, plainly:
// the bottom edge (PLAY_B) stopped being a fourth bouncing wall and became
// the one edge that costs a life instead (see "THREE LIVES, LOSING THE
// BALL, AND GAME OVER" below for the whole design). Nothing about the
// original worry - a game a small child can lose through simple
// imprecision is a game she can be shut out of - was wrong; it is simply a
// cost the owner has decided is worth paying now, the same way real
// breakout's own tension is worth it for an audience that can aim. The
// paddle, the wall's three nested arcs, the black-and-white ink, and the
// fixed-timestep clock underneath all of it are unchanged and still argued
// for below exactly as they were.
//
// =============================================================================
// CONTROL: TILT FOR PLAY, AND ONE NARROW EXCEPTION FOR THE RESTART
// =============================================================================
//
// The owner asked for tilt games as their own category, separate from touch
// games, so tilt is this app's whole PLAY input - the same call
// firmware/apps/level.c already made (decision 0012) for the same reason:
// tilting the puck to move something inside it is a gesture a toddler
// already has, and it needs no glass free to make. While a game is actually
// live, this app reads NO touch at all, the same way tiltball.c does;
// frame.touchDown/X/Y are never looked at. A touch-driven paddle was
// considered and dropped: a thumb on the glass while the other hand tilts
// the puck is an awkward, two-handed ask of exactly the hand that is small
// here, and it would also mean this app needs the measured-dropout
// touch-gesture testing every touch-driven app on this device carries (see
// AGENTS.md's "A feature driven by touch needs BOTH kinds of file") for a
// control surface that adds nothing tilt does not already give for free.
//
// THE ONE EXCEPTION, added alongside the lives feature: once the game is
// over (LP_GAMEOVER_WAIT below), a tap anywhere restarts it - see "THREE
// LIVES" for why. That is the ONLY moment this file ever reads
// f->touchDown, and it is guarded to that one state; a stray touch during
// play, or during the brief freeze after a life is lost, is never
// consulted (exactly dino.c's own "tapped is deliberately not consulted"
// rule during its death freeze, reused here for the same reason - too soon
// to mean anything). So the sentence above ("while a game is actually live,
// this app reads NO touch") is still literally true; the new touch read
// belongs to the one screen that is not a live game.
//
// The paddle's X position is a straight, unsmoothed map of
// app_frame_t.tilt.gx (firmware/runtime/tilt.h) onto the play field's width,
// clamped at the edges (see PADDLE_GX_FULL below for the tilt angle that
// saturates it). No second filter is added on top: tilt.h is emphatic that
// there is exactly ONE filtered signal for every app to share, and a
// per-app smoothing layer on top of it is the "written twice" seam decision
// 0012 exists to prevent. That turns out to answer the coasting question
// for free, which is worth spelling out rather than leaving as an accident:
//
// WHAT THE PADDLE DOES WHILE `coasting`. tilt.c's own magnitude trust gate
// HOLDS the filtered gx/gy/gz at their last believed value whenever the
// puck is being carried (the measured vector stops being gravity, so the
// filter stops tracking it - see tilt.h's FILTERING section). Because this
// app reads gx directly and does nothing further to it, the paddle inherits
// that hold automatically: it FREEZES in place while coasting, and resumes
// tracking smoothly the instant trust returns, because the value it is
// reading did not jump in the meantime - only real tilting after that does.
// A "decay toward centre while coasting" policy was considered and
// rejected: it would be exactly the second, private filter tilt.h forbids,
// it would walk the paddle away from wherever the ball actually is while
// she is carrying the puck, and it would then have to visibly snap back to
// the real reading the moment trust returns - the lurch the brief explicitly
// warns against. Freezing costs no extra code, agrees with the one shared
// filter, and is never wrong in a way a jump could be. The paddle keeps
// doing exactly this - tracking tilt, freezing while coasting - straight
// through a life-lost freeze and through the whole time the game sits over
// at LP_GAMEOVER_WAIT: nothing about losing costs her the one piece of
// continuous, honest feedback this device gives her, and there is no ball
// there to hit regardless, so it does no harm.
//
// =============================================================================
// THE WALL IS THREE NESTED ARCS, NOT A GRID - DECISION 0009's TARGET, HIT
// HEAD ON, THEN HIT AGAIN ONCE THE OWNER ASKED FOR THREE ROWS
// =============================================================================
//
// "A wall of rectangular bricks is the single most ruler-shaped thing in all
// of games" - decision 0009 forbids hard edges, right angles and dead-straight
// lines, and a rectangular brick grid is built from nothing else. Three
// straight rows would be exactly that grid with the columns erased, which is
// why there is still no grid here after the owner asked for "3 rows of balls
// in to clear": eighteen round bricks, six per row, sit on THREE CONCENTRIC
// ARCS sharing one centre (ARC_CX, ARC_CY) and three different radii
// (ROW_PEAK_Y/ROW below - each row's radius is just ARC_CY minus that row's
// topmost point), the same sinf/cosf placement the single-arc version used,
// run three times at three radii instead of once. Nested arcs read as
// concentric rainbow bands - "three rows to clear" to an eye that cannot read
// a word of it - precisely because each row is still a curve, never a
// straight shelf: a grid announces itself by columns lining up vertically
// AND rows lining up horizontally at the same right angle everywhere, and a
// family of arcs sharing one centre has neither - every brick's neighbour in
// its own row curves away from it, and the row above it curves away too, at
// a DIFFERENT rate (a different radius), so no two edges in the whole wall
// are ever parallel the way a grid's are. Nothing about the arrangement is a
// right angle, and nothing about an individual brick is either: it is a
// circle, anti-aliased by coverage exactly like the sketchpad's own ink and
// the bubble level's dial.
//
// The paddle follows the same rule for the same reason. A flat bar with
// square ends is the paddle-shaped version of the ruler the wall above
// avoids, so it is drawn as the intersection of two circles instead: a thick
// BAND around a large, off-screen arc (which gives the gentle upward bow -
// PADDLE_ARC_R/PADDLE_THICK_HALF below) intersected with a bounding disc
// centred on the paddle itself (which rounds both ends into a lens rather
// than a stamped-off vertical cut - PADDLE_LENS_R below). Both halves of
// that intersection are already-analytic circle tests, so the paddle costs
// two sqrtf calls per pixel and no lookup table, and it reads as one bowed,
// rounded stroke rather than a bar with two shapes stuck together.
//
// The ball is the only shape with no arrangement to speak of: a plain,
// SOLID disc - the one filled shape in the whole picture, except for the
// lives markers below, which are deliberately drawn in the very same
// vocabulary (see "ONE INK").
//
// =============================================================================
// ONE INK: A RING VS A SOLID DISC, NOT A HUE
// =============================================================================
//
// The owner's brief was explicit: "breakout should be black & white". The
// ten-brick version used a palette (a different colour per brick) to make a
// wall of identical discs read as separate, distinguishable objects, and to
// make it visibly thin out as it broke. Both of those jobs still have to get
// done with one ink, so the split moved from HUE to SHAPE: the ball stays
// the one filled, SOLID black disc on the whole screen; every brick is now a
// black RING (an annulus, the same primitive dino.c's record ripple already
// draws) instead of a filled circle. A solid blob and a thin drawn outline
// are unmistakable apart at a glance even in pure black on white - the same
// distinction AGENTS.md's own icon set leans on between a filled shape and a
// stroked one - so the ball can never be mistaken for a brick, or a brick
// for the ball, with zero colour spent on the question.
//
// Bricks are told apart from EACH OTHER the way the ball is told apart from
// them: not by hue, but by shape, position and size. Position does most of
// the work for free - eighteen rings at eighteen different centres are
// eighteen different objects the instant two of them are more than a
// ring's-width apart, which the arc spacing already guarantees (see
// ball_bricks_bounce's own radius sum - bricks never overlap by
// construction). Size carries the row: each row's ring is drawn at a
// different band thickness (ROW_BAND_HALF below, thin outward and thicker
// toward the paddle), so even a single row seen in isolation - all its own
// rings the same radius, evenly spaced along one curve - reads as one
// coherent BAND, and the three bands read as three visibly different
// weights of line rather than three arbitrary repeats of the same mark.
// "Clearing the wall" still thins the picture out exactly the way the old
// palette did: a broken ring's ink is just gone, the same subtraction as a
// broken coloured disc's ink used to be.
//
// What stays black-and-only-black, never a ring, is the ball and the
// paddle - the two things that MOVE, the "pen" of this app the way the
// sketchpad's own ink is - so the picture keeps a second, orthogonal cue
// (filled vs stroked) between what acts and what gets acted on, on top of
// the cue (position/size) that tells the acted-on things apart from each
// other. The two new shapes this file draws for the lives feature keep the
// same grammar rather than inventing a third: each life is a small SOLID
// disc (see "THREE LIVES" below on why - it is deliberately drawn as a
// miniature ball, the one thing she can lose), and the ripple that marks a
// lost ball is a RING, the same annulus a brick is, because it is a mark
// left behind by an event, not an actor.
//
// =============================================================================
// THE GAME LOOP: ONE FIXED-TIMESTEP CLOCK, NOT A DTMS EULER STEP
// =============================================================================
//
// Decision 0010 flagged this exact trap before any game existed to fall into
// it: "today's tick rate is 'as fast as the loop spins'... a game tuned in
// the emulator plays at a different speed on the board, and at a different
// speed depending on how much it drew last frame", and asked for "one
// fixed-timestep accumulator helper... that all three games share." This was
// the first game built after that was written, so this file is where that
// helper was built, in the shape decision 0010 asked for; the lives feature
// added below stays inside that same discipline rather than sidestepping it
// - see "THREE LIVES" for how the freeze and the game-over wait are timed
// off the same simMs clock as everything else, not off f->nowMs directly.
//
// The mechanism: `s->simMs` is the absolute simulated-time-so-far, advanced
// in fixed FIXED_DT_MS quanta, catching up to `f->nowMs` at the START of
// every tick():
//
//     while (s->simMs + FIXED_DT_MS <= f->nowMs) {
//         s->simMs += FIXED_DT_MS;
//         sim_step(s, FIXED_DT_MS, s->simMs);
//     }
//
// This is deliberately NOT "integrate by f->dtMs once per tick": that would
// make the physics step size depend on how often tick() happens to be
// called, which is exactly the "speed must not depend on push cost" trap.
// Stepping in a FIXED quantum instead means the number of physics steps
// taken between any two points in real time is a pure function of the
// ELAPSED TIME between them - floor((nowMs - startMs) / FIXED_DT_MS) - and
// does not care how that elapsed time was chopped into individual tick()
// calls. That property is not just tidy, it is load-bearing: the gate's own
// `settles-the-same` counterfactual (tools/gate/exercise.ts) runs the exact
// same wall-clock window once as three coarse ticks and once at 60Hz, and
// demands the two arrive at an identical framebuffer. A per-call Euler
// integration would fail that by construction (a 233ms step and forty
// 16ms steps do not accumulate rounding or collision timing the same way);
// a fixed-quantum accumulator run to the same absolute nowMs produces the
// identical sequence of steps either way, because integer millisecond
// addition is exact and associative regardless of how the calls are
// grouped. Rendering happens exactly once per tick(), after the catch-up
// loop empties, per app.h's own contract ("the app draws what changed and
// pushes only that") - the loop can run several physics steps inside one
// tick without that costing more than one paint-and-push pass.
//
// A generous but real ceiling (BACKLOG_CAP_MS) drops backlog past 5 real
// seconds rather than let a genuinely pathological gap (a debugger paused on
// a breakpoint, say) force thousands of catch-up steps in one tick; nothing
// in normal play or in the gate's own stimuli comes close to it (the
// longest single real-time jump anything here produces is under two
// seconds - tools/gate/rules/rp2350-amoled-1.8.ts's PROBE_HOLD_MS +
// PROBE_WINDOW_MS), so it is a safety net, not a tuned parameter.
//
// =============================================================================
// SPEED, AND CLEARING THE WALL
// =============================================================================
//
// The ball's speed is a function of how many bricks are left, not of time:
// it starts at BALL_SPEED_BASE and rises toward BALL_SPEED_MAX as bricks
// break, capped there. That makes a session feel more alive as the wall
// thins without ever letting the ball outrun what a two-year-old can follow
// with her eyes, and - because it is driven by a discrete brick count rather
// than a clock - it stays exactly as deterministic as everything else here.
// A ball respawned after a life is lost resumes at whatever pace the
// CURRENT brick count implies (rescale_ball_speed(s) is called again right
// after the respawn), not reset to base - see "THREE LIVES" for why losing
// a life is not allowed to touch the wall at all, speed included.
//
// Clearing the last brick is celebrated without a word: the wall goes empty
// for a beat (CELEB_PAUSE_MS, the ball frozen so nothing is happening in two
// places on screen at once), and then the bricks pop back in ONE AT A TIME
// in a travelling wave along the same arc they live on
// (CELEB_STAGGER_MS apart, each growing from nothing over CELEB_GROW_MS with
// an ease-out "pop"), after which the ball resumes at its base speed. The
// celebration and the respawn are the same event: there is no separate
// "new wall" moment to announce, because the regrow wave already reads as
// one - the wall coming back to life is the reward, and it asks nothing of
// her to see it. Starting a fresh game after a game over REUSES this exact
// wave rather than inventing a second "the wall is back" moment - see
// "THREE LIVES" below.
//
// =============================================================================
// THREE LIVES, LOSING THE BALL, AND GAME OVER
// =============================================================================
//
// WHAT LOSING THE BALL LOOKS LIKE. The bottom edge (PLAY_B) used to be a
// fourth bouncing wall; it is now the one edge that does not bounce
// (ball_wall_bounce no longer touches it at all - the check moved into
// sim_step, right after the other three walls have had their turn). The
// instant the ball's own bottom edge crosses it, the ball VANISHES - drawn
// as nothing at all the next frame, not slowed, not shrunk - and a black
// RING blooms in its place over LOST_RIPPLE_DURATION_MS (the same annulus
// shape, the same duration, as dino.c's own record ripple and tiltball.c's
// own capture ripple: this device already has a wordless "something just
// happened here" mark, and losing a ball is exactly that kind of moment, so
// it is reused rather than reinvented). The ring is anchored at a FIXED
// screen position (RIPPLE_CX/CY, just above the paddle's own rest line),
// not at wherever the ball actually crossed - the same call dino.c's own
// ripple makes and argues for in its own header comment: "something just
// happened here" is a screen-space event to mark, not a world coordinate to
// remember, and a fixed anchor means the ring always appears where her eye
// already is (right where the paddle lives), not off wherever the ball
// happened to drift that particular time. The whole scene then holds still
// for LIFE_LOST_FREEZE_MS (550ms, dino.c's own DEATH_FREEZE_MS number and
// reasoning: a picture that was constantly moving suddenly not moving at
// all is itself the news) before anything else happens. Both durations are
// timed off s->simMs, the same deterministic clock every other animation in
// this file uses (see compute_visible_brick_radii's own comment on why),
// not off wall-clock reads.
//
// WHERE THE LIVES LIVE, AND HOW A CHILD WHO CANNOT READ KNOWS WHAT THEY
// MEAN. Three small SOLID discs (LIFE_DOT_*), tucked in the top-left corner
// at (LIFE_DOT_X0 + i*LIFE_DOT_GAP, LIFE_DOT_Y) - a region the wall's own
// arcs never reach (the nearest brick at that height sits well past x=76,
// see the geometry check below) and comfortably clear of
// PANEL_BEZEL_MARGIN_PX (gfx.h). They are drawn SOLID, not as rings, on
// purpose: the owner's own visual-over-textual taste in this repo (three
// marks that disappear, not the text "3") is met most directly by drawing
// each life as a miniature version of the one thing she can actually lose -
// a tiny ball - so "one fewer mark" and "one fewer chance for the ball"
// read as the same fact rather than an arbitrary counter that happens to
// start at three. Losing a life erases one dot; nothing else about the
// picture needs to change for the count to be legible, because paint_rect
// recomputes the whole lives row from s->drawnLives every time it repaints
// that rectangle (the same "nothing is patched, everything is recomputed"
// discipline the ball, paddle and every brick already use), so a missing
// dot is just paper where ink used to be, not something erased by hand.
//
// GEOMETRY CHECK, done once here rather than trusted by eye: row 0 (the
// outermost, highest arc) reaches its highest point at its own centre
// brick, (ARC_CX, ROW0_PEAK_Y) = (224, 55); its bricks move DOWN (larger y,
// further from the lives row) as they move toward either edge, reaching
// (ARC_CX +/- 155.7, ~105.6) at the arc's own ends. The lives row sits at
// y=LIFE_DOT_Y=38 (PLAY_T+12), x in [32, 84] (LIFE_DOT_X0 to
// LIFE_DOT_X0+2*LIFE_DOT_GAP, +/- the dot radius) - entirely inside the
// triangular gap between the play field's own top-left corner and where the
// wall's arc actually begins, with the nearest brick ink more than 40px
// below the lowest life dot. Nothing here overlaps.
//
// WHAT HAPPENS TO THE BRICKS ON A LIFE LOST: KEPT, ALWAYS, EVEN THE LIFE
// THAT ENDS THE GAME. A life lost never touches s->bricks at all - only the
// discrete, deliberate act of starting a FRESH game (the tap from game
// over, see below) ever resets the wall. The argument: real progress
// against the wall is the one thing she is actually building across a
// session, and a life lost is already announced clearly by the ring and the
// freeze - making her rebuild the wall too, on top of that, would be a
// second, larger punishment stacked on the first one for the same single
// mistake, and would also fight the game's own "the toy is never off and
// never waiting on her" ethos (a wall that keeps thinning as she plays,
// life or no life, is what makes a whole session feel like it is going
// somewhere). If lives remain after the freeze, reset_ball_and_paddle()
// drops a fresh ball back in at the exact same deterministic start position
// enter() itself uses, and rescale_ball_speed() sets its pace from
// whatever aliveCount currently is - the wall's progress, unchanged, is
// literally what decides how fast the next ball moves.
//
// GAME OVER, AND HOW SHE STARTS AGAIN. At zero lives the freeze does not
// end in a respawn; it ends in LP_GAMEOVER_WAIT, and the picture just... sits
// there. No ball (still invisible), a paddle that keeps gently answering her
// tilt (see "CONTROL" above on why that is left on), and the wall exactly
// as she left it. This follows the same idiom bowling.c's own PH_READY and
// dino.c's own RS_DEAD_WAIT already use for "nothing is happening right
// now, and that absence of movement IS the message" - dino.c's header
// comment even names the mechanism directly ("a scene that was constantly
// moving suddenly not moving at all is itself the news"). The restart
// gesture is dino.c's own answer too, copied rather than re-derived: A TAP,
// ANYWHERE ON THE GLASS - dino.c's tap_tick() arming/release-grace state
// machine (TAP_ARM_SAMPLES/TAP_ARM_MS/TAP_ARM_RATE_HZ/TAP_RELEASE_GRACE_MS,
// the same numbers, themselves copied from four.c) reused verbatim, because
// this is the exact same hardware doing the exact same dropout-hostile
// thing, and "one physical press, however long held through however many
// dropouts, fires exactly one action" is not an argument this file has
// anything to add to. No target to find, no button to locate - the device's
// own "the target the whole panel is legible from" idiom, applied to the
// one gesture the terminal screen needs. Firing it resets lives to
// START_LIVES, drops a fresh ball and paddle in, and - if the wall was not
// already whole (aliveCount < N_BRICKS, the ordinary case, since a game
// over almost never lands exactly on a full wall) - sets s->celebrating
// true, which is the SAME regrow wave a mid-game wall clear already
// triggers. A "fresh game" is answered with the same "the wall comes back"
// vocabulary a "fresh wall" already is, rather than a second animation
// invented to say the same thing. Was BOOT considered instead, the way
// bowling.c uses it to abandon a stuck rack? No: four.c's own header
// comment (section 8) calls a BOOT press "an adult's" affordance, not a
// wordless one a two-year-old is expected to find on her own - the tap is
// the toddler-facing answer here, the same way it already is in dino.c.
//
// WHY THIS EXCEPTION TO "READS NO TOUCH" IS SAFE TO MAKE NARROWLY. Touch is
// only ever consulted while s->lifePhase == LP_GAMEOVER_WAIT
// (breakout_tick's own guard clears s->tap whenever that is not the current
// phase, the same way four.c clears its own gesture state whenever the
// screen it belongs to is not showing); it is never read during LP_PLAYING
// or LP_LOST_FREEZE. So "tilt is this app's whole input while a game is
// live" - the sentence the CONTROL section above still opens with - remains
// literally true; the one thing that changed is that this app now HAS a
// screen that is not a live game, and that screen behaves like every other
// terminal screen on this device.
//
// =============================================================================
// DIFFICULTY: WHAT WAS ACTUALLY MEASURED, AND WHAT CHANGED
// =============================================================================
//
// THREE LIVES landed with nothing retuned for the new fail state, and it
// showed. An earlier, informal probe reported an "idealised" ball-tracking
// controller losing all three lives in about 11 seconds and named the
// shared tilt filter's own settling lag as the likely cause, without
// testing it. tools/breakout-difficulty.ts (committed, seeded, re-runnable)
// is that test, run properly.
//
// WHAT IT FOUND, AGAINST THE REAL COMPILED GAME, NOT BY REASONING ABOUT THE
// NUMBERS. A genuinely idealised controller - perfect ball position every
// tick (read straight off the pushed-rectangle stream, the same channel a
// person watching the emulator's own dirty-rect overlay would see), one
// frame of latency, proportional tilt through the real, UNMODIFIED tilt.c
// filter - does not lose a single life at either the old or the new ball
// speed below; it clears all eighteen bricks cleanly every time. That rules
// the shared filter's lag OUT as the binding constraint for anyone who can
// actually aim: lead-compensating the same controller (commanding tilt from
// where the ball WILL be, cancelling the filter's own measured ~100-150ms
// settling time) does not turn a loss into a win, because there was no loss
// left to turn. The earlier 11-second figure, re-measured here as a
// sanity check, turns out to match what THIS probe gets from a controller
// that applies NO correction at all - a paddle left dead centre the whole
// game - to within a couple of frames. So the filter's lag was never what
// made this hard; a two-year-old's own hand is, and nothing in this tree
// can watch one directly (decision 0003) - which is why the tuning below is
// aimed at a MODELLED sloppy hand, not a perfect one. See
// tools/breakout-difficulty.ts's own header for the full method.
//
// PLAYABILITY, DEFINED BEFORE IT WAS TUNED TO (restated briefly here; the
// full reasoning lives with the tool that measures it):
//
//   An idealised controller should clear the wall comfortably - it already
//   did, at the old speed too, so this was never the thing being tuned.
//
//   A SLOPPY controller - modelled with a 350ms reaction delay, a 220ms
//   voluntary-correction cadence, +/-40px of aim error redrawn fresh each
//   correction, and a hand that cannot snap from rail to rail (0.8g of tilt
//   in 500ms) - none of these are measurements, all of them are named
//   assumptions about a small child's hand that nothing here can watch -
//   should survive a median of at least 20 seconds across many seeded
//   trials, with a median of at least 4 of 18 bricks broken: long enough
//   that a few real tilts visibly mattered, short enough that losing still
//   actually happens, since three lives and a game over were the owner's
//   own explicit ask (see "IT IS OVERRULED HERE" at the top of this file).
//
// WHAT ACTUALLY BOUND THE SLOPPY CONTROLLER, MEASURED: the ball's own speed,
// stacked against that controller's own latency, not the paddle's width or
// its travel range (both measured, both cleared - PADDLE_TRAVEL_MAX*2 alone
// already spans the full field, and an idealised paddle glued to the ball's
// exact x essentially never misses). The innermost brick row (closest to
// the paddle) can put the ball on a beeline for the paddle with as little as
// 530-990ms of warning at the OLD speed range - and the sloppy model's own
// reaction delay plus one correction cadence can already spend 350-570ms of
// that before a single correction lands, before the shared filter or the
// hand's own slew rate cost anything further. Measured median survival at
// the old speed (0.16-0.30px/ms): 17.9s across 20 seeds, under the 20s
// target. BALL_SPEED_BASE/MAX are now 0.15/0.28px/ms - a 6-7% cut, not a
// rewrite - and the same 20 seeds now measure a median survival of 24.8s
// (min 18.0s, max 38.1s) and a median of 12/18 bricks broken. The idealised
// controller is, if anything, easier now (a slower ball is strictly more
// forgiving of perfect play too), so this one change moved the sloppy
// number without costing the ceiling anything.
//
// =============================================================================
// PER-FRAME COST AND RESIDUE, THE WAY EVERY OTHER APP HERE PROVES IT
// =============================================================================
//
// Nothing here erases anything. Exactly like firmware/apps/tiltball.c,
// paint_rect() recomputes every pixel of a dirty rectangle from the CURRENT
// model (ball, paddle, every brick's current radius, the lives row, the
// ripple) rather than patching around an old shape, so an old position
// cannot survive a repaint of the ground it stood on - residue is
// impossible by construction, not by bookkeeping, and it is the same reason
// tiltball.c gives for the same technique. The dirty rectangle is the union of
// whatever actually changed this tick (the ball's old and new box - or its
// visibility toggling off/on, which counts as a change even when the
// position did not move -, the paddle's old and new box, each brick whose
// drawn radius changed, the lives row whenever the count changes, and the
// ripple whenever it is animating), consolidated into at most EIGHT pushes
// now (was six, before the lives row and the ripple joined the list of
// things that can change in the same tick); if more than that changes at
// once (still only reachable during the wall's own regrow wave, when
// several bricks can cross their own grow schedule inside a single coarse
// tick) the fallback is one big union rect instead - still correct, just
// less surgical, and still nowhere near the panel's per-tick budget. See
// "MEASURED COST" in the task report for the actual numbers
// `bun run tools/gate/run.ts --measure` prints for this app.
//
// =============================================================================
// WHAT THIS FILE CANNOT VERIFY, SAID BEFORE THE CODE PER DECISION 0010
// =============================================================================
//
// The axis mapping. This app reads f->tilt.gx exactly as published and never
// touches a device axis, so if firmware/runtime/tilt.c's device-to-panel
// mapping is ever found to be flipped or transposed on real hardware (it is
// still a documented HYPOTHESIS - see tilt.h), the fix is upstream in tilt.c
// and this file changes nothing. What no test in this tree can confirm is
// whether "gx > 0" really is "tilt the puck's right edge down" once the axis
// ritual is run on the board; only that ritual can.
//
// Whether the 150ms filter feels immediate enough in a two-year-old's actual
// hand for THIS use (swinging a paddle, not levelling a dial) - tilt.h says
// plainly its constants have not been judged by a hand yet, and a game
// asking for quicker paddle response is a harder test of that lag than the
// bubble level ever was.
//
// Whether eighteen thin black rings on three nested arcs read as "three rows
// to clear" or as "some circles" from across a room to a toddler - decision
// 0009's own rule about whether a picture is any good is checked by no tool
// in this tree; the contact sheet (preview/breakout-*.png,
// tools/preview-breakout.ts) is where that question has to be judged, by an
// eye, the same as every icon on this device already is.
//
// WHETHER THREE LIVES IS THE RIGHT NUMBER, AND WHETHER A TAP READS AS
// "TRY AGAIN" RATHER THAN AS NOTHING IN PARTICULAR. Nothing in this tree can
// measure whether losing the ball three times in a row before a fresh start
// is a fair, legible arc for THIS child, on THIS device, at THIS ball
// speed - that is a feel question, decision 0003's forbidden kind, same as
// every timing constant elsewhere in this file. Nor can anything here
// confirm the tap-anywhere gesture is actually discovered unprompted on the
// game-over screen the way it already has been on dino's - dino's own
// restart reuses a gesture she is already mid-session on (the same tap that
// jumps); this app's restart is the FIRST touch gesture this app has ever
// asked of her, arriving only once tilt has already failed her three times.
// Whether that is still findable, or needs a nudge (a soft pulse on the
// paddle, say), is exactly the kind of thing only watching a real two-year-
// old's hands can answer.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "app.h"
#include "gfx.h"

#define BREAKOUT_PI 3.14159265358979323846f // -std=c11 has no M_PI; same reason
                                             // level.c and menu.c each carry their own.

/* ---------------------------------------------------------------------
 * Geometry, LANDSCAPE coordinates (448 wide x 368 tall, LAND_W x LAND_H).
 * Held sideways like every other landscape app here. All per-pixel drawing
 * below maps landscape to panel itself (paint_rect), the same deliberate
 * exception shapes.c's aa_composite_land and level.c's paint_rect both make,
 * for the same reason: a coverage value has no rectangle for gfx to rotate.
 * ------------------------------------------------------------------- */

// The bounce boundary applied to the BALL'S CENTRE. 26px in from every edge
// keeps the drawn ink (a 9px ball plus ~1.5px of anti-aliasing fringe) at
// least 15px clear of PANEL_BEZEL_MARGIN_PX (10, gfx.h) on every wall.
// PLAY_B is no longer a bounce boundary - see header comment's "THREE
// LIVES" section: crossing it is now how the ball is lost.
#define PLAY_L 26.0f
#define PLAY_R (LAND_W - 26.0f) // 422
#define PLAY_T 26.0f
#define PLAY_B (LAND_H - 26.0f) // 342

#define BALL_R 9.0f

// The paddle. See the header comment's "the wall is an arc, not a grid"
// section for why it is drawn as a band-around-a-circle intersected with a
// bounding disc, rather than a bar.
#define PADDLE_Y            300.0f
#define PADDLE_HALF_W        46.0f
#define PADDLE_ARC_R        220.0f // the (off-screen) circle the paddle's
                                    // band is concentric with; smaller = more bow
#define PADDLE_THICK_HALF     6.0f // half the ink band's thickness
#define PADDLE_LENS_R         50.0f // bounding disc that rounds both ends
#define PADDLE_CENTER_X      ((PLAY_L + PLAY_R) / 2.0f)                    // 224
#define PADDLE_TRAVEL_MAX    ((PLAY_R - PLAY_L) / 2.0f - PADDLE_HALF_W)    // 152

// The tilt angle (as a fraction of 1g in-plane, roughly deg = asin(g)*180/pi
// so 0.4g is about 23.6 degrees) at which the paddle reaches full travel.
// Comfortably inside what a small hand can hold without the puck feeling
// like it is about to tip past "on edge".
#define PADDLE_GX_FULL 0.4f

// How much a paddle hit deflects the ball sideways, in px/ms of vx added per
// unit of (hit position - paddle centre) / PADDLE_HALF_W. Rescaled away by
// rescale_ball_speed() immediately after, so this only ever changes ANGLE,
// never lets a hit speed the ball up on its own.
#define PADDLE_ENGLISH 0.09f

/* ---- lives, losing the ball, and game over - see header comment's
 * "THREE LIVES, LOSING THE BALL, AND GAME OVER" section for the whole
 * argument. Defined here, after PADDLE_CENTER_X/PADDLE_Y, since the
 * ripple's fixed anchor is expressed relative to the paddle. ------------- */
#define START_LIVES 3

// The beat of stillness after a life is lost, before either a fresh ball
// drops in or the table empties for good - dino.c's own DEATH_FREEZE_MS
// number and reasoning, reused rather than re-derived.
#define LIFE_LOST_FREEZE_MS 550u

// The ring that blooms when a life is lost - dino.c's own record-ripple and
// tiltball.c's own capture-ripple numbers, reused rather than re-derived
// (nothing about "how long should a bloom read as one event" changed by
// moving from a runner or a rolling ball to this game).
#define LOST_RIPPLE_DURATION_MS 380u
#define LOST_RIPPLE_MAX_R    20.0f
#define LOST_RIPPLE_MAX_BAND  6.0f
// A FIXED screen anchor, not the ball's actual crossing point - see header
// comment for why (dino.c's own ripple makes the same call, for the same
// reason). Checked clear of every edge's bezel margin: at max extent the
// ring reaches RIPPLE_CY +/- 26 = [300, 352] and RIPPLE_CX +/- 26 =
// [198, 250], both comfortably inside PANEL_BEZEL_MARGIN_PX of every side.
#define RIPPLE_CX PADDLE_CENTER_X
#define RIPPLE_CY (PADDLE_Y + 26.0f)

// The lives themselves: three small SOLID discs, top-left corner, clear of
// the wall's own arcs and the bezel - see header comment's "GEOMETRY CHECK".
#define LIFE_DOT_R   6.0f
#define LIFE_DOT_GAP 18.0f
#define LIFE_DOT_X0  (PLAY_L + 12.0f) // 38
#define LIFE_DOT_Y   (PLAY_T + 12.0f) // 38

// The restart gesture's arming/release-grace numbers - dino.c's own
// TAP_ARM_*/TAP_RELEASE_GRACE_MS, copied verbatim (itself copied from
// four.c): the same controller, the same dropout profile, the same
// arithmetic.
#define TAP_ARM_SAMPLES      4
#define TAP_ARM_MS           40u
#define TAP_ARM_RATE_HZ      15u
#define TAP_RELEASE_GRACE_MS 300u

/* ---- the wall: eighteen rings on three concentric arcs (see the header
 * comment's "THE WALL IS THREE NESTED ARCS" section for why nesting arcs,
 * not stacking straight shelves, is what "three rows" means under decision
 * 0009). All three rows share one centre (ARC_CX, ARC_CY); a row's own
 * radius is simply that shared centre minus the row's topmost point
 * (ROW_PEAK_Y), so "three rows" is three radii, not three unrelated
 * layouts. -------------------------------------------------------------- */
#define N_ROWS 3
#define BRICKS_PER_ROW 6
#define N_BRICKS (N_ROWS * BRICKS_PER_ROW)
#define BRICK_R 15.0f            // outer ink radius, fully grown
#define ARC_CX 224.0f            // LAND_W / 2
#define ARC_CY 320.0f            // the one centre all three rows curve around
#define ARC_THETA_MAX_DEG 36.0f  // half-angle, shared by every row

// Row 0 is outermost (topmost, farthest from the paddle), row 2 innermost
// (closest to the paddle) - see ROW_PEAK_Y/ROW_BAND_HALF below, and the
// header comment on why the band thickness grows toward the paddle.
#define ROW0_PEAK_Y 55.0f
#define ROW1_PEAK_Y 98.0f
#define ROW2_PEAK_Y 141.0f
#define ROW0_BAND_HALF 2.5f
#define ROW1_BAND_HALF 3.0f
#define ROW2_BAND_HALF 3.5f

/* ---- speed ------------------------------------------------------------ */
#define BALL_SPEED_BASE 0.15f // px/ms
#define BALL_SPEED_MAX  0.28f
#define BALL_SPEED_PER_BRICK ((BALL_SPEED_MAX - BALL_SPEED_BASE) / (float)N_BRICKS)
#define BALL_START_ANGLE_DEG 34.0f // deterministic, not random - see enter()

/* ---- the fixed-timestep clock (see header comment) --------------------- */
#define FIXED_DT_MS 10u
#define BACKLOG_CAP_MS 5000u

/* ---- clearing the wall, celebrated by regrowing it in a wave ----------- */
#define CELEB_PAUSE_MS   400u
#define CELEB_STAGGER_MS  55u
#define CELEB_GROW_MS    220u
#define CELEB_TOTAL_MS (CELEB_PAUSE_MS + (uint32_t)(N_BRICKS - 1) * CELEB_STAGGER_MS + CELEB_GROW_MS)

// Redraw granularity: the drawn position is snapped to this, so two runs
// that converge on the same simulated state converge on the same pixels,
// and a frame that would repaint the same picture is skipped. Same
// technique and same constant as level.c's LEVEL_POS_QUANT.
#define POS_QUANT 0.5f

/* ---------------------------------------------------------------------
 * Colour. Unswapped RGB565 (px_swap applied once, at the final pixel
 * write - see paint_rect). Everything drawn is one of these two values now
 * - see the header comment's "ONE INK" section for why a ring vs a solid
 * disc replaced a palette.
 * ------------------------------------------------------------------- */
#define RGB565_WHITE 0xFFFFu
#define RGB565_BLACK 0x0000u

// Per-row ring thickness (half the band's width, ROW_BAND_HALF above),
// indexed by row = brick index / BRICKS_PER_ROW - see the header comment's
// "ONE INK" section on why size (not hue) is what tells a row apart.
static const float ROW_BAND_HALF[N_ROWS] = { ROW0_BAND_HALF, ROW1_BAND_HALF, ROW2_BAND_HALF };
static const float ROW_PEAK_Y[N_ROWS]    = { ROW0_PEAK_Y, ROW1_PEAK_Y, ROW2_PEAK_Y };

/* ---------------------------------------------------------------------
 * State: one struct from the arena (app.h), never file-scope.
 * ------------------------------------------------------------------- */
typedef struct {
    float cx, cy;    // fixed for life; only ever set once, in enter()
    bool alive;       // true = fully present and collidable
} brick_t;

// The game's own life-cycle - see header comment's "THREE LIVES" section.
// LP_PLAYING (0) is the zeroed arena's own default, so a fresh app-switch-in
// lands in ordinary play, exactly like every other state enum in this file.
typedef enum {
    LP_PLAYING = 0,
    LP_LOST_FREEZE,   // a life just went; a beat of stillness (timed off
                       // simMs) before either a fresh ball drops in or the
                       // table empties for good
    LP_GAMEOVER_WAIT,  // no lives left; everything holds still, waiting for
                        // a tap - see header comment
} life_phase_t;

// The restart gesture's own arming state - dino.c's tap_state_t/tap_tick,
// copied verbatim (see header comment). Only ever fed touch while
// s->lifePhase == LP_GAMEOVER_WAIT.
typedef struct {
    bool     everContacted;
    bool     armed;         // this physical press has already fired
    int      contactCount;
    uint32_t firstContactMs;
    uint32_t lastContactMs;
} tap_state_t;

typedef struct {
    bool     inited;
    uint32_t simMs;   // the fixed-timestep clock (header comment)

    float ballX, ballY, ballVX, ballVY;
    float paddleX;
    bool  ballVisible; // false while lost - see header comment

    brick_t bricks[N_BRICKS];
    int     aliveCount;

    bool     celebrating;
    uint32_t celebrateStartMs; // simMs at the tick the last brick broke,
                                // OR at the tick a fresh game's regrow began

    int          lives;
    life_phase_t lifePhase;
    uint32_t     lifePhaseStartMs; // simMs at the tick lifePhase last changed

    bool     rippleActive;
    uint32_t rippleStartMs; // simMs

    tap_state_t tap; // the restart gesture - read only in LP_GAMEOVER_WAIT

    // What is actually ON SCREEN right now. paint_rect() renders exactly
    // this, which is what makes an incrementally-updated frame identical to
    // a freshly-entered one - same argument and same technique as
    // level.c's drawn* fields.
    float drawnBallX, drawnBallY;
    bool  drawnBallVisible;
    float drawnPaddleX;
    int   drawnLives;
    float drawnRippleR, drawnRippleBand; // 0 when the ripple has nothing to draw
    float drawnBrickR[N_BRICKS];
} breakout_state_t;

static breakout_state_t *s_state;

/* ---------------------------------------------------------------------
 * Colour blending. Unlike level.c (grey-only, MIN composition), bricks need
 * a real colour composited under black ink, so this is a small back-to-
 * front alpha blend instead: paper white, then the best-covering brick (if
 * any), then the lives row, then the ripple, then the paddle, then the
 * ball - each a single mix by that shape's own anti-aliased coverage. Order
 * rarely matters in practice (these shapes almost never share a pixel - see
 * header comment's "ONE INK" addendum), but this is the fixed sequence
 * every frame uses regardless.
 * ------------------------------------------------------------------- */
static inline uint16_t rgb565_pack(int r, int g, int b) {
    if (r < 0) r = 0; else if (r > 31) r = 31;
    if (g < 0) g = 0; else if (g > 63) g = 63;
    if (b < 0) b = 0; else if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline uint16_t rgb565_mix(uint16_t a, uint16_t b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    return rgb565_pack((int)(ar + (br - ar) * t + 0.5f),
                        (int)(ag + (bg - ag) * t + 0.5f),
                        (int)(ab + (bb - ab) * t + 0.5f));
}

// Coverage (0..1) from a signed distance (negative = inside), same
// convention as sketch.c's draw_capsule and level.c's shade(): a half-pixel
// feather either side of the true edge.
static inline float coverage_from_dist(float d) {
    float c = 0.5f - d;
    if (c < 0.0f) c = 0.0f; else if (c > 1.0f) c = 1.0f;
    return c;
}

/* ---------------------------------------------------------------------
 * Drawing. One function paints; nothing erases (see header comment).
 * ------------------------------------------------------------------- */
typedef struct { int x0, y0, x1, y1; } rect_t; // inclusive, landscape

static rect_t clamp_rect(rect_t r) {
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > LAND_W - 1) r.x1 = LAND_W - 1;
    if (r.y1 > LAND_H - 1) r.y1 = LAND_H - 1;
    return r;
}

// Renders every pixel of an inclusive landscape rectangle from the model in
// `s->drawn*` - the whole anti-residue argument, identical in spirit to
// level.c's paint_rect: a pixel inside the rectangle is recomputed, never
// patched, so nothing can survive there the model does not put there.
static void paint_rect(const breakout_state_t *s, rect_t r) {
    r = clamp_rect(r);
    if (r.x0 > r.x1 || r.y0 > r.y1) return;

    const float paddleArcCy = PADDLE_Y + PADDLE_ARC_R;
    const float paddleAabbYlo = PADDLE_Y - PADDLE_LENS_R - PADDLE_THICK_HALF - 3.0f;
    const float paddleAabbYhi = PADDLE_Y + PADDLE_LENS_R + PADDLE_THICK_HALF + 3.0f;
    const float paddleAabbHalfW = PADDLE_HALF_W + PADDLE_LENS_R + 3.0f;

    for (int lx = r.x0; lx <= r.x1; lx++) {
        float fx = (float)lx + 0.5f;
        float dxBall = fx - s->drawnBallX;
        float dxPaddle = fx - s->drawnPaddleX;
        bool paddleInX = fabsf(dxPaddle) <= paddleAabbHalfW;

        int idx = lx * PANEL_W + (PANEL_W - 1 - r.y0);
        for (int ly = r.y0; ly <= r.y1; ly++, idx--) {
            float fy = (float)ly + 0.5f;
            uint16_t outv = RGB565_WHITE;

            // The wall: whichever brick's RING covers this pixel most - see
            // the header comment's "ONE INK" section on why a brick is a
            // stroked annulus (outer radius br, half-thickness the row's own
            // ROW_BAND_HALF) rather than a filled disc now that colour
            // cannot tell it apart from the ball. Bricks never overlap each
            // other by construction (the arc's own spacing, chosen with
            // room to spare), so "most" is really "the only one that can",
            // found with a cheap box reject before any sqrtf - the box and
            // the far/near radius rejects below both grow by that row's
            // band, since the ring's ink now lives on BOTH sides of br.
            {
                float bestCov = 0.0f;
                for (int i = 0; i < N_BRICKS; i++) {
                    float br = s->drawnBrickR[i];
                    if (br <= 0.01f) continue;
                    float band = ROW_BAND_HALF[i / BRICKS_PER_ROW];
                    float dxk = fx - s->bricks[i].cx;
                    if (fabsf(dxk) > br + band + 2.0f) continue;
                    float dyk = fy - s->bricks[i].cy;
                    if (fabsf(dyk) > br + band + 2.0f) continue;
                    float d2 = dxk * dxk + dyk * dyk;
                    float rrOuter = br + band + 1.5f;
                    if (d2 > rrOuter * rrOuter) continue;
                    float rrInner = br - band - 1.5f;
                    if (rrInner > 0.0f && d2 < rrInner * rrInner) continue; // inside the ring's own hole
                    float d = fabsf(sqrtf(d2) - br) - band;
                    float c = coverage_from_dist(d);
                    if (c > bestCov) bestCov = c;
                }
                if (bestCov > 0.0f) outv = rgb565_mix(outv, RGB565_BLACK, bestCov);
            }

            // The lives row: up to START_LIVES small SOLID discs - see
            // header comment's "WHERE THE LIVES LIVE" section. Drawn from
            // s->drawnLives, not s->lives, per the "paint only ever reads
            // drawn state" rule every shape here follows.
            if (s->drawnLives > 0) {
                float bestCov = 0.0f;
                for (int i = 0; i < s->drawnLives; i++) {
                    float cxk = LIFE_DOT_X0 + (float)i * LIFE_DOT_GAP;
                    float dxk = fx - cxk;
                    if (fabsf(dxk) > LIFE_DOT_R + 1.5f) continue;
                    float dyk = fy - LIFE_DOT_Y;
                    if (fabsf(dyk) > LIFE_DOT_R + 1.5f) continue;
                    float d = sqrtf(dxk * dxk + dyk * dyk) - LIFE_DOT_R;
                    float c = coverage_from_dist(d);
                    if (c > bestCov) bestCov = c;
                }
                if (bestCov > 0.0f) outv = rgb565_mix(outv, RGB565_BLACK, bestCov);
            }

            // The life-lost ripple: a ring at a fixed screen anchor - see
            // header comment's "WHAT LOSING THE BALL LOOKS LIKE" section.
            if (s->drawnRippleR > 0.0f) {
                float dxk = fx - RIPPLE_CX, dyk = fy - RIPPLE_CY;
                float dist = sqrtf(dxk * dxk + dyk * dyk);
                float band = s->drawnRippleBand;
                float dOuter = dist - s->drawnRippleR;
                float dInner = dist - (s->drawnRippleR - band);
                float d = dOuter > -dInner ? dOuter : -dInner;
                float c = coverage_from_dist(d);
                if (c > 0.0f) outv = rgb565_mix(outv, RGB565_BLACK, c);
            }

            // The paddle: intersection of a band around a large arc (the
            // bow) and a bounding disc (rounds both ends) - see the header
            // comment's "the wall is an arc, not a grid" section.
            if (paddleInX && fy >= paddleAabbYlo && fy <= paddleAabbYhi) {
                float dyArc = fy - paddleArcCy;
                float distArc = sqrtf(dxPaddle * dxPaddle + dyArc * dyArc) - PADDLE_ARC_R;
                float dBand = fabsf(distArc) - PADDLE_THICK_HALF;
                float dyLens = fy - PADDLE_Y;
                float distLens = sqrtf(dxPaddle * dxPaddle + dyLens * dyLens) - PADDLE_LENS_R;
                float dPaddle = dBand > distLens ? dBand : distLens; // SDF intersection
                float c = coverage_from_dist(dPaddle);
                if (c > 0.0f) outv = rgb565_mix(outv, RGB565_BLACK, c);
            }

            // The ball, always on top - drawn only while visible (see
            // header comment: it vanishes the instant it is lost).
            if (s->drawnBallVisible) {
                float dyBall = fy - s->drawnBallY;
                float d2 = dxBall * dxBall + dyBall * dyBall;
                float rr = BALL_R + 1.5f;
                if (d2 <= rr * rr) {
                    float d = sqrtf(d2) - BALL_R;
                    float c = coverage_from_dist(d);
                    if (c > 0.0f) outv = rgb565_mix(outv, RGB565_BLACK, c);
                }
            }

            gfx_fb[idx] = px_swap(outv);
        }
    }
}

// Pre-rounds a rectangle to exactly what gfx_push_land would round it to
// anyway (decision 0001: row length a multiple of 8), so the window PAINTED
// and the window PUSHED are the same rectangle. Identical technique to
// level.c's align_for_push - see its comment for the full derivation of why
// landscape height (not width) is the row length that must be a multiple of
// 8, and why the start need only be even.
static rect_t align_for_push(rect_t r) {
    r = clamp_rect(r);
    r.y0 &= ~1;
    int h = r.y1 - r.y0 + 1;
    int need = (8 - (h & 7)) & 7;
    if (need > 0) {
        int room = (LAND_H - 1) - r.y1;
        int grow = need < room ? need : room;
        r.y1 += grow;
        need -= grow;
        if (need > 0) {
            int back = r.y0 < need ? r.y0 : need;
            back &= ~1;
            r.y0 -= back;
        }
    }
    r.x0 &= ~1;
    if (((r.x1 - r.x0 + 1) & 1) != 0) {
        if (r.x1 < LAND_W - 1) r.x1++;
        else r.x0--;
    }
    return r;
}

static void paint_and_push(const breakout_state_t *s, rect_t r) {
    r = align_for_push(r);
    if (r.x0 > r.x1 || r.y0 > r.y1) return;
    paint_rect(s, r);
    gfx_push_land(r.x0, r.y0, r.x1 - r.x0 + 1, r.y1 - r.y0 + 1);
}

static rect_t ball_rect(float x, float y) {
    rect_t r;
    r.x0 = (int)floorf(x - BALL_R - 1.5f);
    r.y0 = (int)floorf(y - BALL_R - 1.5f);
    r.x1 = (int)ceilf(x + BALL_R + 1.5f);
    r.y1 = (int)ceilf(y + BALL_R + 1.5f);
    return r;
}

static rect_t paddle_rect(float x) {
    rect_t r;
    r.x0 = (int)floorf(x - PADDLE_HALF_W - PADDLE_LENS_R * 0.0f - 4.0f);
    r.x1 = (int)ceilf(x + PADDLE_HALF_W + PADDLE_LENS_R * 0.0f + 4.0f);
    r.y0 = (int)floorf(PADDLE_Y - PADDLE_LENS_R - PADDLE_THICK_HALF - 4.0f);
    r.y1 = (int)ceilf(PADDLE_Y + PADDLE_LENS_R + PADDLE_THICK_HALF + 4.0f);
    return r;
}

// `band` is the drawing row's own ROW_BAND_HALF: a ring's ink reaches
// `r_ + band` outward now, not just `r_` the way a filled disc's did, so the
// bounding box has to grow by it too or the ring's own outer edge is left
// outside the pushed (and cleared) rectangle - exactly decision 0001's
// shape of bug, caught here before it ever reached a build.
static rect_t brick_rect(float cx, float cy, float r_, float band) {
    rect_t r;
    r.x0 = (int)floorf(cx - r_ - band - 1.5f);
    r.y0 = (int)floorf(cy - r_ - band - 1.5f);
    r.x1 = (int)ceilf(cx + r_ + band + 1.5f);
    r.y1 = (int)ceilf(cy + r_ + band + 1.5f);
    return r;
}

// The lives row's own footprint - a fixed box, not one sized to the current
// count: a shrinking count has to clear the dot that just disappeared, not
// just repaint whatever is still occupied, so this always covers the whole
// row's max extent (three dots' worth), the same reasoning brick_rect's own
// comment gives for growing by `band`.
static rect_t lives_rect(void) {
    rect_t r;
    r.x0 = (int)floorf(LIFE_DOT_X0 - LIFE_DOT_R - 2.0f);
    r.x1 = (int)ceilf(LIFE_DOT_X0 + (float)(START_LIVES - 1) * LIFE_DOT_GAP + LIFE_DOT_R + 2.0f);
    r.y0 = (int)floorf(LIFE_DOT_Y - LIFE_DOT_R - 2.0f);
    r.y1 = (int)ceilf(LIFE_DOT_Y + LIFE_DOT_R + 2.0f);
    return r;
}

// The ripple's own footprint - fixed position, fixed max radius (see
// LOST_RIPPLE_CX/CY above), so like lives_rect this is always the same box
// regardless of the ring's current radius.
static rect_t ripple_rect(void) {
    rect_t r;
    r.x0 = (int)floorf(RIPPLE_CX - LOST_RIPPLE_MAX_R - 2.0f);
    r.x1 = (int)ceilf(RIPPLE_CX + LOST_RIPPLE_MAX_R + 2.0f);
    r.y0 = (int)floorf(RIPPLE_CY - LOST_RIPPLE_MAX_R - 2.0f);
    r.y1 = (int)ceilf(RIPPLE_CY + LOST_RIPPLE_MAX_R + 2.0f);
    return r;
}

static rect_t rect_union(rect_t a, rect_t b) {
    rect_t r;
    r.x0 = a.x0 < b.x0 ? a.x0 : b.x0;
    r.y0 = a.y0 < b.y0 ? a.y0 : b.y0;
    r.x1 = a.x1 > b.x1 ? a.x1 : b.x1;
    r.y1 = a.y1 > b.y1 ? a.y1 : b.y1;
    return r;
}

static float quantise(float v) {
    return floorf(v / POS_QUANT + 0.5f) * POS_QUANT;
}

/* ---------------------------------------------------------------------
 * Physics: one fixed step. See the header comment's "the game loop"
 * section for why this is called from a fixed-quantum accumulator rather
 * than once per tick with f->dtMs.
 * ------------------------------------------------------------------- */
static void rescale_ball_speed(breakout_state_t *s) {
    int broken = N_BRICKS - s->aliveCount;
    float target = BALL_SPEED_BASE + (float)broken * BALL_SPEED_PER_BRICK;
    if (target > BALL_SPEED_MAX) target = BALL_SPEED_MAX;
    float mag2 = s->ballVX * s->ballVX + s->ballVY * s->ballVY;
    if (mag2 < 1e-6f) { s->ballVX = 0.0f; s->ballVY = -target; return; }
    float k = target / sqrtf(mag2);
    s->ballVX *= k;
    s->ballVY *= k;
}

// L/R/T only now - the bottom is no longer a wall, see header comment's
// "THREE LIVES" section: crossing PLAY_B is checked separately, right after
// this call, in sim_step.
static void ball_wall_bounce(breakout_state_t *s) {
    if (s->ballX - BALL_R < PLAY_L) { s->ballX = PLAY_L + BALL_R; if (s->ballVX < 0.0f) s->ballVX = -s->ballVX; }
    else if (s->ballX + BALL_R > PLAY_R) { s->ballX = PLAY_R - BALL_R; if (s->ballVX > 0.0f) s->ballVX = -s->ballVX; }
    if (s->ballY - BALL_R < PLAY_T) { s->ballY = PLAY_T + BALL_R; if (s->ballVY < 0.0f) s->ballVY = -s->ballVY; }
}

// THE COLLISION ZONE NOW MATCHES THE DRAWING, EXACTLY, NOT A STADIUM
// STAND-IN. This used to be a flat spine (half-width PADDLE_HALF_W) with a
// uniform rr=BALL_R+PADDLE_THICK_HALF+1 capsule around it - a shape nobody
// ever drew, chosen because it was cheap. Measured against the REAL drawn
// shape (paint_rect's own band-around-an-arc intersected with a bounding
// lens, "THE WALL IS THREE NESTED ARCS" section above), that capsule was
// not a close approximation, it was a materially BIGGER shape in every
// direction sampled: at the paddle's own centreline it reached +/-62px
// against the drawn shape's +/-50px, and directly above the paddle it kept
// catching a ball up to 16px above the ink with nothing there at all (a
// two-year-old watching that would see the ball vanish into visibly empty
// paper, not "miss the paddle" - decision 0010's own standard: a picture
// has to be judged, and this one was never looked at against its hitbox).
// Total measured area: the old capsule was 3792px^2 against the drawn
// shape's own 1312px^2 - 2483px^2 of it was pure phantom, catching in air.
//
// The fix: erode paint_rect's own two signed distances (dBand, distLens -
// see paddle_band_sdf/paddle_lens_sdf below, literally the same formulas,
// not an approximation of them) by BALL_R, and use the tighter of the two
// as both the hit test and the true surface normal to bounce off of -
// exactly the "whichever ink wins" logic paint_rect already uses for
// DRAWING, read here as "whichever constraint is binding" for PHYSICS
// instead. A ball's own disc (radius BALL_R) now only bounces where its
// edge would visibly touch drawn ink, never in the white paper next to it.
typedef struct { float d, nx, ny; } paddle_sdf_t;

// The band's own signed distance and outward unit normal, exactly
// paint_rect's `distArc`/`dBand` (dx, dy relative to the paddle's centre,
// paddleX/PADDLE_Y) - see that function's own comment for the geometry.
// The normal is the gradient of |distArc|: radially out from the big
// off-screen arc's own centre on the outer edge of the band (distArc>0),
// radially IN toward it on the inner edge (distArc<0) - the band has two
// faces and each one's own outward direction is different.
static paddle_sdf_t paddle_band_sdf(float dx, float dy) {
    float dyArc = dy - PADDLE_ARC_R;
    float rArc = sqrtf(dx * dx + dyArc * dyArc);
    if (rArc < 0.0001f) rArc = 0.0001f;
    float distArc = rArc - PADDLE_ARC_R;
    float sign = distArc < 0.0f ? -1.0f : 1.0f;
    paddle_sdf_t r = { fabsf(distArc) - PADDLE_THICK_HALF, sign * dx / rArc, sign * dyArc / rArc };
    return r;
}

// The bounding lens disc's own signed distance and outward normal, exactly
// paint_rect's `distLens` - a plain distance-to-a-circle gradient, radially
// out from the paddle's own centre.
static paddle_sdf_t paddle_lens_sdf(float dx, float dy) {
    float rLens = sqrtf(dx * dx + dy * dy);
    if (rLens < 0.0001f) rLens = 0.0001f;
    paddle_sdf_t r = { rLens - PADDLE_LENS_R, dx / rLens, dy / rLens };
    return r;
}

// The paddle is a solid obstacle from either face (it now sits inside the
// open field rather than guarding a floor - see header comment), so this
// bounces off whichever side of the drawn shape the ball is approaching
// from, rather than assuming "from above" the way a classic breakout
// paddle can.
static void ball_paddle_bounce(breakout_state_t *s) {
    float dx = s->ballX - s->paddleX, dy = s->ballY - PADDLE_Y;
    paddle_sdf_t band = paddle_band_sdf(dx, dy);
    paddle_sdf_t lens = paddle_lens_sdf(dx, dy);
    // Whichever constraint is BINDING (the larger, i.e. closer to or past
    // its own surface) is the real one here - paint_rect's own
    // `dBand > distLens ? dBand : distLens`, read as physics instead of
    // ink. Its gradient is the true outward normal at this point.
    paddle_sdf_t active = band.d > lens.d ? band : lens;
    if (active.d > BALL_R) return; // clear of the drawn shape, ball radius included

    // Only bounce a ball moving TOWARD this surface: stops a ball that is
    // already leaving from re-triggering on the next fixed step while it is
    // still inside BALL_R of it.
    float dot = s->ballVX * active.nx + s->ballVY * active.ny;
    if (dot >= 0.0f) return;

    // Push the ball out to exactly BALL_R from the (now binding) surface,
    // along its own normal - same idea as the old capsule's fixed push
    // distance, just measured from the real surface instead of a
    // spine-segment stand-in.
    float push = BALL_R - active.d + 0.5f;
    s->ballX += active.nx * push;
    s->ballY += active.ny * push;

    s->ballVX -= 2.0f * dot * active.nx;
    s->ballVY -= 2.0f * dot * active.ny;

    float off = dx / PADDLE_HALF_W;
    if (off < -1.0f) off = -1.0f; else if (off > 1.0f) off = 1.0f;
    s->ballVX += off * PADDLE_ENGLISH;

    rescale_ball_speed(s);
}

// At most one brick per fixed step - at this step size (a few px of travel)
// two simultaneous brick hits is not a shape this arrangement produces, and
// picking one cleanly beats resolving two half-correctly.
static void ball_bricks_bounce(breakout_state_t *s, uint32_t simMsNow) {
    for (int i = 0; i < N_BRICKS; i++) {
        if (!s->bricks[i].alive) continue;
        float dx = s->ballX - s->bricks[i].cx, dy = s->ballY - s->bricks[i].cy;
        float rr = BALL_R + BRICK_R;
        float d2 = dx * dx + dy * dy;
        if (d2 >= rr * rr) continue;

        float d = sqrtf(d2);
        float nx, ny;
        if (d < 0.0001f) { nx = 0.0f; ny = -1.0f; d = 0.0001f; } else { nx = dx / d; ny = dy / d; }

        float dot = s->ballVX * nx + s->ballVY * ny;
        s->ballVX -= 2.0f * dot * nx;
        s->ballVY -= 2.0f * dot * ny;
        s->ballX = s->bricks[i].cx + nx * (rr + 0.5f);
        s->ballY = s->bricks[i].cy + ny * (rr + 0.5f);

        s->bricks[i].alive = false;
        s->aliveCount--;
        rescale_ball_speed(s);

        if (s->aliveCount == 0) {
            s->celebrating = true;
            s->celebrateStartMs = simMsNow;
        }
        return;
    }
}

/* ---------------------------------------------------------------------
 * The restart gesture - dino.c's tap_tick(), copied verbatim (see header
 * comment's "GAME OVER, AND HOW SHE STARTS AGAIN" section). Only ever
 * called while s->lifePhase == LP_GAMEOVER_WAIT.
 * ------------------------------------------------------------------- */
static bool tap_tick(tap_state_t *g, const app_frame_t *f) {
    bool fired = false;
    if (f->touchDown) {
        if (!g->everContacted) {
            g->everContacted = true;
            g->contactCount = 0;
            g->firstContactMs = f->nowMs;
        }
        g->contactCount++;
        g->lastContactMs = f->nowMs;
        if (!g->armed) {
            uint32_t elapsed = f->nowMs - g->firstContactMs;
            if (g->contactCount >= TAP_ARM_SAMPLES && elapsed >= TAP_ARM_MS &&
                (uint32_t)g->contactCount * 1000u >= TAP_ARM_RATE_HZ * elapsed) {
                g->armed = true;
                fired = true;
            }
        }
    } else if (g->everContacted && (f->nowMs - g->lastContactMs) >= TAP_RELEASE_GRACE_MS) {
        g->everContacted = false;
        g->armed = false;
        g->contactCount = 0;
    }
    return fired;
}

// Drops a fresh ball in at the exact deterministic start position/angle
// enter() itself uses, and re-centres the paddle - shared by enter(), a
// mid-game respawn after a life is lost (bricks untouched), and a full
// restart from game over (lives also reset, by the caller). See header
// comment's "THREE LIVES" section.
static void reset_ball_and_paddle(breakout_state_t *s) {
    s->ballX = ARC_CX;
    s->ballY = PADDLE_Y - 100.0f;
    float a = BALL_START_ANGLE_DEG * BREAKOUT_PI / 180.0f;
    s->ballVX = BALL_SPEED_BASE * cosf(a);
    s->ballVY = -BALL_SPEED_BASE * sinf(a);
    s->paddleX = PADDLE_CENTER_X;
    s->ballVisible = true;
}

static void sim_step(breakout_state_t *s, float dtMs, uint32_t simMsNow) {
    if (s->lifePhase == LP_LOST_FREEZE) {
        if (simMsNow - s->lifePhaseStartMs >= LIFE_LOST_FREEZE_MS) {
            if (s->lives > 0) {
                // Bricks untouched - a life lost never resets the wall, see
                // header comment. rescale_ball_speed() sets the fresh
                // ball's pace from whatever aliveCount already is.
                reset_ball_and_paddle(s);
                rescale_ball_speed(s);
                s->lifePhase = LP_PLAYING;
            } else {
                s->lifePhase = LP_GAMEOVER_WAIT;
                printf("breakout: game over\r\n");
            }
        }
        return; // frozen throughout: no ball motion, no brick changes
    }
    if (s->lifePhase == LP_GAMEOVER_WAIT) {
        return; // nothing moves; only a tap (breakout_tick) ends this
    }

    if (s->celebrating) {
        uint32_t elapsed = simMsNow - s->celebrateStartMs;
        if (elapsed >= CELEB_TOTAL_MS) {
            for (int i = 0; i < N_BRICKS; i++) s->bricks[i].alive = true;
            s->aliveCount = N_BRICKS;
            s->celebrating = false;
            rescale_ball_speed(s); // back to base pace for the fresh wall
        }
        return; // the ball holds its position and velocity throughout
    }

    s->ballX += s->ballVX * dtMs;
    s->ballY += s->ballVY * dtMs;
    ball_wall_bounce(s);

    if (s->ballY + BALL_R > PLAY_B) {
        // Lost - see header comment's "WHAT LOSING THE BALL LOOKS LIKE".
        // The ball vanishes here, this same simulated instant; the ripple
        // and the freeze are what the next few real ticks' worth of
        // painting will show for it.
        s->lives--;
        s->ballVisible = false;
        s->rippleActive = true;
        s->rippleStartMs = simMsNow;
        s->lifePhase = LP_LOST_FREEZE;
        s->lifePhaseStartMs = simMsNow;
        printf("breakout: ball lost, %d live(s) left\r\n", s->lives);
        return;
    }

    ball_paddle_bounce(s);
    ball_bricks_bounce(s, simMsNow);
}

// The radius each brick should be DRAWN at right now - full circle if alive,
// nothing if dead and not (yet) regrowing, an eased-in circle mid-regrow.
// Purely cosmetic: called once per tick after the physics catch-up loop, on
// the final s->simMs, so it is exactly as deterministic as everything else
// here (a pure function of simMs).
static void compute_visible_brick_radii(const breakout_state_t *s, float *outR) {
    if (!s->celebrating) {
        for (int i = 0; i < N_BRICKS; i++) outR[i] = s->bricks[i].alive ? BRICK_R : 0.0f;
        return;
    }
    uint32_t elapsed = s->simMs - s->celebrateStartMs;
    for (int i = 0; i < N_BRICKS; i++) {
        uint32_t start = CELEB_PAUSE_MS + (uint32_t)i * CELEB_STAGGER_MS;
        if (elapsed <= start) { outR[i] = 0.0f; continue; }
        uint32_t t = elapsed - start;
        if (t >= CELEB_GROW_MS) { outR[i] = BRICK_R; continue; }
        float frac = (float)t / (float)CELEB_GROW_MS;
        float eased = 1.0f - (1.0f - frac) * (1.0f - frac); // ease-out, the sketchpad palette's own pop-in curve
        outR[i] = BRICK_R * eased;
    }
}

/* ---------------------------------------------------------------------
 * The frame.
 * ------------------------------------------------------------------- */
static void breakout_enter(void) {
    s_state = APP_STATE(breakout_state_t);
    breakout_state_t *s = s_state;

    // enter() takes no frame (app.h), so unlike tick() this cannot read
    // f->nowMs or f->tilt - s->inited stays false (APP_STATE zeroes) and the
    // very first tick() seeds s->simMs from the real clock. Nothing about
    // the INITIAL picture depends on that first reading, unlike level.c:
    // every position below is deterministic, so there is nothing to hide
    // until a signal arrives.
    reset_ball_and_paddle(s);
    s->lives = START_LIVES;
    s->lifePhase = LP_PLAYING;

    // Three concentric arcs, one per row, sharing ARC_CX/ARC_CY - see the
    // header comment's "THE WALL IS THREE NESTED ARCS" section. Row 0 is
    // outermost (its own radius, ARC_CY - ROW_PEAK_Y[0], is the LARGEST),
    // row 2 innermost, closest to the paddle.
    float thetaMax = ARC_THETA_MAX_DEG * BREAKOUT_PI / 180.0f;
    for (int row = 0; row < N_ROWS; row++) {
        float rowR = ARC_CY - ROW_PEAK_Y[row];
        for (int k = 0; k < BRICKS_PER_ROW; k++) {
            int i = row * BRICKS_PER_ROW + k;
            float t = (BRICKS_PER_ROW == 1) ? 0.0f
                : ((float)k / (float)(BRICKS_PER_ROW - 1)) * 2.0f - 1.0f; // -1..1
            float theta = t * thetaMax;
            s->bricks[i].cx = ARC_CX + rowR * sinf(theta);
            s->bricks[i].cy = ARC_CY - rowR * cosf(theta);
            s->bricks[i].alive = true;
        }
    }
    s->aliveCount = N_BRICKS;
    s->celebrating = false;

    s->drawnBallX = s->ballX;
    s->drawnBallY = s->ballY;
    s->drawnBallVisible = s->ballVisible;
    s->drawnPaddleX = s->paddleX;
    s->drawnLives = s->lives;
    s->drawnRippleR = 0.0f;
    s->drawnRippleBand = 0.0f;
    for (int i = 0; i < N_BRICKS; i++) s->drawnBrickR[i] = BRICK_R;

    // The whole field, once. enter() does not push - the runtime pushes the
    // panel after it returns (app.h).
    rect_t all = { 0, 0, LAND_W - 1, LAND_H - 1 };
    paint_rect(s, all);
}

static void breakout_tick(const app_frame_t *f) {
    breakout_state_t *s = s_state;

    if (!s->inited) {
        s->inited = true;
        s->simMs = f->nowMs;
    }

    // The paddle: a straight, unsmoothed map of the shared tilt signal (see
    // header comment on why no second filter is added, and why that
    // answers the coasting question for free). Applied once per real tick,
    // before this tick's physics catch-up, so any collision below sees the
    // freshest reading available. Kept running through every lifePhase,
    // freeze and game-over-wait included - see header comment's "CONTROL"
    // section on why that is left on.
    {
        float gx = f->tilt.valid ? f->tilt.gx : 0.0f;
        float frac = gx / PADDLE_GX_FULL;
        if (frac < -1.0f) frac = -1.0f; else if (frac > 1.0f) frac = 1.0f;
        s->paddleX = PADDLE_CENTER_X + frac * PADDLE_TRAVEL_MAX;
    }

    // The restart gesture: read ONLY while waiting at game over - see
    // header comment's "WHY THIS EXCEPTION TO READS NO TOUCH IS SAFE TO
    // MAKE NARROWLY" section. Any tap starts a fresh game: full lives, a
    // fresh ball and paddle, and the wall regrows through the exact same
    // wave a mid-game clear already uses (reused, not reinvented) whenever
    // the wall was not already whole.
    if (s->lifePhase == LP_GAMEOVER_WAIT) {
        if (tap_tick(&s->tap, f)) {
            s->lives = START_LIVES;
            reset_ball_and_paddle(s);
            s->lifePhase = LP_PLAYING;
            if (s->aliveCount < N_BRICKS) {
                s->celebrating = true;
                s->celebrateStartMs = s->simMs;
            } else {
                rescale_ball_speed(s);
            }
            printf("breakout: restart\r\n");
        }
    } else if (s->tap.everContacted || s->tap.armed) {
        // Not waiting at game over: the restart gesture's own state never
        // carries in from here - the same "clear the gesture state whenever
        // its own screen is not showing" rule four.c's own choice screen
        // gesture follows (section 8 of its header comment).
        s->tap.everContacted = false;
        s->tap.armed = false;
        s->tap.contactCount = 0;
    }

    if (f->nowMs - s->simMs > BACKLOG_CAP_MS) s->simMs = f->nowMs - BACKLOG_CAP_MS;
    while (s->simMs + FIXED_DT_MS <= f->nowMs) {
        s->simMs += FIXED_DT_MS;
        sim_step(s, (float)FIXED_DT_MS, s->simMs);
    }

    // The ripple: purely cosmetic, computed off s->simMs exactly like the
    // bricks' own regrow curve above - see compute_visible_brick_radii's
    // own comment on why that is what "deterministic" requires here.
    float curRippleR = 0.0f, curRippleBand = 0.0f;
    if (s->rippleActive) {
        uint32_t age = s->simMs - s->rippleStartMs;
        if (age >= LOST_RIPPLE_DURATION_MS) {
            s->rippleActive = false;
        } else {
            float u = (float)age / (float)LOST_RIPPLE_DURATION_MS;
            curRippleR = LOST_RIPPLE_MAX_R * u;
            curRippleBand = LOST_RIPPLE_MAX_BAND * (1.0f - u);
        }
    }

    float curR[N_BRICKS];
    compute_visible_brick_radii(s, curR);

    float newBallX = quantise(s->ballX), newBallY = quantise(s->ballY);
    float newPaddleX = quantise(s->paddleX);

    bool ballChanged = (newBallX != s->drawnBallX) || (newBallY != s->drawnBallY) ||
                        (s->ballVisible != s->drawnBallVisible);
    bool paddleChanged = (newPaddleX != s->drawnPaddleX);
    bool livesChanged = (s->lives != s->drawnLives);
    bool rippleChanged = (curRippleR != s->drawnRippleR) || (curRippleBand != s->drawnRippleBand);
    int changedBricks[N_BRICKS], nChangedBricks = 0;
    for (int i = 0; i < N_BRICKS; i++) {
        if (curR[i] != s->drawnBrickR[i]) changedBricks[nChangedBricks++] = i;
    }

    int totalItems = (ballChanged ? 1 : 0) + (paddleChanged ? 1 : 0) +
                      (livesChanged ? 1 : 0) + (rippleChanged ? 1 : 0) + nChangedBricks;
    if (totalItems == 0) return; // the same picture: no paint, no push

    if (totalItems <= 8) {
        rect_t dirty[12];
        int nd = 0;
        if (ballChanged) dirty[nd++] = rect_union(ball_rect(s->drawnBallX, s->drawnBallY), ball_rect(newBallX, newBallY));
        if (paddleChanged) dirty[nd++] = rect_union(paddle_rect(s->drawnPaddleX), paddle_rect(newPaddleX));
        if (livesChanged) dirty[nd++] = lives_rect();
        if (rippleChanged) dirty[nd++] = ripple_rect();
        for (int k = 0; k < nChangedBricks; k++) {
            int i = changedBricks[k];
            float maxR = curR[i] > s->drawnBrickR[i] ? curR[i] : s->drawnBrickR[i];
            dirty[nd++] = brick_rect(s->bricks[i].cx, s->bricks[i].cy, maxR, ROW_BAND_HALF[i / BRICKS_PER_ROW]);
        }

        s->drawnBallX = newBallX; s->drawnBallY = newBallY; s->drawnBallVisible = s->ballVisible;
        s->drawnPaddleX = newPaddleX;
        s->drawnLives = s->lives;
        s->drawnRippleR = curRippleR; s->drawnRippleBand = curRippleBand;
        for (int i = 0; i < N_BRICKS; i++) s->drawnBrickR[i] = curR[i];

        for (int i = 0; i < nd; i++) paint_and_push(s, dirty[i]);
    } else {
        // More changed in one tick than is worth pushing separately (only
        // reachable mid-regrow-wave, when several bricks cross their own
        // stagger boundary inside one coarse tick): one bigger, safer push
        // instead of up to N_BRICKS+4 small ones.
        rect_t u = rect_union(ball_rect(s->drawnBallX, s->drawnBallY), ball_rect(newBallX, newBallY));
        u = rect_union(u, rect_union(paddle_rect(s->drawnPaddleX), paddle_rect(newPaddleX)));
        if (livesChanged) u = rect_union(u, lives_rect());
        if (rippleChanged) u = rect_union(u, ripple_rect());
        for (int k = 0; k < nChangedBricks; k++) {
            int i = changedBricks[k];
            float maxR = curR[i] > s->drawnBrickR[i] ? curR[i] : s->drawnBrickR[i];
            u = rect_union(u, brick_rect(s->bricks[i].cx, s->bricks[i].cy, maxR, ROW_BAND_HALF[i / BRICKS_PER_ROW]));
        }

        s->drawnBallX = newBallX; s->drawnBallY = newBallY; s->drawnBallVisible = s->ballVisible;
        s->drawnPaddleX = newPaddleX;
        s->drawnLives = s->lives;
        s->drawnRippleR = curRippleR; s->drawnRippleBand = curRippleBand;
        for (int i = 0; i < N_BRICKS; i++) s->drawnBrickR[i] = curR[i];

        paint_and_push(s, u);
    }
}

const app_t g_breakoutApp = {
    .name = "breakout",
    .enter = breakout_enter,
    .tick = breakout_tick,
    .leave = NULL,
    .landscape = true,
    // No shake: this app has nothing destructive to undo (a life lost is
    // already its own wordless event, and the wall never resets on its
    // own) and nothing that needs a manual reset the restart tap does not
    // already cover - exactly tiltball.c's own reasoning (decision 0002
    // section 5).
    .wantsShake = false,
};
