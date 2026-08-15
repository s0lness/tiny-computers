/*
 * dino: the owner's brief, verbatim: "Le jeu du Dino de chrome (tap pour
 * jump, on garde le high score)". One runner, obstacles that come at her,
 * tap anywhere to jump, it gets faster, hitting something ends the run,
 * and it restarts without a word of text. Landscape, like chrono/timer/
 * four - held sideways, buttons along the top edge.
 *
 * WHO THIS IS FOR, and why several of Chrome's own choices are deliberately
 * NOT copied here (AGENTS.md's task brief, not relitigated per app): a
 * two-year-old, reaction time nothing like an adult's. Chrome's dino keeps
 * accelerating without a ceiling, which makes the game a twitch-reflex test
 * within a couple of minutes - fun for an adult, a wall of frustration for
 * a toddler who cannot yet map "the ground bump is coming" to "tap now" in
 * under a second. Three things are tuned away from Chrome's own feel on
 * purpose:
 *
 *   1. START_SPEED_PXMS/MAX_SPEED_PXMS are both far below Chrome's, and
 *      MAX_SPEED_PXMS is a real ceiling, not an asymptote climbing forever.
 *   2. The obstacle gap is a TIME budget (MIN_GAP_MS/MAX_GAP_MS), not a
 *      pixel one, so a faster run does not also mean less REACTION time per
 *      obstacle - only one obstacle is ever on screen at once (see "ONE
 *      OBSTACLE AT A TIME" below), so what "gets faster" buys is a livelier
 *      scene, not a harder puzzle.
 *   3. Hitboxes are smaller than the silhouettes they sit inside (see
 *      "COLLISION" below) - a near-miss LOOKS like a hit and isn't one,
 *      which is the direction a game for small hands should err in.
 *
 * TAP TO JUMP, EVERYWHERE ON THE GLASS. No button to find, no target to
 * aim at - the device's own "the target the whole panel is legible from"
 * idiom, applied to the one gesture this app needs. f->touchX/Y are never
 * read.
 *
 * THE FT3168 DOES NOT REPORT ONE PRESS PER TAP (AGENTS.md: ~34 dropout
 * episodes/sec while a finger is down, occasional stray contacts with
 * nothing touching). A jump trigger built on the runtime's raw
 * touchPressed would misfire - once for the real tap, again for every
 * dropout-then-redown while she is still holding - and a phantom jump
 * fired by a stray at the wrong instant can cost a run she did nothing to
 * lose. THE ARMING LOGIC BELOW (tap_tick()) IS APPS/FOUR.C'S OWN, REUSED
 * VERBATIM IN SHAPE AND CONSTANTS (TAP_ARM_SAMPLES/TAP_ARM_MS/
 * TAP_ARM_RATE_HZ mirror four.c's ARM_SAMPLES/ARM_MS/ARM_RATE_HZ;
 * TAP_RELEASE_GRACE_MS mirrors its RELEASE_GRACE_MS) - see four.c's own
 * header section 2 for the arithmetic this rests on; this file does not
 * re-derive it. One physical press, however long held through however many
 * dropouts, fires exactly one action; a burst of fewer than
 * TAP_ARM_SAMPLES phantom contacts can never fire one at all; and the next
 * action needs a GENUINE lift (silence for TAP_RELEASE_GRACE_MS) before it
 * can arm again - which is also, for free, what stops the very tap that
 * caused a collision from also being read as the restart tap: the
 * arm-state is still "armed" from that same physical hold, so nothing
 * fires again until she actually lets go.
 *
 * ONE OBSTACLE AT A TIME, ON PURPOSE. Chrome clusters cacti and adds a low
 * flying pterodactyl that needs a duck gesture this app deliberately does
 * not have (the brief is tap-to-jump, one input, full stop). A single
 * obstacle, spaced by a generous time budget, is not a simplification of
 * the difficulty - it IS the difficulty design: every obstacle gets her
 * full, uncrowded attention.
 *
 * THE HIGH SCORE, WORDLESSLY. She cannot read a number, so there is no
 * number. What she gets instead is a little marker post standing at the
 * distance her best-ever run reached, planted in the world at that exact
 * spot: it scrolls toward her exactly like an obstacle does, and the
 * instant she runs past it - LIVE, mid-run, not on a results screen - a
 * soft ring blooms where she is standing. That is the whole vocabulary:
 * a post she has never reached before is a new thing to run at; catching
 * up to it and then leaving it behind, with a visible little flourish
 * exactly when it happens, is what "you just beat your best" looks like
 * with no words in it. See "THE FLAG" below.
 *
 * RESTART, WORDLESSLY. Death is a beat of stillness (DEATH_FREEZE_MS - the
 * same idiom apps/four.c uses for "nothing happened", inverted: here
 * something very much just happened, and a scene that was constantly
 * moving suddenly not moving at all is itself the news, the same way it
 * would be if a wind-up toy stopped), then the dino settles into the exact
 * standing pose the app opens on. The SAME tap gesture that jumps restarts
 * it - one gesture, always available, always doing something - so there
 * is nothing to discover beyond "touching this thing does something",
 * which every other app on this device has already taught her.
 *
 * REDRAW STRATEGY: ONE SHARED DIRTY RECTANGLE, NOT FOUR. The dino, the
 * current obstacle and the distance flag all move (or can move) every
 * tick, and the flag and an obstacle keep a CONSTANT relative screen
 * position for as long as both are visible (both scroll by the same
 * traveled-distance subtraction), so if either one spawns near the
 * other's world position they overlap for their whole shared lifetime, not
 * for one unlucky tick. Clearing and redrawing each of them through its
 * OWN separately tracked rectangle would then erase whichever one was
 * drawn first wherever they overlap, on every tick they overlap - a
 * correctness bug, not a cosmetic one, and exactly the shape decision 0010
 * warns a first-of-class bug takes (found by reasoning about the geometry,
 * since there is no board here to catch it by eye). So every tick this app
 * draws computes ONE union rectangle covering whatever was on screen last
 * tick plus whatever belongs on screen this tick (dino always; the
 * obstacle and the flag when in range; the ripple when active), clears
 * exactly that rectangle, redraws everything that belongs in it, and
 * pushes it once. The cost of the simplification is a bigger single push
 * while an obstacle is far from the dino (up to most of the play width);
 * tools/gate/run.ts's own measured budget (three full panels per tick) has
 * three-and-a-half times that to spare - see this file's own --measure
 * numbers in the shipping report. Skipped entirely once the scene is
 * static (the DEAD_WAIT idle screen), via g_dirty below, the same "only
 * push what changed" discipline chrono.c's per-digit cells and four.c's
 * per-column repaint both already use, just at the grain of "the whole
 * scene" instead of "one cell".
 *
 * WHAT THIS FILE READS, AND WHAT IT IS THEREFORE BLIND TO (decision 0010's
 * required paragraph for a new capability). The persistence path
 * (storage_save_u32()) is exercised here only up to the call: this file
 * never reads back through flash_safe_execute()'s own success/failure or
 * measures a real sector erase - see firmware/runtime/storage.c and
 * docs/decisions/0011 for what actually happens on the board and what is
 * still unverified there. The jump-timing numbers (GRAVITY_PXMS2,
 * JUMP_V0_PXMS, the speed ramp) are tuned against wall-clock milliseconds
 * (f->nowMs/dtMs) exactly the way apps/four.c's falling piece is, which
 * the emulator can execute correctly and can never judge as "does this
 * FEEL right" - decision 0003's forbidden question, asked again here in a
 * new shape: only a real thumb on the real panel, at the real achieved
 * tick rate, can say whether the jump window feels fair. Nothing in this
 * tree can.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "app.h"
#include "gfx.h"
#include "shapes.h"
#include "storage.h"

/* =========================================================================
 * Layout, landscape (448 x 368). GROUND_Y is the local y=0 every silhouette
 * below is built against; DINO_X is her fixed horizontal position - she
 * never moves in x, the world moves past her, exactly like Chrome's own.
 * ======================================================================= */
#define GROUND_Y 310
#define DINO_X   120

/* ---- jump physics --------------------------------------------------------
 * Solved from two numbers worth tuning by feel rather than the four this
 * would otherwise be: JUMP_AIRTIME_MS (how long a jump lasts, i.e. how much
 * warning she needs before an obstacle to jump in time) and JUMP_APEX_PX
 * (how high it goes, i.e. how much margin the tallest obstacle's hitbox
 * leaves - see OBSTACLE_KINDS and "COLLISION" below). Standard projectile
 * algebra for a jump that lands back at the height it left from:
 *   apex h = v0^2/(2g), airtime T = 2 v0/g  =>  g = 8h/T^2,  v0 = gT/2.
 */
#define JUMP_APEX_PX     95.0f
#define JUMP_AIRTIME_MS  600.0f
#define GRAVITY_PXMS2    (8.0f * JUMP_APEX_PX / (JUMP_AIRTIME_MS * JUMP_AIRTIME_MS))
#define JUMP_V0_PXMS     (GRAVITY_PXMS2 * JUMP_AIRTIME_MS * 0.5f)

/* ---- the difficulty curve -------------------------------------------------
 * See this file's header for why these are far below Chrome's own and why
 * MAX_SPEED_PXMS is a real ceiling. SPEED_RAMP_MS is how long a single run
 * takes to go from START to MAX at constant play - about two minutes,
 * which is a long run for this audience already.
 */
#define START_SPEED_PXMS 0.070f
#define MAX_SPEED_PXMS   0.160f
#define SPEED_RAMP_MS    120000.0f
#define ACCEL_PXMS2      ((MAX_SPEED_PXMS - START_SPEED_PXMS) / SPEED_RAMP_MS)

// A TIME budget, not a pixel one - see this file's header, point 2. Chosen
// so that even at MAX_SPEED_PXMS an obstacle spawned just off the right
// edge gives at least (LAND_W-DINO_X)/MAX_SPEED_PXMS =~ 2.2s before it
// reaches her, comfortably inside the smaller of these two.
#define MIN_GAP_MS 1600u
#define MAX_GAP_MS 2400u

/* ---- the tap gesture: apps/four.c's arming/release-grace idiom, reused
 * verbatim (see this file's header). ------------------------------------ */
#define TAP_ARM_SAMPLES      4
#define TAP_ARM_MS           40u
#define TAP_ARM_RATE_HZ      15u
#define TAP_RELEASE_GRACE_MS 300u

// The beat of stillness after a collision, before a tap is read as
// "restart" rather than ignored - see this file's header.
#define DEATH_FREEZE_MS 550u

// One running "stride" (a leg-phase flip) every this many world pixels, so
// the gait visibly quickens as the world speeds up rather than ticking at
// a fixed cadence regardless of how fast she is going.
#define LEG_STRIDE_PX 30.0f

/* ---- the record ripple: a ring that blooms in place, ANCHORED TO THE
 * DINO'S OWN FIXED SCREEN POSITION, not to any world coordinate - it marks
 * "something just happened here", not a location the scrolling world
 * should remember. Kept inside the dino's own always-redrawn rectangle
 * (DINO_RECT_* below already has margin for it) rather than tracked
 * separately - see "REDRAW STRATEGY" above. */
#define RIPPLE_DURATION_MS 380u
#define RIPPLE_MAX_R       40.0f
#define RIPPLE_MAX_BAND    8.0f
#define RIPPLE_LOCAL_X     30.0f
#define RIPPLE_LOCAL_Y     -50.0f

/* ---- the dino's own always-on-screen rectangle. Fixed, not tracked frame
 * to frame: her x never moves, and every pose (grounded, airborne at full
 * JUMP_APEX_PX, mid-stride, standing, plus the ripple above) fits inside
 * this one box with margin for anti-aliasing bleed - see draw_dino()'s own
 * local coordinates for where each number below comes from.
 *
 * EVERY BOUND HERE IS AN ENDPOINT PLUS ITS OWN CAP RADIUS, NOT JUST THE
 * ENDPOINT. shapes_fill_capsule_aa_land's round cap bulges the shape's own
 * radius past the coordinate it was called with - the head capsule's tip
 * is at local y=-90 but its ink reaches y=-90-9=-99, and it is that -99
 * (not -90) that has to clear the panel's top edge at full jump height.
 * Missing this the first time this file was written shipped a real bug:
 * the gate's pushed-or-invisible rule caught ink appearing a few pixels
 * above this rectangle on a full-height jump (tools/gate/run.ts, "dino/tap
 * top-right"), because DINO_RECT_Y0 was built from the raw endpoint alone.
 */
// Re-derived for the third-pass silhouette (see draw_dino), which is wider
// and much shorter than the one these numbers were first written for. Each
// bound is the extreme point of a real shape plus 4px of pad, not a guess:
//   X0  tail tip (-88) - its own radius (5) - pad
//   X1  snout centre (+80) + its own radius (14) + pad
//   Y0  head centre (-106) - its own radius (22) - pad, above the apex
// X0 stops 23px clear of the bezel band at DINO_X=120, which is the reason
// the tail was sized to 88 rather than pushed further back.
#define DINO_RECT_X0 -102
#define DINO_RECT_X1  98
#define DINO_RECT_Y0 (-(JUMP_APEX_PX) - 132.0f)
#define DINO_RECT_Y1  12

// Obstacles and the flag are both round-cornered posts on the ground; one
// box shape covers either, sized for the taller of the two obstacle kinds
// (trunk top -106, minus pad).
#define PROP_HALF_W       36
#define PROP_TOP_LOCAL    -114
#define PROP_BOTTOM_LOCAL    6

/* ---- the visible playfield. PANEL_BEZEL_MARGIN_PX (gfx.h) hides the
 * outer 10px on every physical edge, and unlike every other app here
 * (whose content is drawn once, inset, and stays put), an obstacle or the
 * flag SLIDES continuously across the whole landscape width - so unless
 * it is explicitly kept clear of the last 10px on both sides, it spends
 * real time with its silhouette (or part of it) under the case, invisible
 * to her, which is exactly what gfx.h's own comment on this margin forbids
 * ("nothing a child needs to actually see" belongs there). The fix is not
 * a bigger rectangle - it is never letting a prop's silhouette overlap the
 * band at all: it "arrives" already standing just inside the visible right
 * edge (PLAYFIELD_RIGHT - PROP_HALF_W, not off past LAND_W) and stops
 * being drawn the instant its own silhouette would start crossing into the
 * left band, rather than only once it is fully gone. tools/gate/run.ts's
 * `bezel` rule caught the first version of this file, which spawned an
 * obstacle off past LAND_W and let it slide through the right-hand band on
 * the way in. */
#define BEZEL PANEL_BEZEL_MARGIN_PX
#define PLAYFIELD_LEFT  (BEZEL)
#define PLAYFIELD_RIGHT (LAND_W - BEZEL)

/* =========================================================================
 * A tiny xorshift32 PRNG - which obstacle kind, and how long until the
 * next one. Reseeded from f->nowMs at the start of every run (not just
 * once at boot), so consecutive runs in one sitting do not replay the same
 * obstacle sequence.
 * ======================================================================= */
static uint32_t rng_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}
static uint32_t rng_range(uint32_t *state, uint32_t lo, uint32_t hi) {
    return lo + (rng_next(state) % (hi - lo + 1u));
}

/* =========================================================================
 * The tap gesture. See this file's header - identical shape to
 * apps/four.c's arming state machine, generalised from "arm a slide" to
 * "arm one action per physical press".
 * ======================================================================= */
typedef struct {
    bool     everContacted;
    bool     armed;         // this physical press has already fired
    int      contactCount;
    uint32_t firstContactMs;
    uint32_t lastContactMs;
} tap_state_t;

// Returns true on the single tick a fresh, dropout/stray-immune press just
// armed - see four.c's header section 2 for why ARM_SAMPLES/ARM_MS/
// ARM_RATE_HZ together (not either alone) are what a stray can never fake.
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
        // A genuine lift: contact has been absent long enough that this is
        // not just another dropout inside the same physical hold.
        g->everContacted = false;
        g->armed = false;
        g->contactCount = 0;
    }
    return fired;
}

/* =========================================================================
 * Obstacles: one at a time (see this file's header). Two silhouettes, both
 * a tapered trunk plus one or two tapered arms curving off it - the same
 * float-brush vocabulary as every other shape in this app and, per
 * decision 0009, everywhere on this device that is not one of the four
 * Lucide menu icons. Trunk endpoints sit at LOCAL y = -radius at the
 * ground end, not y = 0, so the round cap's own bulge lands exactly ON the
 * ground rather than poking a rounded belly below it - the same
 * consideration draw_dino()'s feet apply below.
 * ======================================================================= */
typedef struct {
    float baseR, topR, trunkH;
    int   armCount;
    float armBaseY[2]; // local y, trunk-relative
    float armSide[2];  // +1 right, -1 left
    float armReach, armRise, armTipUp;
} obstacle_kind_t;

static const obstacle_kind_t OBSTACLE_KINDS[2] = {
    // small: one right-hand arm.
    { 15.0f, 9.0f, 64.0f, 1, { -28.0f, 0.0f }, { 1.0f, 0.0f }, 20.0f, 14.0f, 12.0f },
    // tall: two arms, opposite sides, different heights.
    { 16.0f, 10.0f, 96.0f, 2, { -40.0f, -66.0f }, { 1.0f, -1.0f }, 22.0f, 16.0f, 14.0f },
};

static void draw_obstacle_kind(float ox, int kindIndex) {
    const obstacle_kind_t *k = &OBSTACLE_KINDS[kindIndex];
    float trunkBaseY = GROUND_Y - k->baseR;
    float trunkTopY  = GROUND_Y - k->trunkH;
    shapes_fill_capsule_aa_land(ox, trunkBaseY, k->baseR, ox, trunkTopY, k->topR, PX_BLACK);
    for (int i = 0; i < k->armCount; i++) {
        float baseY = GROUND_Y + k->armBaseY[i];
        float side = k->armSide[i];
        float cx = ox + side * (k->armReach * 0.6f);
        float cy = baseY - k->armRise;
        float tx = ox + side * k->armReach;
        float ty = baseY - k->armRise - k->armTipUp;
        shapes_fill_capsule_aa_land(ox, baseY, k->baseR * 0.55f, cx, cy, k->topR * 0.8f, PX_BLACK);
        shapes_fill_capsule_aa_land(cx, cy, k->topR * 0.8f, tx, ty, 4.0f, PX_BLACK);
    }
}

/* =========================================================================
 * The flag: a marker post at the distance her best run reached. Pole +
 * round finial - a "lollipop", deliberately, so the whole shape is
 * capsules and one disc and needs no straight-edged pennant (decision
 * 0009). See this file's header, "THE HIGH SCORE, WORDLESSLY".
 * ======================================================================= */
static void draw_flag(float ox) {
    shapes_fill_capsule_aa_land(ox, GROUND_Y - 4.0f, 4.0f, ox, GROUND_Y - 70.0f, 2.0f, PX_BLACK);
    shapes_fill_disc_aa_land(ox, GROUND_Y - 80.0f, 10.0f, PX_BLACK);
}

/* =========================================================================
 * The dino herself. A rounded torso, a round head with a round snout
 * bump, a tail sweeping back and up, two sturdy legs, a shrinking-pill
 * contact shadow. Every joint is round; nothing here is a straight edge
 * or a right angle (decision 0009).
 *
 * SIZED TO BE THE MAIN THING ON THE SCREEN, deliberately - the first pass
 * at this shape (see this file's git history) was built at roughly half
 * this scale and, judged against a real render (tools/preview-dino.ts,
 * decision 0010's "no metric confirms a picture" argument - nothing
 * automated here would ever have caught this), read as a small bird or a
 * platypus lost in the corner of the panel rather than as a dinosaur she
 * is playing as. A head built from one tapered capsule blending straight
 * into the body read as a beak; splitting it into a round head PLUS a
 * separate round snout bump (two discs, not one capsule) is what actually
 * reads as a face in profile.
 *
 * `liftPx` is how far she has risen above the ground this instant (0 =
 * grounded). The whole upper body translates up with it; the legs do not -
 * grounded legs reach down to the true ground, airborne legs tuck close to
 * the risen body, which is what actually reads as "jumping" rather than
 * "the whole drawing moved up".
 * ======================================================================= */
static void draw_dino(float ox, float liftPx, bool running, float traveledPx) {
    float oy = GROUND_Y - liftPx; // the risen body's own local origin

    // THIRD PASS, and the reason for it: the first two both built a tall
    // vertical stack (hip 50 up, chest 90, head 158), which is the shape of
    // a hunched bird, not of a dinosaur. Every test passed both times,
    // because a test counts pixels and cannot read a picture.
    //
    // What actually makes Chrome's dino legible in one glance is not its
    // detail, it is its PROPORTION: the mass is horizontal, the head is
    // carried forward rather than stacked on top, and a long tail behind
    // balances it. Wider than tall is the whole trick. This silhouette is
    // ~187px across against ~132 tall; the previous one was 164 across
    // against 184 tall, i.e. portrait, which is why it read as a bird.
    //
    // Everything below is sized inward from DINO_RECT_X0/X1 rather than the
    // rectangle being widened to fit it: the box already reaches within
    // 23px of the bezel band on the left, and a silhouette that outgrows
    // its own pushed rectangle is decision 0001's exact failure.

    // Body: a long, near-horizontal barrel from hip (back, low) to chest
    // (front, slightly higher). This is the mass the eye reads first.
    float hipX = ox - 6.0f,   hipY = oy - 62.0f;
    float chX  = ox + 36.0f,  chY  = oy - 72.0f;
    shapes_fill_capsule_aa_land(hipX, hipY, 30.0f, chX, chY, 25.0f, PX_BLACK);

    // Neck: SHORT and forward-leaning now (40px, was 68). A long neck is
    // what pushed the head up into the bird silhouette; carrying it forward
    // over the chest instead is what puts the weight in front of the legs,
    // which is the pose a running theropod actually holds.
    float headX = ox + 58.0f, headY = oy - 106.0f;
    shapes_fill_capsule_aa_land(chX, chY, 25.0f, headX, headY, 15.0f, PX_BLACK);

    // Head (a disc, not a tapered capsule - see this function's own header)
    // plus a snout bump forward and slightly below it, the two together
    // reading as a blunt animal profile pointing the way she is running.
    shapes_fill_disc_aa_land(headX, headY, 22.0f, PX_BLACK);
    shapes_fill_disc_aa_land(ox + 80.0f, oy - 100.0f, 14.0f, PX_BLACK);

    // Tail: long and near-horizontal, sweeping BACK from the hip and
    // rising only slightly, so it counterweights the forward head instead
    // of pointing away from the body the way the previous pass's short
    // steep spike did. It is the single biggest contributor to reading as
    // a dinosaur, and it is 88px long against the body's own 42.
    // It starts at the body's own BACK EDGE (hip - 30 = the belly capsule's
    // radius), not at the hip point: a tail rooted at the hip spends its
    // first 30px inside the belly, so only the thin outer end survives and
    // it renders as a flat sliver rather than as a taper. Rooting it where
    // the body actually ends is what makes the taper visible against paper.
    shapes_fill_tapered_quad_aa_land(ox - 30.0f, oy - 64.0f, 26.0f,
                                      ox - 62.0f, oy - 72.0f,
                                      ox - 92.0f, oy - 86.0f, 6.0f, PX_BLACK);

    // The little forearm. Too small to be a limb at this size and that is
    // the point: a stubby arm under a big head is the one cue that says
    // "theropod" rather than "some animal", and it costs one capsule.
    shapes_fill_capsule_aa_land(ox + 40.0f, oy - 74.0f, 8.0f,
                                 ox + 58.0f, oy - 62.0f, 5.0f, PX_BLACK);

    // Legs. Attached BELOW the torso's own visible silhouette (legHipY is
    // closer to the ground than hipY + the torso's radius), not at the
    // same point the torso itself pivots from - see this function's own
    // header on why that distinction is what makes them a visible pair of
    // limbs rather than a hidden continuation of the belly. Grounded:
    // alternate a back/front pair by world distance travelled, so the
    // cadence speeds up with the run itself. Airborne: tuck both close to
    // the body.
    float legHipX = ox + 14.0f, legHipY = oy - 30.0f;
    float footAX, footBX, footY;
    if (liftPx > 1.0f) {
        footAX = ox + 6.0f;  footBX = ox + 22.0f; footY = oy - 26.0f;
    } else if (running) {
        bool phase = ((uint32_t)(traveledPx / LEG_STRIDE_PX) & 1u) != 0;
        footAX = phase ? ox + 32.0f : ox - 10.0f;
        footBX = phase ? ox - 10.0f : ox + 32.0f;
        footY = GROUND_Y - 12.0f;
    } else {
        // Standing: a real stance, not feet together - two legs that
        // never visibly merge into one column even at rest (the previous
        // pass placed both feet 4px apart, which read as a single thick
        // leg rather than a pair - see this function's own header).
        footAX = ox - 4.0f; footBX = ox + 24.0f; footY = GROUND_Y - 12.0f;
    }
    shapes_fill_capsule_aa_land(legHipX, legHipY, 16.0f, footAX, footY, 11.0f, PX_BLACK);
    shapes_fill_capsule_aa_land(legHipX, legHipY, 16.0f, footBX, footY, 11.0f, PX_BLACK);

    // Contact shadow: always at the TRUE ground, independent of liftPx -
    // shrinks as she rises, which is the only height cue a silhouette game
    // without a ground line has (see this file's header on why there is no
    // drawn ground line at all).
    // Shorter and thicker than the first version (24x9 rather than 32x6):
    // a capsule 64px long and 12px tall is a straight line with two rounded
    // ends, which is what decision 0009 forbids by name. At roughly 3.5:1 it
    // reads as the soft oval a shadow should be.
    float halfLen = 24.0f - liftPx * 0.24f;
    if (halfLen < 6.0f) halfLen = 6.0f;
    shapes_fill_capsule_aa_land(ox + 14.0f - halfLen, GROUND_Y, 9.0f,
                                 ox + 14.0f + halfLen, GROUND_Y, 9.0f, PX_BLACK);
}

// The record ripple - see this file's header and RIPPLE_* above. `ageMs`
// is time since it started; caller has already checked it is < RIPPLE_
// DURATION_MS.
static void draw_ripple(float ox, uint32_t ageMs) {
    float u = (float)ageMs / (float)RIPPLE_DURATION_MS;
    float r = RIPPLE_MAX_R * u;
    float band = RIPPLE_MAX_BAND * (1.0f - u);
    if (band < 0.6f) return; // collapsed to nothing - nothing left to draw
    float rInner = r - band;
    if (rInner < 0.0f) rInner = 0.0f;
    if (r <= 0.6f) return;
    shapes_fill_annulus_aa_land(ox + RIPPLE_LOCAL_X, GROUND_Y + RIPPLE_LOCAL_Y, r, rInner, PX_BLACK);
}

/* =========================================================================
 * Run state.
 * ======================================================================= */
typedef enum {
    RS_DEAD_WAIT = 0, // idle, waiting for the first/next tap - the zeroed
                       // arena's own default, so a fresh app-switch-in
                       // lands here even before enter() sets it explicitly.
    RS_ALIVE,
    RS_DEAD_FREEZE,
} run_state_t;

typedef struct {
    run_state_t state;
    uint32_t    stateChangedMs;

    tap_state_t tap;
    uint32_t    rng;

    // Physics / world.
    float liftPx, liftVelPxMs;
    float speedPxMs;
    float traveledPx;

    // The one active obstacle.
    bool  obstacleActive;
    int   obstacleKind;
    float obstacleWorldX;
    uint32_t msUntilSpawn;

    // The distance flag for this run.
    bool  flagEnabled;
    float flagWorldX;   // == the best distance this run started with
    bool  flagCrossed;
    uint32_t rippleStartMs;
    bool  rippleActive;

    // Redraw bookkeeping: the one shared dirty rectangle from the previous
    // tick, so this tick's erase covers "wherever anything was, union
    // wherever anything belongs now" - see this file's header, "REDRAW
    // STRATEGY".
    bool havePrevRect;
    int  prevX, prevY, prevW, prevH;
} dino_state_t;

static dino_state_t *s_state;

/* ---- world <-> screen. Both the flag and an obstacle share this formula:
 * a screen position is DINO_X plus how much further (or less far) than the
 * traveled distance their own world position sits at. See this file's
 * header, "THE HIGH SCORE, WORDLESSLY", for why this makes the flag pass
 * under her at the exact instant traveledPx equals its worldX. --------- */
static float world_to_screen_x(const dino_state_t *s, float worldX) {
    return (float)DINO_X + (worldX - s->traveledPx);
}

static void start_run(dino_state_t *s, uint32_t nowMs) {
    s->state = RS_ALIVE;
    s->stateChangedMs = nowMs;
    s->rng = nowMs ^ 0x9E3779B9u; // reseeded per run - see this file's PRNG comment
    s->liftPx = 0.0f;
    s->liftVelPxMs = 0.0f;
    s->speedPxMs = START_SPEED_PXMS;
    s->traveledPx = 0.0f;
    s->obstacleActive = false;
    s->msUntilSpawn = rng_range(&s->rng, MIN_GAP_MS, MAX_GAP_MS);
    s->rippleActive = false;
    s->flagCrossed = false;

    uint32_t best = 0;
    s->flagEnabled = storage_get_u32(STORAGE_KIND_DINO_HISCORE, &best);
    s->flagWorldX = (float)best;

    printf("dino: run start%s\r\n", s->flagEnabled ? "" : " (no previous best)");
}

static void end_run(dino_state_t *s, uint32_t nowMs) {
    s->state = RS_DEAD_FREEZE;
    s->stateChangedMs = nowMs;

    uint32_t finalDistance = (uint32_t)s->traveledPx;
    printf("dino: hit at %lu px\r\n", (unsigned long)finalDistance);
    // Save only on the event that made the value worth keeping, and only
    // once - never per frame, never speculatively (decision 0011). A run
    // that does not beat the record it started with costs zero flash
    // writes.
    if (!s->flagEnabled || finalDistance > (uint32_t)s->flagWorldX) {
        storage_save_u32(STORAGE_KIND_DINO_HISCORE, finalDistance);
        printf("dino: new record %lu px\r\n", (unsigned long)finalDistance);
    }
}

/* =========================================================================
 * enter(): draws the idle standing pose into the framebuffer the runtime
 * has just cleared white. No push - app.h: the runtime pushes the whole
 * panel once after this returns.
 * ======================================================================= */
static void dino_enter(void) {
    s_state = APP_STATE(dino_state_t);
    // s_state->state is already RS_DEAD_WAIT (0) from the zeroed arena.
    draw_dino((float)DINO_X, 0.0f, false, 0.0f);
}

/* =========================================================================
 * tick()
 * ======================================================================= */
static void dino_tick(const app_frame_t *f) {
    dino_state_t *s = s_state;
    bool tapped = tap_tick(&s->tap, f);
    bool dirty = false;

    if (s->state == RS_ALIVE) {
        dirty = true; // a running world redraws every tick by construction

        if (tapped && s->liftPx <= 0.5f) {
            s->liftVelPxMs = -JUMP_V0_PXMS; // negative: rising
            printf("dino: jump\r\n");
        }

        // Integrate jump physics off the clock, exactly like apps/four.c's
        // falling piece (f->dtMs) - see this file's header on why this,
        // not a touch-driven animation, is what the gate's clock-driven
        // rule requires and what a real device's variable tick rate needs.
        float dtMs = (float)f->dtMs;
        if (s->liftPx > 0.0f || s->liftVelPxMs < 0.0f) {
            s->liftPx += -s->liftVelPxMs * dtMs; // liftVel is negative while rising
            s->liftVelPxMs += GRAVITY_PXMS2 * dtMs;
            if (s->liftPx <= 0.0f) { s->liftPx = 0.0f; s->liftVelPxMs = 0.0f; }
            // Explicit-Euler ceiling, not a cosmetic one. runtime_core.c
            // clamps dtMs to 250ms, and a single 250ms step taken right at
            // takeoff (velocity still near -JUMP_V0_PXMS) integrates
            // liftPx well past JUMP_APEX_PX before gravity has had any
            // chance to turn it around - discrete integration overshoots a
            // continuous apex when the step is coarse relative to the
            // motion. DINO_RECT_Y0 above is sized against JUMP_APEX_PX
            // specifically, so an unclamped overshoot is not just
            // physically wrong, it is ink outside the pushed rectangle -
            // exactly what tools/gate/run.ts's pushed-or-invisible rule
            // caught here (a coarse-tick stimulus is exactly what the gate
            // drives, per decision 0003's "an app must not depend on a
            // touch-sample tick rate"). Clamping is also the physically
            // sensible answer regardless of the rectangle: nothing about a
            // real jump should let one unlucky frame interval throw her
            // higher than the jump was ever designed to go.
            if (s->liftPx > JUMP_APEX_PX) s->liftPx = JUMP_APEX_PX;
        }

        if (s->speedPxMs < MAX_SPEED_PXMS) {
            s->speedPxMs += ACCEL_PXMS2 * dtMs;
            if (s->speedPxMs > MAX_SPEED_PXMS) s->speedPxMs = MAX_SPEED_PXMS;
        }
        s->traveledPx += s->speedPxMs * dtMs;

        // Spawn: only while no obstacle is active - see this file's header,
        // "ONE OBSTACLE AT A TIME". The countdown is frozen, not consumed,
        // while an obstacle is on screen, so the full gap is honoured
        // starting from when the screen actually clears.
        if (!s->obstacleActive) {
            if (s->msUntilSpawn <= f->dtMs) {
                s->obstacleActive = true;
                s->obstacleKind = (int)rng_range(&s->rng, 0, 1);
                // Arrives already standing just inside the visible right
                // edge (PLAYFIELD_RIGHT - PROP_HALF_W), never off past
                // LAND_W - see "the visible playfield" above for why.
                s->obstacleWorldX = s->traveledPx +
                    (float)(PLAYFIELD_RIGHT - PROP_HALF_W - DINO_X);
                printf("dino: obstacle spawn kind=%d\r\n", s->obstacleKind);
            } else {
                s->msUntilSpawn -= f->dtMs;
            }
        }

        // COLLISION. The hitbox is smaller than the drawn silhouette in
        // every direction - see this file's header, point 3: a near-miss
        // by eye is a genuine miss by the rules, never the reverse.
        if (s->obstacleActive) {
            float obstacleScreenX = world_to_screen_x(s, s->obstacleWorldX);
            const obstacle_kind_t *k = &OBSTACLE_KINDS[s->obstacleKind];
            float hitHalfW = 9.0f;
            float hitH = k->trunkH * 0.78f;
            // Deliberately just the torso, not the head/snout that reach
            // further forward and up - see this file's header, point 3: a
            // near-miss by eye (an obstacle grazing under her chin) is a
            // genuine miss by the rules.
            float dinoLeft = (float)DINO_X - 4.0f, dinoRight = (float)DINO_X + 64.0f;
            float dinoTop = GROUND_Y - s->liftPx - 110.0f, dinoBottom = GROUND_Y - s->liftPx + 2.0f;
            float obLeft = obstacleScreenX - hitHalfW, obRight = obstacleScreenX + hitHalfW;
            float obTop = GROUND_Y - hitH, obBottom = GROUND_Y + 2.0f;
            if (dinoRight > obLeft && dinoLeft < obRight && dinoBottom > obTop && dinoTop < obBottom) {
                end_run(s, f->nowMs);
                dirty = true;
            }

            if (s->state == RS_ALIVE && obstacleScreenX < PLAYFIELD_LEFT + PROP_HALF_W) {
                // Its own silhouette would start crossing into the LEFT
                // bezel band - stop drawing it here, not once it is fully
                // gone (see "the visible playfield" above), and clear the
                // slot for the next spawn.
                s->obstacleActive = false;
                s->msUntilSpawn = rng_range(&s->rng, MIN_GAP_MS, MAX_GAP_MS);
            }
        }

        // THE FLAG CROSSING. Fires once, the instant traveled distance
        // reaches the world position the flag was planted at - see this
        // file's header, "THE HIGH SCORE, WORDLESSLY".
        if (s->state == RS_ALIVE && s->flagEnabled && !s->flagCrossed &&
            s->traveledPx >= s->flagWorldX) {
            s->flagCrossed = true;
            s->rippleActive = true;
            s->rippleStartMs = f->nowMs;
            printf("dino: passed previous best\r\n");
        }
        if (s->rippleActive && f->nowMs - s->rippleStartMs >= RIPPLE_DURATION_MS) {
            s->rippleActive = false;
            dirty = true; // one more redraw to erase the ripple's last ring
        }
    } else if (s->state == RS_DEAD_FREEZE) {
        if (f->nowMs - s->stateChangedMs >= DEATH_FREEZE_MS) {
            s->state = RS_DEAD_WAIT;
            s->stateChangedMs = f->nowMs;
            s->obstacleActive = false;
            s->flagEnabled = false;
            s->rippleActive = false;
            dirty = true; // settle into the idle standing pose
        }
        // tapped is deliberately not consulted here: a tap during the
        // freeze is neither a jump (she is not alive) nor a restart (too
        // soon) - see this file's header on why the beat exists at all.
    } else { // RS_DEAD_WAIT
        if (tapped) {
            start_run(s, f->nowMs);
            dirty = true;
        }
    }

    if (!dirty) return;

    // ---- ONE shared dirty rectangle - see this file's header, "REDRAW
    // STRATEGY". The dino's box is always present; the current obstacle
    // and the flag join it only while in range.
    int x0 = DINO_X + DINO_RECT_X0, y0 = (int)(GROUND_Y + DINO_RECT_Y0);
    int x1 = DINO_X + DINO_RECT_X1, y1 = GROUND_Y + DINO_RECT_Y1;

    bool obstacleVisible = false;
    float obstacleScreenX = 0.0f;
    if (s->state != RS_DEAD_WAIT && s->obstacleActive) {
        obstacleScreenX = world_to_screen_x(s, s->obstacleWorldX);
        // Strict playfield bounds, not a wide off-screen margin: see "the
        // visible playfield" above. obstacleActive is already kept false
        // once this would fail on the left (the spawn/deactivate logic
        // above), so this mainly guards the redraw pass against ever
        // drawing a silhouette whose bounding box dips into either
        // bezel-hidden band, belt-and-suspenders.
        if (obstacleScreenX - PROP_HALF_W >= PLAYFIELD_LEFT && obstacleScreenX + PROP_HALF_W <= PLAYFIELD_RIGHT) {
            obstacleVisible = true;
            int ox0 = (int)obstacleScreenX - PROP_HALF_W, ox1 = (int)obstacleScreenX + PROP_HALF_W;
            int oy0 = GROUND_Y + PROP_TOP_LOCAL, oy1 = GROUND_Y + PROP_BOTTOM_LOCAL;
            if (ox0 < x0) x0 = ox0;
            if (ox1 > x1) x1 = ox1;
            if (oy0 < y0) y0 = oy0;
            if (oy1 > y1) y1 = oy1;
        }
    }

    bool flagVisible = false;
    float flagScreenX = 0.0f;
    if (s->state == RS_ALIVE && s->flagEnabled) {
        flagScreenX = world_to_screen_x(s, s->flagWorldX);
        // Same strict playfield bounds as the obstacle above, and for the
        // same reason: unlike the obstacle, the flag has no spawn/deactivate
        // step to keep it inset already, so this gate is load-bearing here,
        // not a backstop - it simply does not exist on screen until it
        // crosses into the visible band, and stops existing the instant it
        // would start leaving it.
        if (flagScreenX - PROP_HALF_W >= PLAYFIELD_LEFT && flagScreenX + PROP_HALF_W <= PLAYFIELD_RIGHT) {
            flagVisible = true;
            int fx0 = (int)flagScreenX - PROP_HALF_W, fx1 = (int)flagScreenX + PROP_HALF_W;
            int fy0 = GROUND_Y + PROP_TOP_LOCAL, fy1 = GROUND_Y + PROP_BOTTOM_LOCAL;
            if (fx0 < x0) x0 = fx0;
            if (fx1 > x1) x1 = fx1;
            if (fy0 < y0) y0 = fy0;
            if (fy1 > y1) y1 = fy1;
        }
    }

    // Union in whatever was drawn last tick too, so a prop that just left
    // range (or the ripple's last ring) is erased rather than left behind.
    if (s->havePrevRect) {
        if (s->prevX < x0) x0 = s->prevX;
        if (s->prevY < y0) y0 = s->prevY;
        if (s->prevX + s->prevW > x1) x1 = s->prevX + s->prevW;
        if (s->prevY + s->prevH > y1) y1 = s->prevY + s->prevH;
    }

    int w = x1 - x0, h = y1 - y0;
    gfx_fill_rect_land(x0, y0, w, h, PX_WHITE);

    bool running = (s->state == RS_ALIVE);
    draw_dino((float)DINO_X, s->liftPx, running, s->traveledPx);
    if (obstacleVisible) draw_obstacle_kind(obstacleScreenX, s->obstacleKind);
    if (flagVisible) draw_flag(flagScreenX);
    if (s->rippleActive) draw_ripple((float)DINO_X, f->nowMs - s->rippleStartMs);

    gfx_push_land(x0, y0, w, h);

    s->havePrevRect = true;
    s->prevX = x0; s->prevY = y0; s->prevW = w; s->prevH = h;
}

const app_t g_dinoApp = {
    .name       = "dino",
    .enter      = dino_enter,
    .tick       = dino_tick,
    .leave      = NULL,
    .landscape  = true,
    .wantsShake = false,
};
