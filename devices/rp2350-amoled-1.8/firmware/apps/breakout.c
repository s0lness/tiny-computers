// breakout: a ball that never dies, whacking a rainbow of round bricks it
// bounces around by itself, with a tilted paddle for a two-year-old to bump
// it with. "Casse brique", the owner's own name for it.
//
// =============================================================================
// WHO THIS IS FOR, AND THE ONE DESIGN CALL EVERYTHING ELSE FOLLOWS FROM
// =============================================================================
//
// The owner's brief named the constraint directly: a two-year-old cannot aim,
// so a game where the ball is LOST through imprecision is abandoned inside a
// minute. Real breakout's whole tension is exactly that failure - miss the
// paddle, lose the ball, the game ends - which makes real breakout precisely
// the wrong game for this hand.
//
// So the floor is a WALL, not a goal. All four edges of the play field bounce
// the ball, the same as the top, left and right. There is no life to lose, no
// game over, and therefore nothing here needed a lives counter, a score, or a
// "you lost" picture - the brief's "no text, no numbers" requirement is met
// by having nothing that would ever need one. The paddle stops being the
// single point of failure and becomes what it actually is fun to have it be:
// a big, friendly obstacle she can slide under the ball with a tilt of her
// hand, for the satisfaction of swatting it into the wall on purpose instead
// of watching it wander there by luck. If she never touches it, the ball
// still bounces itself around and still breaks bricks on its own, so the toy
// is never "off" and never waiting on her.
//
// =============================================================================
// CONTROL: TILT ONLY, NOT TOUCH, AND WHY
// =============================================================================
//
// The owner asked for tilt games as their own category, separate from touch
// games, so tilt is this app's whole input surface - the same call
// firmware/apps/level.c already made (decision 0012) for the same reason:
// tilting the puck to move something inside it is a gesture a toddler
// already has, and it needs no glass free to make. This app reads NO touch
// at all, the same way level.c does; frame.touchDown/X/Y are simply never
// looked at. A touch-driven paddle was considered and dropped: a thumb on
// the glass while the other hand tilts the puck is an awkward, two-handed
// ask of exactly the hand that is small here, and it would also mean this
// app needs the measured-dropout touch-gesture testing every touch-driven
// app on this device carries (see AGENTS.md's "A feature driven by touch
// needs BOTH kinds of file") for a control surface that adds nothing tilt
// does not already give for free.
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
// filter, and is never wrong in a way a jump could be.
//
// =============================================================================
// THE WALL IS AN ARC, NOT A GRID - DECISION 0009's TARGET, HIT HEAD ON
// =============================================================================
//
// "A wall of rectangular bricks is the single most ruler-shaped thing in all
// of games" - decision 0009 forbids hard edges, right angles and dead-straight
// lines, and a rectangular brick grid is built from nothing else. So there is
// no grid here: ten round bricks (a "brick" is a filled disc, the cheapest
// shape decision 0009's float brush can carry - see shapes_fill_disc_aa_land's
// own comment on a disc being a zero-length capsule) are laid out along ONE
// gentle arc across the top of the field, like a rainbow, their centres
// computed with sinf/cosf from a fixed peak and half-angle
// (ARC_CX/ARC_CY/ARC_R/ARC_THETA_MAX_DEG below) rather than stepped across
// rows and columns. Nothing about the arrangement is a right angle, and
// nothing about an individual brick is either: it is a circle, anti-aliased
// by coverage exactly like the sketchpad's own ink and the bubble level's
// dial.
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
// The ball is the only shape with no arrangement to speak of: a plain disc,
// the same primitive as a brick, just solid black instead of coloured (see
// "COLOUR" below for why the split is black actors / coloured bricks rather
// than an all-black or all-coloured picture).
//
// =============================================================================
// COLOUR: THE ACTORS STAY BLACK, THE WORLD GETS THE RAINBOW
// =============================================================================
//
// AGENTS.md says to use colour if it earns its place, and names the timer and
// menu icons as deliberately one ink. Connect Four earns colour by using it
// to carry real state (whose piece is whose); here it is used to make ten
// discs read as ten DIFFERENT round bricks rather than one shape stamped ten
// times, and to make a breaking wall look like it is actually thinning out
// rather than merely getting smaller. That is worth the colour. What stays
// black is the ball and the paddle - the two things that MOVE, the "pen" of
// this app the way the sketchpad's own ink is - so the picture keeps the
// device's usual one-ink read for the parts that act, and spends colour only
// on the parts that get acted on.
//
// =============================================================================
// THE GAME LOOP: ONE FIXED-TIMESTEP CLOCK, NOT A DTMS EULER STEP
// =============================================================================
//
// Decision 0010 flagged this exact trap before any game existed to fall into
// it: "today's tick rate is 'as fast as the loop spins'... a game tuned in
// the emulator plays at a different speed on the board, and at a different
// speed depending on how much it drew last frame", and asked for "one
// fixed-timestep accumulator helper... that all three games share." This is
// the first game built after that was written, so this file is where that
// helper is built, in the shape decision 0010 asked for; nothing existed to
// share it with yet, but nothing here is app-specific about the technique
// either; a next game can lift `sim_step`'s calling convention wholesale.
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
// her to see it.
//
// =============================================================================
// PER-FRAME COST AND RESIDUE, THE WAY EVERY OTHER APP HERE PROVES IT
// =============================================================================
//
// Nothing here erases anything. Exactly like firmware/apps/level.c,
// paint_rect() recomputes every pixel of a dirty rectangle from the CURRENT
// model (ball, paddle, every brick's current radius) rather than patching
// around an old shape, so an old position cannot survive a repaint of the
// ground it stood on - residue is impossible by construction, not by
// bookkeeping, and it is the same reason level.c gives for the same
// technique. The dirty rectangle is the union of whatever actually changed
// this tick (the ball's old and new box, the paddle's old and new box, and
// each brick whose drawn radius changed), consolidated into at most six
// pushes; if more than that changes in one tick (only reachable during the
// wall's regrow wave, when several bricks can cross their own grow schedule
// inside a single coarse tick) the fallback is one big union rect instead -
// still correct, just less surgical, and still nowhere near the panel's
// per-tick budget. See "MEASURED COST" in the task report for the actual
// numbers `bun run tools/gate/run.ts --measure` prints for this app.
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
// Whether ten bricks at fifteen pixels each read as "a rainbow" or as "some
// dots" from across a room to a toddler - decision 0009's own rule about
// whether a picture is any good is checked by no tool in this tree; the
// contact sheet (preview/breakout-*.png, tools/preview-breakout.ts) is where
// that question has to be judged, by an eye, the same as every icon on this
// device already is.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

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

/* ---- the wall: ten discs on one arc -------------------------------- */
#define N_BRICKS 10
#define BRICK_R 15.0f
#define ARC_CX 224.0f          // LAND_W / 2
#define ARC_PEAK_Y 60.0f       // topmost brick's centre y
#define ARC_R 260.0f           // the arc's own (off-screen, below) radius
#define ARC_CY (ARC_PEAK_Y + ARC_R)     // 320
#define ARC_THETA_MAX_DEG 36.0f // half-angle: end bricks sit near x=70/378, y=~113

/* ---- speed ------------------------------------------------------------ */
#define BALL_SPEED_BASE 0.16f // px/ms
#define BALL_SPEED_MAX  0.30f
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
 * write - see paint_rect). The actors (ball, paddle) stay pure black; only
 * bricks get the palette. See the header comment's "COLOUR" section.
 * ------------------------------------------------------------------- */
#define RGB565_WHITE 0xFFFFu
#define RGB565_BLACK 0x0000u

static const uint16_t BRICK_PALETTE[8] = {
    0xF800u, // red
    0xFCA0u, // orange
    0xFFE0u, // yellow
    0x07E0u, // green
    0x07FFu, // cyan
    0x001Fu, // blue
    0x781Fu, // violet
    0xF81Fu, // magenta
};

/* ---------------------------------------------------------------------
 * State: one struct from the arena (app.h), never file-scope.
 * ------------------------------------------------------------------- */
typedef struct {
    float cx, cy;    // fixed for life; only ever set once, in enter()
    uint16_t color;
    bool alive;       // true = fully present and collidable
} brick_t;

typedef struct {
    bool     inited;
    uint32_t simMs;   // the fixed-timestep clock (header comment)

    float ballX, ballY, ballVX, ballVY;
    float paddleX;

    brick_t bricks[N_BRICKS];
    int     aliveCount;

    bool     celebrating;
    uint32_t celebrateStartMs; // simMs at the tick the last brick broke

    // What is actually ON SCREEN right now. paint_rect() renders exactly
    // this, which is what makes an incrementally-updated frame identical to
    // a freshly-entered one - same argument and same technique as
    // level.c's drawn* fields.
    float drawnBallX, drawnBallY;
    float drawnPaddleX;
    float drawnBrickR[N_BRICKS];
} breakout_state_t;

static breakout_state_t *s_state;

/* ---------------------------------------------------------------------
 * Colour blending. Unlike level.c (grey-only, MIN composition), bricks need
 * a real colour composited under black ink, so this is a small back-to-
 * front alpha blend instead: paper white, then the best-covering brick (if
 * any), then the paddle, then the ball - each a single mix by that shape's
 * own anti-aliased coverage. Three layers, always in this order, never more
 * than three per pixel.
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

            // The wall: whichever brick covers this pixel most. Bricks
            // never overlap each other by construction (the arc's own
            // spacing, chosen with room to spare), so "most" is really
            // "the only one that can", found with a cheap box reject
            // before any sqrtf.
            {
                float bestCov = 0.0f;
                uint16_t bestColor = RGB565_WHITE;
                for (int i = 0; i < N_BRICKS; i++) {
                    float br = s->drawnBrickR[i];
                    if (br <= 0.01f) continue;
                    float dxk = fx - s->bricks[i].cx;
                    if (fabsf(dxk) > br + 2.0f) continue;
                    float dyk = fy - s->bricks[i].cy;
                    if (fabsf(dyk) > br + 2.0f) continue;
                    float d2 = dxk * dxk + dyk * dyk;
                    float rr = br + 1.5f;
                    if (d2 > rr * rr) continue;
                    float d = sqrtf(d2) - br;
                    float c = coverage_from_dist(d);
                    if (c > bestCov) { bestCov = c; bestColor = s->bricks[i].color; }
                }
                if (bestCov > 0.0f) outv = rgb565_mix(outv, bestColor, bestCov);
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

            // The ball, always on top.
            {
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

static rect_t brick_rect(float cx, float cy, float r_) {
    rect_t r;
    r.x0 = (int)floorf(cx - r_ - 1.5f);
    r.y0 = (int)floorf(cy - r_ - 1.5f);
    r.x1 = (int)ceilf(cx + r_ + 1.5f);
    r.y1 = (int)ceilf(cy + r_ + 1.5f);
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

static void ball_wall_bounce(breakout_state_t *s) {
    if (s->ballX - BALL_R < PLAY_L) { s->ballX = PLAY_L + BALL_R; if (s->ballVX < 0.0f) s->ballVX = -s->ballVX; }
    else if (s->ballX + BALL_R > PLAY_R) { s->ballX = PLAY_R - BALL_R; if (s->ballVX > 0.0f) s->ballVX = -s->ballVX; }
    if (s->ballY - BALL_R < PLAY_T) { s->ballY = PLAY_T + BALL_R; if (s->ballVY < 0.0f) s->ballVY = -s->ballVY; }
    else if (s->ballY + BALL_R > PLAY_B) { s->ballY = PLAY_B - BALL_R; if (s->ballVY > 0.0f) s->ballVY = -s->ballVY; }
}

// The paddle is a solid obstacle from either face (it now sits inside the
// open field rather than guarding a floor - see header comment), so this
// checks distance to its own spine and bounces off whichever side the ball
// is approaching from, rather than assuming "from above" the way a classic
// breakout paddle can.
static void ball_paddle_bounce(breakout_state_t *s) {
    float dx = s->ballX - s->paddleX;
    float cdx = dx;
    if (cdx < -PADDLE_HALF_W) cdx = -PADDLE_HALF_W; else if (cdx > PADDLE_HALF_W) cdx = PADDLE_HALF_W;
    float nearX = s->paddleX + cdx;
    float ddx = s->ballX - nearX, ddy = s->ballY - PADDLE_Y;
    float rr = BALL_R + PADDLE_THICK_HALF + 1.0f;
    float d2 = ddx * ddx + ddy * ddy;
    if (d2 >= rr * rr) return;

    bool fromAbove = ddy <= 0.0f;
    // Only bounce a ball moving TOWARD this face: stops a ball that is
    // already leaving from re-triggering on the next fixed step while it is
    // still inside the hit radius.
    if (fromAbove ? (s->ballVY <= 0.0f) : (s->ballVY >= 0.0f)) return;

    float nx, ny;
    if (dx > -PADDLE_HALF_W && dx < PADDLE_HALF_W) {
        // Within the paddle's own flat middle: the nearest point is
        // straight up or down, so the normal is simply vertical - cheaper
        // and more stable than normalising a near-zero (ddx,ddy) here.
        nx = 0.0f; ny = fromAbove ? -1.0f : 1.0f;
    } else {
        float d = sqrtf(d2);
        if (d < 0.0001f) d = 0.0001f;
        nx = ddx / d; ny = ddy / d;
    }

    float pushDist = rr + 0.5f;
    s->ballX = nearX + nx * pushDist;
    s->ballY = PADDLE_Y + ny * pushDist;

    float dot = s->ballVX * nx + s->ballVY * ny;
    s->ballVX -= 2.0f * dot * nx;
    s->ballVY -= 2.0f * dot * ny;

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

static void sim_step(breakout_state_t *s, float dtMs, uint32_t simMsNow) {
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
    s->ballX = ARC_CX;
    s->ballY = PADDLE_Y - 100.0f;
    float a = BALL_START_ANGLE_DEG * BREAKOUT_PI / 180.0f;
    s->ballVX = BALL_SPEED_BASE * cosf(a);
    s->ballVY = -BALL_SPEED_BASE * sinf(a);

    s->paddleX = PADDLE_CENTER_X;

    float thetaMax = ARC_THETA_MAX_DEG * BREAKOUT_PI / 180.0f;
    for (int i = 0; i < N_BRICKS; i++) {
        float t = (N_BRICKS == 1) ? 0.0f : ((float)i / (float)(N_BRICKS - 1)) * 2.0f - 1.0f; // -1..1
        float theta = t * thetaMax;
        s->bricks[i].cx = ARC_CX + ARC_R * sinf(theta);
        s->bricks[i].cy = ARC_CY - ARC_R * cosf(theta);
        s->bricks[i].color = BRICK_PALETTE[i % 8];
        s->bricks[i].alive = true;
    }
    s->aliveCount = N_BRICKS;
    s->celebrating = false;

    s->drawnBallX = s->ballX;
    s->drawnBallY = s->ballY;
    s->drawnPaddleX = s->paddleX;
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
    // freshest reading available.
    {
        float gx = f->tilt.valid ? f->tilt.gx : 0.0f;
        float frac = gx / PADDLE_GX_FULL;
        if (frac < -1.0f) frac = -1.0f; else if (frac > 1.0f) frac = 1.0f;
        s->paddleX = PADDLE_CENTER_X + frac * PADDLE_TRAVEL_MAX;
    }

    if (f->nowMs - s->simMs > BACKLOG_CAP_MS) s->simMs = f->nowMs - BACKLOG_CAP_MS;
    while (s->simMs + FIXED_DT_MS <= f->nowMs) {
        s->simMs += FIXED_DT_MS;
        sim_step(s, (float)FIXED_DT_MS, s->simMs);
    }

    float curR[N_BRICKS];
    compute_visible_brick_radii(s, curR);

    float newBallX = quantise(s->ballX), newBallY = quantise(s->ballY);
    float newPaddleX = quantise(s->paddleX);

    bool ballChanged = (newBallX != s->drawnBallX) || (newBallY != s->drawnBallY);
    bool paddleChanged = (newPaddleX != s->drawnPaddleX);
    int changedBricks[N_BRICKS], nChangedBricks = 0;
    for (int i = 0; i < N_BRICKS; i++) {
        if (curR[i] != s->drawnBrickR[i]) changedBricks[nChangedBricks++] = i;
    }

    int totalItems = (ballChanged ? 1 : 0) + (paddleChanged ? 1 : 0) + nChangedBricks;
    if (totalItems == 0) return; // the same picture: no paint, no push

    if (totalItems <= 6) {
        rect_t dirty[8];
        int nd = 0;
        if (ballChanged) dirty[nd++] = rect_union(ball_rect(s->drawnBallX, s->drawnBallY), ball_rect(newBallX, newBallY));
        if (paddleChanged) dirty[nd++] = rect_union(paddle_rect(s->drawnPaddleX), paddle_rect(newPaddleX));
        for (int k = 0; k < nChangedBricks; k++) {
            int i = changedBricks[k];
            float maxR = curR[i] > s->drawnBrickR[i] ? curR[i] : s->drawnBrickR[i];
            dirty[nd++] = brick_rect(s->bricks[i].cx, s->bricks[i].cy, maxR);
        }

        s->drawnBallX = newBallX; s->drawnBallY = newBallY;
        s->drawnPaddleX = newPaddleX;
        for (int i = 0; i < N_BRICKS; i++) s->drawnBrickR[i] = curR[i];

        for (int i = 0; i < nd; i++) paint_and_push(s, dirty[i]);
    } else {
        // More changed in one tick than is worth pushing separately (only
        // reachable mid-regrow-wave, when several bricks cross their own
        // stagger boundary inside one coarse tick): one bigger, safer push
        // instead of up to N_BRICKS+2 small ones.
        rect_t u = rect_union(ball_rect(s->drawnBallX, s->drawnBallY), ball_rect(newBallX, newBallY));
        u = rect_union(u, rect_union(paddle_rect(s->drawnPaddleX), paddle_rect(newPaddleX)));
        for (int k = 0; k < nChangedBricks; k++) {
            int i = changedBricks[k];
            float maxR = curR[i] > s->drawnBrickR[i] ? curR[i] : s->drawnBrickR[i];
            u = rect_union(u, brick_rect(s->bricks[i].cx, s->bricks[i].cy, maxR));
        }

        s->drawnBallX = newBallX; s->drawnBallY = newBallY;
        s->drawnPaddleX = newPaddleX;
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
    // No shake: this app has nothing destructive to undo and nothing that
    // needs a manual reset (the wall regrows itself), so shake stays the
    // sketchpad's own gesture rather than becoming a universal verb -
    // exactly level.c's own reasoning (decision 0002 section 5).
    .wantsShake = false,
};
