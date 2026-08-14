/*
 * four: Connect Four, for a child who cannot read.
 *
 * The owner's brief, verbatim: "Un jeu de puissance 4. Pour les controles,
 * le plus simple serait de faire glisser son pouce et de voir en
 * surbrillance ou le plot va tomber. Lorsque l'on relache, ca fait vraiment
 * tomber le plot." Slide a thumb, the column under it lights up and shows
 * where the piece would land, release drops it.
 *
 * That gesture is the app. It is also the sketchpad palette's gesture
 * (press, slide, release, see apps/sketch.c's palette section), which is
 * the point: one idiom, learned once, reused. Everything below follows from
 * it plus four facts about who is holding this thing.
 *
 * ---------------------------------------------------------------------
 * 1. SHE CANNOT SEE WHICH COLUMN SHE IS ON, SO THE HIGHLIGHT IS THE GAME
 * ---------------------------------------------------------------------
 *
 * The landscape canvas is 448px wide, seven columns is 50px of board each,
 * and AGENTS.md measures a child's fingertip contact at ~75px on this
 * panel. Her thumb is wider than the column it is choosing and it covers
 * the thing it is choosing. So the feedback cannot be subtle and it cannot
 * live under her hand.
 *
 * Three redundant cues, all of them outside the ~75px her thumb hides:
 *
 *   - THE CHUTE. A capsule (a stadium: rounded ends, radius = half its own
 *     width, so it has no corners at all) 48px wide runs the WHOLE height
 *     of the screen, from above the board to below it, tinted in a pale
 *     wash of whoever is about to play. Her thumb hides ~75px of a 362px
 *     shape, so most of it is always visible, above and below her hand.
 *   - THE LANDING RING. The lowest empty hole of that column is drawn as a
 *     thick ring in the player's full colour: the outline of the piece that
 *     is about to be there. This is literally "ou le plot va tomber", and
 *     it is almost always below her thumb, since pieces stack from the
 *     bottom.
 *   - THE WAITING PIECE. A filled disc in the player's colour sits at the
 *     top of the chute, riding the column she is on. Its COLOUR is also
 *     the only thing that says whose turn it is (see section 3).
 *
 * A full column is said with the same vocabulary rather than a new one:
 * the chute washes grey, there is no landing ring (nothing will land), and
 * the waiting piece goes hollow. Filled means it will happen, hollow means
 * it will not. No words are involved.
 *
 * Tracking is immediate: any accepted touch sample repaints the two
 * affected columns in the same tick that saw it, never on a later frame.
 *
 * ---------------------------------------------------------------------
 * 2. TOUCH DROPS CONTACT ~34 TIMES A SECOND, AND RELEASE IS THE VERB
 * ---------------------------------------------------------------------
 *
 * This is the hazard that has already burned this project once today (see
 * emulator/wasm/tests/repro-touch-dropout-palette-open.ts: the palette
 * worked in the emulator and did not work in her hands). A gesture that
 * ends on RELEASE, over a controller that fakes a release dozens of times
 * per second, is exactly the shape that breaks.
 *
 * The runtime hands apps a resolved touchDown/touchPressed/touchReleased
 * (app.h), computed straight from the sample stream, so a dropout arrives
 * here as a genuine-looking release. This app therefore does not believe
 * touchReleased at all. It keeps its own lastContactMs - the same split
 * sketch.c had to make (its lastContactMs, apart from lastSampleMs) - and
 * only calls a gesture finished once contact has been ABSENT for
 * RELEASE_GRACE_MS. See that constant for the arithmetic behind its value,
 * which is derived from the measured dropout rate rather than picked.
 *
 * The mirror-image hazard is a STRAY: a single phantom contact while
 * nothing is touching the glass, which must not paint a highlight and must
 * never drop a piece. A gesture only becomes ARMED (allowed to highlight,
 * allowed to drop) after ARM_SAMPLES separate contact samples spanning at
 * least ARM_MS, which no single stray can satisfy.
 *
 * ---------------------------------------------------------------------
 * 3. NO TEXT. ANYWHERE. FOR ANYTHING.
 * ---------------------------------------------------------------------
 *
 * Whose turn it is: the colour of the waiting piece at the top. Red is
 * hers, blue is the device's. Nothing else says it, and nothing else needs
 * to: when it is her turn the red piece bobs gently at the top of the
 * screen, which is an invitation; when it is the device's turn, a blue
 * piece appears over a blue-washed chute and performs her own gesture back
 * at her before dropping (see PH_THINK), which is both "wait, I am playing"
 * and a demonstration of how the game is played.
 *
 * Who won: the four in a row BREATHE - each winning piece pulses in size
 * and blooms a ring outward, in the winner's colour, forever until touched
 * (see PH_WIN). Nothing is announced, and the winner's identity is carried
 * by the same red/blue vocabulary as everything else.
 *
 * Nobody won (a full board): no pulse at all, a beat of stillness, then
 * the board drains. The ABSENCE of the celebration is the difference, which
 * is the only distinction available without words and is also the true one.
 *
 * Starting the next game: the board opens into seven white chutes and every
 * piece slides out of the bottom, left to right, and a fresh board is
 * there. That is the real toy's own reset (you pull the slide out and the
 * pieces fall into your lap), it needs no menu and no button, and it
 * happens by itself. A touch during the celebration only skips ahead to
 * it, so a touch never MEANS two different things - decision 0002's "no
 * modal state" - it just means "hurry up".
 *
 * ---------------------------------------------------------------------
 * 4. DECISION 0009 AGAINST A BOARD THAT IS LITERALLY A GRID
 * ---------------------------------------------------------------------
 *
 * "j'aimerais que sur toutes nos apps on ait pas de bords trop durs [...]
 * ou d'angles trop droits ou de traits trop droits", and a Connect Four
 * board is a grid of rectangles. The resolution here is that THERE IS NO
 * GRID DRAWN. Not softened, not rounded: absent.
 *
 * What is drawn is one rounded slab (corner radius 34 on a 362x312 shape,
 * so the corners are a third of the short side and it reads as a lozenge,
 * not a rectangle with filed corners), and 42 circles punched out of it in
 * paper white. There is no cell border, no line, no rule, no right angle
 * anywhere in the app: the grid exists only as the ARRANGEMENT of round
 * holes, which is how a physical Connect Four board actually looks. The
 * highlight is a stadium, whose "corner radius" equals its half-width, so
 * it has no straight-to-straight junction at all. Every piece, every
 * landing ring, every halo is a circle.
 *
 * Every edge in this file is anti-aliased by coverage, the same
 * signed-distance-to-shape technique sketch.c uses for the pen and for the
 * palette's balloons, so nothing is a staircase. Decision 0009's
 * consequences section warns specifically about
 * shapes_fill_between_curves_aa_land (integer column arrays, cannot
 * anti-alias a non-vertical edge); nothing here uses it.
 *
 * Why local primitives rather than shapes.h's: shapes.h's _land family
 * composites by MIN (darkest wins), which is right for ink on paper and
 * wrong here - this app paints opaque colour over opaque colour (a piece
 * over a hole over a slab), and MIN would turn a red disc on a grey slab
 * into a muddy union instead of a red disc. It also cannot clip to an x
 * span, which is the whole basis of this file's push discipline (section
 * 5). So fill_disc/fill_ring/fill_rrect below are the same SDF-to-coverage
 * idea with source-over blending and a clip, three small functions, rather
 * than a misuse of primitives built for a different compositing model.
 *
 * ---------------------------------------------------------------------
 * 5. DECISION 0001, ASSERTED BY CONSTRUCTION RATHER THAN BY ARITHMETIC
 * ---------------------------------------------------------------------
 *
 * Every pushed window's row length must be a multiple of 8 pixels, and no
 * pixel may change outside a pushed rectangle. A piece falling down a
 * column is an animation over a busy board, which is the exact shape that
 * has left residue on this project three times.
 *
 * This app makes both impossible instead of getting them right. gfx maps a
 * landscape rectangle (lx, ly, w, h) to the panel rectangle
 * (PANEL_W-(ly+h), lx, h, w): width and height swap, so the PUSHED ROW
 * LENGTH IS THE LANDSCAPE HEIGHT. Therefore:
 *
 *   EVERY REDRAW IN THIS APP IS A FULL-HEIGHT LANDSCAPE STRIP.
 *
 * ly is always 0 and h is always LAND_H = 368, which is a multiple of 8, so
 * the row length is 368 on every single push, at rest and mid-animation
 * alike, with nothing for gfx_push to round and no edge case at the panel
 * boundary. There is no per-frame bounding-box arithmetic to get wrong,
 * which is what palette_render_frame()'s own header comment concluded after
 * paying for the alternative.
 *
 * And render_span() redraws EVERYTHING that intersects the strip, from
 * white, every time - slab, holes, pieces, highlight, animations - rather
 * than patching what it believes changed. A strip is therefore a pure
 * function of (state, time), so nothing can be left behind inside it, and
 * nothing outside it is touched because no primitive is allowed to draw
 * outside the clip it is handed. Dirty tracking picks WHICH columns to
 * repaint, and being wrong about that can only ever cost a stale column,
 * never a stray pixel.
 *
 * ---------------------------------------------------------------------
 * 6. THE DEVICE PLAYS, AND IT PLAYS DELIBERATELY BADLY
 * ---------------------------------------------------------------------
 *
 * See ai_choose() for the full design and the cost. Short version: this is
 * a toy for a child who mostly plays alone, so the device takes the other
 * side; a four-ply search fits this chip a hundred times over and would
 * beat a five year old every game, which ends with her never opening the
 * app again. So it looks exactly one move ahead, and it is only ALLOWED to
 * act on what it sees some of the time.
 *
 * ---------------------------------------------------------------------
 * 7. WHAT DOES NOT SURVIVE, ON PURPOSE
 * ---------------------------------------------------------------------
 *
 * A game does not survive switching apps. The arena is zeroed on switch
 * (app.h) and nothing here fights that. It would take one file-scope
 * static of ~48 bytes, which app.h's arena rule forbids as a style matter
 * and which the SRAM budget would genuinely not notice - so this is a
 * design choice, not a limitation:
 *
 *   - Every app on this device forgets. chrono resets, timer resets, and
 *     the sketchpad's canvas IS the framebuffer, which the runtime clears
 *     on switch. An app that alone remembered would be the odd object in
 *     the drawer.
 *   - A half-finished board she left ten minutes ago, whose plan she has
 *     forgotten, is a memory test. A fresh board is a game. She is five.
 *   - Decision 0002 4b: each app is an object and refers to nothing outside
 *     itself. Persisting across a switch is the app knowing that a switch
 *     happened.
 *
 * Do not "fix" this later thinking it was an oversight.
 *
 * SOUND is deliberately not wired either. sound.h offers exactly one
 * sound, the timer's alarm chime (a four-note rising motif, see
 * sound_synth.c); a piece landing is not that sound, and borrowing the
 * alarm for it would teach her that the alarm means nothing in particular.
 * A drop needs its own short knock, which is a sound_synth.c change, not
 * an app change.
 */
#include <stdio.h>
#include <math.h>

#include "app.h"
#include "gfx.h"

/* =====================================================================
 * Geometry, in LANDSCAPE coordinates (LAND_W x LAND_H = 448 x 368).
 *
 * Cells are square (50x50) so that the 8px gutter between neighbouring
 * holes is the same horizontally and vertically; an uneven gutter is the
 * first thing that makes a board of circles look drawn by accident. Seven
 * 50px columns is 350px of board on a 448px canvas, which leaves 49px of
 * paper either side - deliberately airy, and costing nothing in reach,
 * because the TOUCH columns are not the drawn ones (col_from_x() clamps, so
 * the outer columns own the margin beside them and there is no dead x on
 * the whole canvas).
 *
 * The board sits low, leaving HOPPER_CY's band clear at the top for the
 * waiting piece. The waiting piece is a full-size disc, not a token: it is
 * the piece that is about to fall, and it should look like the thing it is
 * about to become.
 * ================================================================== */
#define COLS 7
#define ROWS 6
#define CELL 50

#define BOARD_X0 49   // landscape x of column 0's left edge
#define BOARD_Y0 56   // landscape y of row 0's top edge (row 0 is the TOP row)

#define HOLE_R   21.0f  // a hole, and therefore a piece: 42px across, 8px of gutter

// The slab: the board's own body, a rounded lozenge with the holes punched
// out of it. SLAB_PAD is its rim beyond the cell grid.
#define SLAB_PAD 6
#define SLAB_X0  (BOARD_X0 - SLAB_PAD)
#define SLAB_X1  (BOARD_X0 + COLS * CELL + SLAB_PAD)
#define SLAB_Y0  (BOARD_Y0 - SLAB_PAD)
#define SLAB_Y1  (BOARD_Y0 + ROWS * CELL + SLAB_PAD)
#define SLAB_CX  ((SLAB_X0 + SLAB_X1) / 2.0f)
#define SLAB_CY  ((SLAB_Y0 + SLAB_Y1) / 2.0f)
#define SLAB_HW  ((SLAB_X1 - SLAB_X0) / 2.0f)
#define SLAB_HH  ((SLAB_Y1 - SLAB_Y0) / 2.0f)
#define SLAB_R   34.0f  // a third of the short side: a lozenge, not a filed square

// The chute: the full-height highlight. A stadium, since its corner radius
// equals its half-width.
#define CHUTE_HW  24.0f
#define CHUTE_Y0  3.0f
#define CHUTE_Y1  365.0f
#define CHUTE_CY  ((CHUTE_Y0 + CHUTE_Y1) / 2.0f)
#define CHUTE_HH  ((CHUTE_Y1 - CHUTE_Y0) / 2.0f)

#define HOPPER_CY 26.0f  // the waiting piece's resting centre, above the slab

// The landing ring's stroke. Thick enough to read as "a piece outline" at
// arm's length rather than as a hairline circle.
#define GHOST_STROKE 7.0f

// How far beyond a column's own cell any of this app's drawing may reach.
// The span a dirty column is repainted through is CELL/2 + SPAN_PAD from
// the column's centre (see span_for_cols), so every radius below must stay
// inside that: the widest is the win halo at HALO_R_MAX.
#define SPAN_PAD 8

/* =====================================================================
 * Timing.
 * ================================================================== */

// Gravity, in px/ms^2, chosen from the drop it has to perform rather than
// from physics: the longest fall on this board is HOPPER_CY to the bottom
// row, ~305px, and 0.005 puts that at sqrt(2*305/0.005) = ~350ms - long
// enough to watch the piece travel, short enough that a child who has
// already decided is not waiting on an animation.
#define GRAVITY 0.005f
#define BOUNCE_KEEP 0.30f  // restitution: a soft knock, not a rubber ball
#define BOUNCE_MIN_V 0.10f // below this the piece is done bouncing
#define BOUNCE_MAX 2

// How long the device visibly aims before dropping. It is not thinking
// (ai_choose costs microseconds); this is entirely so that its move is a
// THING SHE WATCHES HAPPEN rather than a piece that teleports onto the
// board, and so that the gesture is demonstrated back to her.
#define THINK_MS 620

// The celebration runs until touched, but a child who has wandered off
// should not come back to a device stuck on a four-day-old win, and the
// pulse is a repaint. CELEBRATE_MAX_MS caps it; CELEBRATE_SKIP_MS is how
// long the celebration is protected from the touch that skips it, so that
// the finger that dropped the winning piece (or a stray) cannot end the
// celebration before she has seen it.
#define CELEBRATE_MAX_MS 9000
#define CELEBRATE_SKIP_MS 1200
#define PULSE_MS 900.0f      // one breath of the winning four
// How much the winning pieces swell, in px. 5 on a 21px radius is a quarter
// of the piece, which is the point: a 3px version was measurably breathing
// and still read as "the same board" in a still frame, and the whole
// announcement of who won is that these four are DOING something the other
// pieces are not. It also means they shrink to 16 at the trough, so the
// white of their own holes opens up around them and the breath is legible
// in both directions.
#define PULSE_GROW 5.0f
// The bloom. Its INNER radius has to start outside the swollen piece
// (HOLE_R + PULSE_GROW = 26), or the ring spends the part of its life where
// it is brightest hidden inside the disc it is supposed to be coming out of
// - which is exactly what the first render showed, since the ring and the
// swell share a phase. HALO_R_MAX must also stay inside the span a dirty
// column is repainted through, CELL/2 + SPAN_PAD = 33, with room for the
// anti-aliased edge.
#define HALO_R_MIN 27.5f
#define HALO_R_MAX 31.5f
#define HALO_STROKE 3.0f

// A full board with no winner: a beat of stillness (long enough to read as
// deliberate, short enough not to feel like a hang) and then the drain.
// The absence of a pulse is what says "nobody won".
#define DRAW_PAUSE_MS 900

// The drain. Columns let go left to right, DRAIN_STAGGER_MS apart, so the
// board empties as a ripple rather than as one flat collapse - and so that
// only two or three columns are ever animating at once, which is also what
// keeps the per-frame push cost near one strip instead of seven.
#define DRAIN_GRAVITY 0.008f
#define DRAIN_STAGGER_MS 55
// Extra time after the LAST column lets go, so that its TOPMOST piece is
// genuinely off-screen before the fresh board appears rather than being
// snatched away mid-slide. Row 0 sits at y=81 and has to reach 368+HOLE_R
// = 389, so it needs 310px of travel, which at DRAIN_GRAVITY takes
// sqrt(2*310/0.008) = 278ms. 300 leaves margin.
#define DRAIN_SETTLE_MS 300

// The idle invitation: while it is her turn and her thumb is not on the
// glass, the waiting piece bobs. This is the only thing on screen that says
// "it is your turn" and it costs one column strip per frame at a throttled
// rate.
#define BOB_MS 1600.0f
#define BOB_AMP 4.0f
#define BOB_STEP_MS 60

// Repaint throttle for animations. Physics is integrated on every tick from
// f->dtMs (so it is frame-rate independent and identical in the emulator and
// on the board), but a repaint is a panel push, and the board's main loop
// runs far faster than anything worth looking at. 16ms is ~60fps.
#define RENDER_MIN_MS 16

/* =====================================================================
 * Touch, and the dropout arithmetic that sets these two numbers.
 *
 * The measured hardware profile this app is tested against is 34 dropout
 * episodes per second of finger-down (repro-touch-dropout-stroke-start.ts
 * back-calculated it from a real session: 798 dropouts over ~23s). The
 * controller reports at 60Hz, so that is a per-report loss probability of
 * p = 34/60 = 0.57, and the length of a gap is geometric: a gap survives G
 * milliseconds with probability p^(G/16.7).
 *
 *   grace   P(one gap outlasts it)   P(any gap does, over a 2s gesture)
 *   ------  ----------------------   ---------------------------------
 *   100ms   3.3e-2                   ~63%   unusable
 *   150ms   6.4e-3                   ~17%
 *   220ms   9.3e-4                   ~2.7%  (sketch.c's LIFT_DEBOUNCE_MS)
 *   260ms   2.6e-4                   ~0.8%
 *
 * (a 2s gesture contains about 29 gaps, since a gap begins on ~14.7 reports
 * per second at this rate.)
 *
 * 260ms is chosen over sketch.c's 220ms because the two failures are not
 * equally bad. A false lift in the sketchpad splits one stroke into two,
 * which is a cosmetic defect in something she is still drawing. A false
 * lift here DROPS HER PIECE, in a column she may not have chosen yet, and
 * ends her turn: it is unrecoverable inside the rules of the game. The cost
 * of the extra 40ms is that the piece leaves her thumb about a quarter of a
 * second after she lifts, and the fall itself takes ~350ms, so what she
 * sees is a piece that starts moving as her hand clears it.
 *
 * This is the one number in this file that should be re-derived if the
 * measured dropout rate ever changes, and the table above is here so that
 * re-deriving it is arithmetic rather than taste.
 * ================================================================== */
#define RELEASE_GRACE_MS 260

/* ---------------------------------------------------------------------
 * ARMING, and the stray problem, which is the dropout problem's mirror
 * image and needs a different answer.
 *
 * A stray is a phantom contact reported while nothing is touching the
 * glass. It must never drop a piece: the device sitting in a bag playing
 * by itself is, to a child, the toy being broken.
 *
 * THE FIRST ATTEMPT WAS WRONG AND THE TEST CAUGHT IT. "Two contact samples
 * spanning at least 40ms" sounds like it rejects strays, because a stray is
 * a single report. It does not: RELEASE_GRACE_MS keeps a candidate gesture
 * alive for 260ms after its last contact, so ANY two strays inside that
 * window armed a gesture, and the release verdict then dropped a piece.
 * repro-touch-dropout-four-drop.ts's stray scenario reproduced this
 * immediately.
 *
 * WHAT ACTUALLY SEPARATES THE TWO is not how many samples arrive but how
 * DENSELY. A finger on this controller reports contact on about 43% of
 * reports even under the worst measured dropout rate, which is ~26 contacts
 * per second; the modelled stray process runs at 0.2 per second, and even a
 * pathological one is orders of magnitude below a finger. So arming needs
 * ARM_SAMPLES contacts AND a sustained rate of at least ARM_RATE_HZ. The
 * consequence is a hard, non-statistical guarantee worth stating plainly:
 *
 *   NO BURST OF FEWER THAN ARM_SAMPLES PHANTOM CONTACTS CAN EVER ARM A
 *   GESTURE, AT ANY SPACING.
 *
 * which is what that test asserts directly, rather than hoping a random
 * stray process happens not to produce a bad pair.
 *
 * WHAT IT COSTS. A real finger needs about 4/0.43 = 9 reports, ~155ms, to
 * produce 4 contacts under the worst dropout profile, so the highlight
 * appears about a sixth of a second after her thumb lands. That is the
 * initial appearance only: once armed, tracking is immediate, every
 * accepted sample repainting in its own tick. The owner's requirement is
 * that the highlight follows her thumb without perceptible lag, which this
 * meets; paying 155ms once, at the start, to never drop a piece by itself
 * is the right side of that trade.
 * ------------------------------------------------------------------- */
#define ARM_SAMPLES 4
#define ARM_MS 40
#define ARM_RATE_HZ 15u

/* =====================================================================
 * Colour.
 *
 * Two sides that a child must tell apart instantly on a 1.8 inch panel at
 * arm's length. RED and BLUE, the maximally separated pair on this panel:
 * opposite ends of the hue circle, both dark enough to hold their edge
 * against white paper AND against the grey slab. The classic red/yellow of
 * the real toy was rejected on the second half of that: yellow on white
 * paper is nearly invisible at this size, and a colour she has to squint at
 * is a colour that does not tell her whose turn it is.
 *
 * Red is HERS. The device is blue.
 *
 * Both are the sketchpad palette's own red and blue (sketch.c
 * palette_color(): 0xF800, 0x001F), on purpose: the device should have one
 * vocabulary of colour across its apps, not one per app.
 *
 * The slab is a neutral light grey rather than the real toy's blue,
 * because a blue board would compete with a blue player. The pale washes
 * are the two player colours lifted most of the way to white, so a
 * highlighted column reads as "this one is yours/mine" without ever
 * competing with an actual piece.
 *
 * Written as a function rather than a static const array for the reason
 * sketch.c's palette_color() gives: px_swap() is a real function call and a
 * static initialiser needs a constant expression.
 * ================================================================== */
#define P_NONE  0
#define P_CHILD 1  // red
#define P_DEV   2  // blue

static uint16_t col_piece(uint8_t player) {
    return player == P_CHILD ? px_swap(0xF800)   // red
                             : px_swap(0x001F);  // blue
}

// The washes are deliberately at HALF strength rather than a whisper: a
// first render at #FFCACD read as a faint tint against the #DEDEDE slab on a
// bright screen, and this stripe is the one thing standing between a child
// and not knowing which column she is on. They stay well clear of a real
// piece's own saturation, so a washed column can never be mistaken for a
// column with a piece in it.
static uint16_t col_wash(uint8_t player) {
    return player == P_CHILD ? px_swap(0xFD55)   // #FFAAAD
                             : px_swap(0xADFF);  // #ADBEFF
}

static uint16_t col_wash_full(void) {
    return px_swap(0xB596);  // #B5B2B5: this column cannot take a piece
}

static uint16_t col_slab(void) {
    return px_swap(0xDEFB);  // #DEDEDE, a neutral light grey
}

/* =====================================================================
 * State. One arena allocation, per app.h.
 * ================================================================== */
enum {
    PH_PLAY,     // waiting for whoever's turn it is; her gesture is live
    PH_THINK,    // the device is aiming: its chute and waiting piece are shown
    PH_FALL,     // a piece is falling
    PH_WIN,      // the winning line breathes
    PH_DRAWPAUSE,// a full board, nobody won: a beat before the drain
    PH_DRAIN,    // the board empties and the next game begins
};

typedef struct {
    uint8_t  board[ROWS * COLS];   // P_NONE / P_CHILD / P_DEV, row 0 is the TOP row
    uint8_t  height[COLS];         // pieces in each column, 0..ROWS
    uint8_t  turn;                 // whose move it is
    uint8_t  phase;
    uint32_t phaseStartMs;

    // Her gesture. See section 2 of this file's header comment.
    bool     contactSeen;          // a gesture is in progress (dropouts included)
    bool     armed;                // it has survived ARM_SAMPLES/ARM_MS
    uint32_t gestureStartMs;
    uint32_t lastContactMs;        // last sample that actually reported contact
    int      contactCount;
    int      hoverCol;             // the highlighted column, or -1
    int      parkCol;              // where the waiting piece rests between gestures

    // The falling piece.
    int      fallCol, fallRow;
    uint8_t  fallPlayer;
    float    fallY, fallV;
    int      bouncesLeft;

    // The device's move, chosen at the start of PH_THINK so that the chute
    // it aims down is the one it will actually use.
    int      aiCol;

    // The winning line: up to 6 cells, since a run can be longer than four.
    uint8_t  winCells[6];
    int      winCount;
    uint8_t  winner;

    int      lastPlayCol;          // what ai_choose() plays near
    uint32_t rng;

    uint32_t lastRenderMs;
    uint8_t  dirty;                // bit c set: column c must be repainted
    bool     dirtyAll;
} four_state_t;

static four_state_t *s_state;

/* =====================================================================
 * Small helpers: geometry and the board.
 * ================================================================== */
static float col_x(int c)  { return (float)(BOARD_X0 + CELL / 2 + c * CELL); }
static float row_y(int r)  { return (float)(BOARD_Y0 + CELL / 2 + r * CELL); }

// Which column a landscape x belongs to. Clamped rather than rejected, so
// the paper margins either side of the board belong to the outer columns
// and there is no x on the canvas that means nothing - the same "no dead
// space a touch can fall into" rule menu.c's column_rect_land() follows.
static int col_from_x(int lx) {
    int c = (lx - BOARD_X0) / CELL;
    if (lx < BOARD_X0) c = 0;              // integer division truncates toward
                                            // zero, so negatives need this
    if (c < 0) c = 0;
    if (c > COLS - 1) c = COLS - 1;
    return c;
}

static uint8_t cell_at(const four_state_t *s, int c, int r) {
    return s->board[r * COLS + c];
}

// The row a piece dropped in column c would land on, or -1 if it is full.
static int landing_row(const four_state_t *s, int c) {
    if (s->height[c] >= ROWS) return -1;
    return ROWS - 1 - (int)s->height[c];
}

// How many of `p`'s pieces run from (c,r) exclusive in direction (dc,dr).
static int run_len(const four_state_t *s, int c, int r, uint8_t p, int dc, int dr) {
    int n = 0;
    int cc = c + dc, rr = r + dr;
    while (cc >= 0 && cc < COLS && rr >= 0 && rr < ROWS && cell_at(s, cc, rr) == p) {
        n++;
        cc += dc;
        rr += dr;
    }
    return n;
}

// The longest line `p` would own through (c,r) if it played there, capped at
// 4 (a five-long run is not more interesting than a four-long one to
// anything that reads this).
static int line_len(const four_state_t *s, int c, int r, uint8_t p) {
    static const int DC[4] = { 1, 0, 1,  1 };
    static const int DR[4] = { 0, 1, 1, -1 };
    int best = 1;
    for (int i = 0; i < 4; i++) {
        int n = 1 + run_len(s, c, r, p, DC[i], DR[i]) + run_len(s, c, r, p, -DC[i], -DR[i]);
        if (n > best) best = n;
    }
    return best > 4 ? 4 : best;
}

// The cells of the winning line through (c,r), written into out[] (up to 6).
// Returns 0 when there is no win.
static int win_cells(const four_state_t *s, int c, int r, uint8_t p, uint8_t *out) {
    static const int DC[4] = { 1, 0, 1,  1 };
    static const int DR[4] = { 0, 1, 1, -1 };
    for (int i = 0; i < 4; i++) {
        int back = run_len(s, c, r, p, -DC[i], -DR[i]);
        int fwd  = run_len(s, c, r, p,  DC[i],  DR[i]);
        int n = 1 + back + fwd;
        if (n < 4) continue;
        if (n > 6) n = 6;
        int cc = c - DC[i] * back, rr = r - DR[i] * back;
        for (int k = 0; k < n; k++) {
            out[k] = (uint8_t)(rr * COLS + cc);
            cc += DC[i];
            rr += DR[i];
        }
        return n;
    }
    return 0;
}

// xorshift32. Seeded from the wall clock at enter() and re-stirred with the
// timing of every one of her drops, so a real session never repeats even
// though a scripted one (the emulator's tests) is perfectly deterministic.
static uint32_t rnd(four_state_t *s) {
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;
    return x;
}

/* =====================================================================
 * The opponent.
 *
 * WHO PLAYS THE OTHER SIDE: the device. This is a toy for a child who
 * mostly plays alone, and two humans hunched over a 1.8 inch puck is not
 * the thing that gets used.
 *
 * HOW WEAKLY, AND WHY THIS SHAPE. The failure to avoid is not "it is too
 * strong", it is "it stops feeling like an opponent". A random dropper is
 * beatable and worthless: nothing it does is a response to anything she
 * did, so there is nobody on the other side. What makes a weak player read
 * as a player is that it ANSWERS - it plays near what just happened, it
 * builds its own little clusters, and it occasionally does something that
 * makes her stop and think. So the weakness is not put into the move
 * choice, it is put into the ATTENTION:
 *
 *   - It sees exactly one move ahead. It never asks what she does next, so
 *     it walks into double threats, gives her open threes, and stacks a
 *     piece under a square she needs. That is where most of her wins come
 *     from and it is invisible as a handicap: it looks like a player who is
 *     concentrating on their own side of the board.
 *   - It only ACTS on what it sees some of the time. It takes an
 *     immediate win on AI_TAKE_WIN_PCT of the chances it gets and blocks
 *     her three on AI_BLOCK_PCT of hers. Both numbers are deliberately
 *     under half. It must be able to do both, or winning against it means
 *     nothing and a lucky loss is inexplicable; it must not do them
 *     reliably, or a five year old never wins.
 *   - When the roll says it did not see its own win, the winning column is
 *     REMOVED from the pool it then picks from. Otherwise the ordinary
 *     line-building score would hand it the same column anyway and the
 *     handicap would quietly do nothing. Blocking columns are NOT removed:
 *     it should sometimes block by accident, exactly like a distracted
 *     player.
 *   - What is left is a one-ply score: build your own line, play near the
 *     action, mild preference for the middle, plus a coin flip to break
 *     ties so the same position does not always produce the same move.
 *
 * WHAT IT COSTS. 7 columns x 4 directions x 2 scan directions x at most 5
 * cells = under 300 array reads and no recursion, no allocation, no
 * malloc, bounded stack, once per device move. A four-ply minimax over
 * this board is on the order of 2400 leaf evaluations, which this chip
 * would also do in a millisecond - the reason for not doing it is the child,
 * not the CPU, and that is worth being explicit about so nobody
 * "optimises" this into a real engine later.
 * ================================================================== */
#define AI_TAKE_WIN_PCT 55
#define AI_BLOCK_PCT    35

static int ai_choose(four_state_t *s) {
    int winCols[COLS], nWin = 0;
    int blockCols[COLS], nBlock = 0;
    int pool[COLS], nPool = 0;
    int poolScore[COLS];
    int bestScore = -1000;

    for (int c = 0; c < COLS; c++) {
        int r = landing_row(s, c);
        if (r < 0) continue;

        bool isWin = line_len(s, c, r, P_DEV) >= 4;
        bool isBlock = line_len(s, c, r, P_CHILD) >= 4;
        if (isWin) winCols[nWin++] = c;
        if (isBlock) blockCols[nBlock++] = c;

        // A column that wins is excluded from the ordinary pool: see this
        // function's header comment on why the handicap has to be enforced
        // here rather than left to the score.
        if (isWin) continue;

        int own = line_len(s, c, r, P_DEV);
        int score = (own >= 3) ? 5 : (own == 2 ? 3 : 0);
        int near = 3 - (c > s->lastPlayCol ? c - s->lastPlayCol : s->lastPlayCol - c);
        if (near > 0) score += near;
        if (c == COLS / 2) score += 1;
        score += (int)(rnd(s) & 1u);

        pool[nPool] = c;
        poolScore[nPool] = score;
        nPool++;
        if (score > bestScore) bestScore = score;
    }

    if (nWin > 0 && (int)(rnd(s) % 100u) < AI_TAKE_WIN_PCT) return winCols[rnd(s) % (uint32_t)nWin];
    if (nBlock > 0 && (int)(rnd(s) % 100u) < AI_BLOCK_PCT) return blockCols[rnd(s) % (uint32_t)nBlock];

    if (nPool == 0) {
        // Every legal column is a winning one: it has to play somewhere, and
        // "the roll said it did not notice" cannot mean "it does not move".
        if (nWin > 0) return winCols[rnd(s) % (uint32_t)nWin];
        return -1;
    }

    int best[COLS], nBest = 0;
    for (int i = 0; i < nPool; i++) {
        if (poolScore[i] == bestScore) best[nBest++] = pool[i];
    }
    return best[rnd(s) % (uint32_t)nBest];
}

/* =====================================================================
 * Drawing primitives: signed distance -> coverage -> source-over blend,
 * clipped to a landscape x span.
 *
 * Same technique as sketch.c's draw_capsule/draw_rounded_rect and
 * shapes.c's anti-aliased family, with two differences that section 4 of
 * this file's header comment argues for: composition is source-over (this
 * app paints opaque colour over opaque colour, not ink onto paper), and
 * every function takes the clip span that section 5's push discipline is
 * built on.
 *
 * The sqrtf is confined to the ~1px feather at each edge: the interior is
 * accepted and the exterior rejected by squared-distance comparisons, so a
 * 42px disc costs about 130 square roots rather than 1800. That matters on
 * the board (this runs per animation frame, per column) and it matters in
 * the emulator too, where every sqrtf is a wasm-to-JS call.
 *
 * gfx.h rotates RECTANGLES, not pixels, so a landscape app placing
 * individual pixels maps its own coordinates - the mapping is gfx.h's own
 * documented one, (lx, ly) -> (PANEL_W-1-ly, lx), the same single exception
 * shapes.c's aa_composite_land() takes for the same reason.
 * ================================================================== */
static void blend_land(int lx, int ly, uint16_t colorPx, int alpha) {
    if (lx < 0 || lx >= LAND_W || ly < 0 || ly >= LAND_H) return;
    int idx = lx * PANEL_W + (PANEL_W - 1 - ly);
    if (alpha >= 256) {
        gfx_fb[idx] = colorPx;
        return;
    }
    if (alpha <= 0) return;
    uint16_t d = px_swap(gfx_fb[idx]);
    uint16_t sc = px_swap(colorPx);
    int dr = (d >> 11) & 31, dg = (d >> 5) & 63, db = d & 31;
    int sr = (sc >> 11) & 31, sg = (sc >> 5) & 63, sb = sc & 31;
    int r = dr + ((sr - dr) * alpha) / 256;
    int g = dg + ((sg - dg) * alpha) / 256;
    int b = db + ((sb - db) * alpha) / 256;
    gfx_fb[idx] = px_swap((uint16_t)((r << 11) | (g << 5) | b));
}

static void fill_disc(float cx, float cy, float r, uint16_t colorPx,
                       int clipX0, int clipX1, int alpha) {
    if (r <= 0.0f || alpha <= 0) return;
    int minX = (int)floorf(cx - r - 1.0f); if (minX < clipX0) minX = clipX0;
    int maxX = (int)ceilf(cx + r + 1.0f);  if (maxX > clipX1 - 1) maxX = clipX1 - 1;
    int minY = (int)floorf(cy - r - 1.0f); if (minY < 0) minY = 0;
    int maxY = (int)ceilf(cy + r + 1.0f);  if (maxY > LAND_H - 1) maxY = LAND_H - 1;
    if (minX > maxX || minY > maxY) return;

    float rOut = r + 0.5f, rIn = r - 0.5f;
    float rOut2 = rOut * rOut;
    float rIn2 = rIn > 0.0f ? rIn * rIn : -1.0f;

    for (int y = minY; y <= maxY; y++) {
        float dy = (float)y + 0.5f - cy;
        float dy2 = dy * dy;
        if (dy2 >= rOut2) continue;
        for (int x = minX; x <= maxX; x++) {
            float dx = (float)x + 0.5f - cx;
            float d2 = dx * dx + dy2;
            if (d2 >= rOut2) continue;
            int a = alpha;
            if (d2 > rIn2) {
                float cov = rOut - sqrtf(d2);
                if (cov <= 0.0f) continue;
                if (cov > 1.0f) cov = 1.0f;
                a = (int)(cov * (float)alpha);
            }
            blend_land(x, y, colorPx, a);
        }
    }
}

static void fill_ring(float cx, float cy, float rOut, float rIn, uint16_t colorPx,
                       int clipX0, int clipX1, int alpha) {
    if (rOut <= 0.0f || rOut <= rIn || alpha <= 0) return;
    int minX = (int)floorf(cx - rOut - 1.0f); if (minX < clipX0) minX = clipX0;
    int maxX = (int)ceilf(cx + rOut + 1.0f);  if (maxX > clipX1 - 1) maxX = clipX1 - 1;
    int minY = (int)floorf(cy - rOut - 1.0f); if (minY < 0) minY = 0;
    int maxY = (int)ceilf(cy + rOut + 1.0f);  if (maxY > LAND_H - 1) maxY = LAND_H - 1;
    if (minX > maxX || minY > maxY) return;

    float oOut = rOut + 0.5f, oIn = rOut - 0.5f;
    float iOut = rIn + 0.5f, iIn = rIn - 0.5f;
    float oOut2 = oOut * oOut, oIn2 = oIn * oIn;
    float iOut2 = iOut * iOut;
    float iIn2 = iIn > 0.0f ? iIn * iIn : -1.0f;

    for (int y = minY; y <= maxY; y++) {
        float dy = (float)y + 0.5f - cy;
        float dy2 = dy * dy;
        if (dy2 >= oOut2) continue;
        for (int x = minX; x <= maxX; x++) {
            float dx = (float)x + 0.5f - cx;
            float d2 = dx * dx + dy2;
            if (d2 >= oOut2) continue;   // outside the ring entirely
            if (d2 <= iIn2) continue;    // inside the hole entirely
            int a = alpha;
            if (d2 > oIn2 || d2 < iOut2) {
                float d = sqrtf(d2);
                float cov = oOut - d;                 // coverage against the outer edge
                float covIn = d - (rIn - 0.5f);       // and against the inner one
                if (covIn < cov) cov = covIn;
                if (cov <= 0.0f) continue;
                if (cov > 1.0f) cov = 1.0f;
                a = (int)(cov * (float)alpha);
            }
            blend_land(x, y, colorPx, a);
        }
    }
}

// A rounded rectangle, by the standard rounded-box signed distance (shrink
// to the sharp inner box by the corner radius, take the distance to that,
// subtract the radius back out). With cornerR == halfW it is a stadium,
// which is what the chute uses.
static void fill_rrect(float cx, float cy, float halfW, float halfH, float cornerR,
                        uint16_t colorPx, int clipX0, int clipX1, int alpha) {
    if (halfW <= 0.0f || halfH <= 0.0f || alpha <= 0) return;
    if (cornerR > halfW) cornerR = halfW;
    if (cornerR > halfH) cornerR = halfH;
    if (cornerR < 0.0f) cornerR = 0.0f;

    int minX = (int)floorf(cx - halfW - 1.0f); if (minX < clipX0) minX = clipX0;
    int maxX = (int)ceilf(cx + halfW + 1.0f);  if (maxX > clipX1 - 1) maxX = clipX1 - 1;
    int minY = (int)floorf(cy - halfH - 1.0f); if (minY < 0) minY = 0;
    int maxY = (int)ceilf(cy + halfH + 1.0f);  if (maxY > LAND_H - 1) maxY = LAND_H - 1;
    if (minX > maxX || minY > maxY) return;

    float rOut = cornerR + 0.5f, rIn = cornerR - 0.5f;
    float rOut2 = rOut * rOut;
    float rIn2 = rIn > 0.0f ? rIn * rIn : -1.0f;

    for (int y = minY; y <= maxY; y++) {
        float qy = fabsf((float)y + 0.5f - cy) - halfH + cornerR;
        for (int x = minX; x <= maxX; x++) {
            float qx = fabsf((float)x + 0.5f - cx) - halfW + cornerR;
            int a = alpha;
            if (qx > 0.0f && qy > 0.0f) {
                // A corner quadrant: the only place a square root is needed.
                float q2 = qx * qx + qy * qy;
                if (q2 >= rOut2) continue;
                if (q2 > rIn2) {
                    float cov = rOut - sqrtf(q2);
                    if (cov <= 0.0f) continue;
                    if (cov > 1.0f) cov = 1.0f;
                    a = (int)(cov * (float)alpha);
                }
            } else {
                // A straight edge (or the interior): the distance is just the
                // larger of the two axis distances, no square root involved.
                float d = (qx > qy ? qx : qy) - cornerR;
                float cov = 0.5f - d;
                if (cov <= 0.0f) continue;
                if (cov < 1.0f) a = (int)(cov * (float)alpha);
            }
            blend_land(x, y, colorPx, a);
        }
    }
}

/* =====================================================================
 * Rendering.
 *
 * render_span() is a pure function of (state, time) over one full-height
 * landscape strip: it starts from white and redraws every element that
 * intersects the strip, in back-to-front order. It never patches. See
 * section 5 of this file's header comment for why this, and not tighter
 * dirty rectangles, is the design.
 * ================================================================== */

// Which column is currently highlighted, and in whose colour. During
// PH_THINK the device highlights its own chosen column, which is how its
// move is a thing she watches happen rather than a piece that appears.
static int hilite_col(const four_state_t *s) {
    if (s->phase == PH_PLAY && s->turn == P_CHILD && s->armed) return s->hoverCol;
    if (s->phase == PH_THINK) return s->aiCol;
    // The chute and its landing ring STAY UP while the piece falls, rather
    // than blinking out the instant she lets go. The fall is the answer to
    // the promise the ring was making, and watching the piece slide down its
    // own lit channel into the exact ring that was already drawn is what
    // makes the promise legible; clearing the highlight first would leave the
    // piece falling through a board that no longer says where it is going.
    if (s->phase == PH_FALL) return s->fallCol;
    return -1;
}

static uint8_t hilite_player(const four_state_t *s) {
    if (s->phase == PH_THINK) return P_DEV;
    if (s->phase == PH_FALL) return s->fallPlayer;
    return s->turn;
}

// Where the waiting piece sits, or -1 when there is none (nothing is
// waiting during a fall, a celebration or a drain, and the ABSENCE of the
// waiting piece is what says that a touch now means something else).
static int waiting_col(const four_state_t *s) {
    if (s->phase == PH_THINK) return s->aiCol;
    if (s->phase != PH_PLAY) return -1;
    if (s->turn != P_CHILD) return -1;
    return s->armed ? s->hoverCol : s->parkCol;
}

// The drain's per-column progress: how far column c's pieces have slid, in
// px, or a negative number if this column has not let go yet.
static float drain_offset(const four_state_t *s, uint32_t nowMs, int c) {
    float t = (float)(nowMs - s->phaseStartMs) - (float)(c * DRAIN_STAGGER_MS);
    if (t <= 0.0f) return -1.0f;
    return 0.5f * DRAIN_GRAVITY * t * t;
}

static void render_span(four_state_t *s, uint32_t nowMs, int lx0, int lx1) {
    if (lx0 < 0) lx0 = 0;
    if (lx1 > LAND_W) lx1 = LAND_W;
    if (lx1 <= lx0) return;

    // 1. paper.
    gfx_fill_rect_land(lx0, 0, lx1 - lx0, LAND_H, PX_WHITE);

    // 2. the slab.
    fill_rrect(SLAB_CX, SLAB_CY, SLAB_HW, SLAB_HH, SLAB_R, col_slab(), lx0, lx1, 256);

    // 3. the chute: either the highlight (a pale wash of whoever is about to
    //    play, or grey when that column cannot take a piece) or, during the
    //    drain, a white channel the pieces slide out through.
    int hc = hilite_col(s);
    if (s->phase == PH_DRAIN) {
        for (int c = 0; c < COLS; c++) {
            if (drain_offset(s, nowMs, c) < 0.0f) continue;
            fill_rrect(col_x(c), CHUTE_CY, CHUTE_HW, CHUTE_HH, CHUTE_HW, PX_WHITE, lx0, lx1, 256);
        }
    } else if (hc >= 0) {
        bool full = landing_row(s, hc) < 0;
        uint16_t wash = full ? col_wash_full() : col_wash(hilite_player(s));
        fill_rrect(col_x(hc), CHUTE_CY, CHUTE_HW, CHUTE_HH, CHUTE_HW, wash, lx0, lx1, 256);
    }

    // 4. the holes, and the pieces sitting in them. A hole is a white disc
    //    punched out of the slab; a piece is the same disc in a player's
    //    colour. There is no separate "hole ring" to draw, which is what
    //    keeps the board free of any line at all.
    float pulse = 0.0f, haloR = 0.0f;
    int haloAlpha = 0;
    if (s->phase == PH_WIN) {
        float u = fmodf((float)(nowMs - s->phaseStartMs), PULSE_MS) / PULSE_MS;
        pulse = PULSE_GROW * sinf(u * 6.2831853f);
        haloR = HALO_R_MIN + (HALO_R_MAX - HALO_R_MIN) * u;
        haloAlpha = (int)(256.0f * (1.0f - u));
    }

    for (int c = 0; c < COLS; c++) {
        float dx0 = col_x(c) - (float)(CELL / 2 + SPAN_PAD);
        float dx1 = col_x(c) + (float)(CELL / 2 + SPAN_PAD);
        if (dx1 < (float)lx0 || dx0 > (float)lx1) continue;

        float drainOff = s->phase == PH_DRAIN ? drain_offset(s, nowMs, c) : -1.0f;

        for (int r = 0; r < ROWS; r++) {
            uint8_t v = cell_at(s, c, r);

            if (drainOff >= 0.0f) {
                // This column has let go: no hole is drawn at all (the chute
                // above replaced them), only the pieces, sliding.
                if (v != P_NONE) fill_disc(col_x(c), row_y(r) + drainOff, HOLE_R, col_piece(v), lx0, lx1, 256);
                continue;
            }

            // The hole first, always, even under a piece: a winning piece
            // pulses SMALLER as well as larger, and what it must reveal when
            // it shrinks is paper, not slab.
            fill_disc(col_x(c), row_y(r), HOLE_R, PX_WHITE, lx0, lx1, 256);
            if (v == P_NONE) continue;

            bool isWin = false;
            if (s->phase == PH_WIN) {
                for (int k = 0; k < s->winCount; k++) {
                    if (s->winCells[k] == (uint8_t)(r * COLS + c)) { isWin = true; break; }
                }
            }
            fill_disc(col_x(c), row_y(r), isWin ? HOLE_R + pulse : HOLE_R, col_piece(v), lx0, lx1, 256);
        }
    }

    // 4b. the winning line's bloom, in a pass of its own AFTER every hole and
    //     piece: a halo grows well past its own hole (HALO_R_MAX), across the
    //     slab and over its neighbours, so drawing it inline would leave it
    //     half-covered by whichever cell happened to be painted next.
    if (s->phase == PH_WIN) {
        for (int k = 0; k < s->winCount; k++) {
            int cell = s->winCells[k];
            int c = cell % COLS, r = cell / COLS;
            fill_ring(col_x(c), row_y(r), haloR, haloR - HALO_STROKE, col_piece(s->winner), lx0, lx1, haloAlpha);
        }
    }

    // 5. the landing ring: the outline of the piece that is about to be
    //    there. This is the answer to "ou le plot va tomber" and it is
    //    drawn last among the board's own elements so nothing covers it.
    if (hc >= 0 && s->phase != PH_DRAIN) {
        int lr = landing_row(s, hc);
        if (lr >= 0) {
            fill_ring(col_x(hc), row_y(lr), HOLE_R, HOLE_R - GHOST_STROKE,
                      col_piece(hilite_player(s)), lx0, lx1, 256);
        }
    }

    // 6. the waiting piece, at the top of the chute: filled when it will
    //    fall, hollow when the column under it cannot take it.
    int wc = waiting_col(s);
    if (wc >= 0) {
        float wy = HOPPER_CY;
        if (s->phase == PH_PLAY && s->turn == P_CHILD && !s->armed) {
            // The idle invitation: it is your turn.
            wy += BOB_AMP * sinf(fmodf((float)nowMs, BOB_MS) / BOB_MS * 6.2831853f);
        }
        uint8_t wp = hilite_player(s);
        if (landing_row(s, wc) < 0) {
            fill_ring(col_x(wc), wy, HOLE_R, HOLE_R - GHOST_STROKE, col_piece(wp), lx0, lx1, 256);
        } else {
            fill_disc(col_x(wc), wy, HOLE_R, col_piece(wp), lx0, lx1, 256);
        }
    }

    // 7. the falling piece, over everything, since it is passing in front of
    //    the board's face exactly as a real one does.
    if (s->phase == PH_FALL) {
        fill_disc(col_x(s->fallCol), s->fallY, HOLE_R, col_piece(s->fallPlayer), lx0, lx1, 256);
    }
}

/* ---------------------------------------------------------------------
 * Dirty tracking and the push. Every push is a full-height strip, so its
 * row length after gfx's rotation is LAND_H = 368, a multiple of 8, always
 * - see section 5 of this file's header comment.
 * ------------------------------------------------------------------- */
static void mark(four_state_t *s, int col) {
    if (col < 0 || col >= COLS) return;
    s->dirty |= (uint8_t)(1u << col);
}

static void mark_all(four_state_t *s) { s->dirtyAll = true; }

static void span_for_cols(int a, int b, int *lx0, int *lx1) {
    *lx0 = BOARD_X0 + a * CELL - SPAN_PAD;
    *lx1 = BOARD_X0 + (b + 1) * CELL + SPAN_PAD;
    if (*lx0 < 0) *lx0 = 0;
    if (*lx1 > LAND_W) *lx1 = LAND_W;
}

static void flush(four_state_t *s, uint32_t nowMs) {
    if (s->dirtyAll) {
        render_span(s, nowMs, 0, LAND_W);
        gfx_push_land(0, 0, LAND_W, LAND_H);
        s->dirtyAll = false;
        s->dirty = 0;
        return;
    }
    if (s->dirty == 0) return;

    // Coalesce runs of adjacent dirty columns: a run is one strip, one
    // render, one push.
    int c = 0;
    while (c < COLS) {
        if ((s->dirty & (1u << c)) == 0) { c++; continue; }
        int start = c;
        while (c < COLS && (s->dirty & (1u << c)) != 0) c++;
        int lx0, lx1;
        span_for_cols(start, c - 1, &lx0, &lx1);
        render_span(s, nowMs, lx0, lx1);
        gfx_push_land(lx0, 0, lx1 - lx0, LAND_H);
    }
    s->dirty = 0;
}

/* =====================================================================
 * The game itself.
 * ================================================================== */
static void start_fall(four_state_t *s, uint32_t nowMs, int col, uint8_t player) {
    s->phase = PH_FALL;
    s->phaseStartMs = nowMs;
    s->fallCol = col;
    s->fallRow = landing_row(s, col);
    s->fallPlayer = player;
    s->fallY = HOPPER_CY;
    s->fallV = 0.0f;
    s->bouncesLeft = BOUNCE_MAX;
    mark(s, col);
    printf("four: drop col=%d row=%d player=%d\r\n", col, s->fallRow, (int)player);
}

static void begin_think(four_state_t *s, uint32_t nowMs) {
    s->aiCol = ai_choose(s);
    if (s->aiCol < 0) {                 // no legal move: a full board, handled
        s->phase = PH_DRAWPAUSE;        // as a draw by the caller's own check,
        s->phaseStartMs = nowMs;        // but belt and braces
        mark_all(s);
        return;
    }
    s->phase = PH_THINK;
    s->phaseStartMs = nowMs;
    mark(s, s->aiCol);
    printf("four: ai col=%d\r\n", s->aiCol);
}

static void reset_game(four_state_t *s, uint32_t nowMs) {
    for (int i = 0; i < ROWS * COLS; i++) s->board[i] = P_NONE;
    for (int c = 0; c < COLS; c++) s->height[c] = 0;
    // She always starts. Going first is an advantage in Connect Four, and
    // she is the reason this app exists; alternating would hand the device
    // the advantage in half the games it is already meant to lose.
    s->turn = P_CHILD;
    s->phase = PH_PLAY;
    s->phaseStartMs = nowMs;
    s->winCount = 0;
    s->winner = P_NONE;
    s->lastPlayCol = COLS / 2;
    s->parkCol = COLS / 2;
    s->hoverCol = -1;
    s->contactSeen = false;
    s->armed = false;
    mark_all(s);
    printf("four: new game\r\n");
}

// A piece has finished falling: commit it, and decide what happens next.
static void land_piece(four_state_t *s, uint32_t nowMs) {
    int c = s->fallCol, r = s->fallRow;
    mark(s, c);   // the falling piece has to be replaced by the settled one
                   // in the same tick, whatever the branches below then do
    s->board[r * COLS + c] = s->fallPlayer;
    s->height[c]++;
    s->lastPlayCol = c;

    s->winCount = win_cells(s, c, r, s->fallPlayer, s->winCells);
    if (s->winCount > 0) {
        s->winner = s->fallPlayer;
        s->phase = PH_WIN;
        s->phaseStartMs = nowMs;
        mark_all(s);
        printf("four: win player=%d len=%d\r\n", (int)s->winner, s->winCount);
        return;
    }

    bool full = true;
    for (int i = 0; i < COLS; i++) if (s->height[i] < ROWS) { full = false; break; }
    if (full) {
        s->phase = PH_DRAWPAUSE;
        s->phaseStartMs = nowMs;
        mark_all(s);
        printf("four: draw\r\n");
        return;
    }

    if (s->fallPlayer == P_CHILD) {
        s->turn = P_DEV;
        begin_think(s, nowMs);
    } else {
        s->turn = P_CHILD;
        s->phase = PH_PLAY;
        s->phaseStartMs = nowMs;
        s->parkCol = c;   // the waiting piece reappears where the action was,
                           // not at a fixed home, so her eye does not have to
                           // travel back to the middle every turn
        mark_all(s);
    }
}

/* ---------------------------------------------------------------------
 * Her gesture. See section 2 of this file's header comment for why none of
 * this trusts f->touchReleased.
 * ------------------------------------------------------------------- */
static void gesture_tick(four_state_t *s, const app_frame_t *f) {
    bool live = (s->phase == PH_PLAY && s->turn == P_CHILD);

    if (!live) {
        // Any gesture in progress is abandoned rather than carried across a
        // phase boundary: a finger still down when her turn comes back starts
        // a fresh gesture, arms in ARM_MS, and highlights - which is what she
        // would expect, and it costs nothing to be strict here.
        if (s->contactSeen || s->armed) {
            s->contactSeen = false;
            s->armed = false;
            s->contactCount = 0;
            if (s->hoverCol >= 0) { mark(s, s->hoverCol); s->hoverCol = -1; }
        }
        return;
    }

    if (f->touchDown) {
        if (!s->contactSeen) {
            s->contactSeen = true;
            s->gestureStartMs = f->nowMs;
            s->contactCount = 0;
        }
        s->lastContactMs = f->nowMs;
        s->contactCount++;

        uint32_t elapsed = f->nowMs - s->gestureStartMs;
        if (!s->armed && s->contactCount >= ARM_SAMPLES && elapsed >= ARM_MS &&
            (uint32_t)s->contactCount * 1000u >= ARM_RATE_HZ * elapsed) {
            s->armed = true;
            mark(s, s->parkCol);   // the bobbing waiting piece goes away
        }

        if (s->armed) {
            int lx, ly;
            // Touch arrives in PANEL coordinates; gfx.h documents only the
            // landscape-to-panel direction, so the inverse is algebra on that
            // same mapping - the same inversion menu.c's panel_to_land() does,
            // and for the same reason (it is the only landscape app besides
            // that one that hit-tests a touch).
            lx = f->touchY;
            ly = PANEL_W - 1 - f->touchX;
            (void)ly;   // only the column matters: the whole height of the
                        // screen is that column's target, so there is no way
                        // to miss vertically at all
            int col = col_from_x(lx);
            if (col != s->hoverCol) {
                mark(s, s->hoverCol);
                mark(s, col);
                s->hoverCol = col;
                printf("four: hover c=%d\r\n", col);
            }
        }
        return;
    }

    // No contact this tick. That is almost always a dropout, not a lift.
    if (!s->contactSeen) return;
    if ((f->nowMs - s->lastContactMs) < RELEASE_GRACE_MS) return;

    bool wasArmed = s->armed;
    int col = s->hoverCol;
    s->contactSeen = false;
    s->armed = false;
    s->contactCount = 0;
    s->hoverCol = -1;

    if (!wasArmed || col < 0) { mark(s, col); return; }

    // Re-stir the generator with the timing of her release: a real session
    // never repeats, a scripted one (the tests) still does.
    s->rng ^= f->nowMs * 2654435761u;

    s->parkCol = col;
    if (landing_row(s, col) < 0) {
        // A full column: the release does nothing at all, which is what the
        // grey wash and the hollow waiting piece have been saying the whole
        // time she was over it. Her turn does not end.
        mark(s, col);
        printf("four: release over a full column c=%d, ignored\r\n", col);
        return;
    }
    start_fall(s, f->nowMs, col, P_CHILD);
}

/* =====================================================================
 * enter / tick.
 * ================================================================== */
static void four_enter(void) {
    s_state = APP_STATE(four_state_t);
    four_state_t *s = s_state;

    // Seeded from the wall clock, which on the board is however long it took
    // her to get here and is therefore genuinely unpredictable; in the
    // emulator it is whatever the harness ticked, so a scripted game is
    // reproducible. Re-stirred on every one of her drops (gesture_tick).
    s->rng = 0x9E3779B9u;

    reset_game(s, 0);
    s->dirtyAll = false;         // enter() must not push: the runtime pushes
    s->dirty = 0;                // the whole panel once after it returns
    render_span(s, 0, 0, LAND_W);

    printf("four: state=%d bytes (arena %d)\r\n", (int)sizeof(four_state_t), (int)APP_ARENA_BYTES);
}

static void four_tick(const app_frame_t *f) {
    four_state_t *s = s_state;
    uint32_t now = f->nowMs;

    // BOOT abandons the game and deals a fresh board, the same "one physical
    // button, one obvious job" chrono gives it. It is an adult's escape
    // hatch (and the emulator's), not something she needs.
    if (f->bootClicked && s->phase != PH_DRAIN) {
        s->phase = PH_DRAIN;
        s->phaseStartMs = now;
        s->contactSeen = false;
        s->armed = false;
        s->hoverCol = -1;
        mark_all(s);
    }

    gesture_tick(s, f);

    switch (s->phase) {
    case PH_PLAY:
        // The idle bob, on its own slower clock. A 4px travel over 1.6s moves
        // about a quarter of a pixel per 60fps frame, so repainting a strip at
        // the animation rate would push sixty near-identical frames a second
        // for a change nobody can see; BOB_STEP_MS is the rate at which the
        // bob actually moves about a pixel.
        if (s->turn == P_CHILD && !s->armed && (now - s->lastRenderMs) >= BOB_STEP_MS) {
            mark(s, s->parkCol);
        }
        break;

    case PH_THINK:
        if ((now - s->phaseStartMs) >= THINK_MS) start_fall(s, now, s->aiCol, P_DEV);
        break;

    case PH_FALL: {
        // Integrated from f->dtMs on every tick, so the fall takes the same
        // wall-clock time whatever the loop rate is; only the REPAINT is
        // throttled, below.
        float dt = (float)f->dtMs;
        s->fallV += GRAVITY * dt;
        s->fallY += s->fallV * dt;

        float target = row_y(s->fallRow);
        if (s->fallY >= target) {
            if (s->bouncesLeft > 0 && s->fallV > BOUNCE_MIN_V) {
                s->fallY = target;
                s->fallV = -s->fallV * BOUNCE_KEEP;
                s->bouncesLeft--;
            } else {
                s->fallY = target;
                land_piece(s, now);
                break;
            }
        }
        if ((now - s->lastRenderMs) >= RENDER_MIN_MS) mark(s, s->fallCol);
        break;
    }

    case PH_WIN:
        if ((now - s->phaseStartMs) >= CELEBRATE_MAX_MS ||
            (f->touchDown && (now - s->phaseStartMs) >= CELEBRATE_SKIP_MS)) {
            s->phase = PH_DRAIN;
            s->phaseStartMs = now;
            mark_all(s);
            break;
        }
        if ((now - s->lastRenderMs) >= RENDER_MIN_MS) {
            for (int k = 0; k < s->winCount; k++) mark(s, s->winCells[k] % COLS);
        }
        break;

    case PH_DRAWPAUSE:
        if ((now - s->phaseStartMs) >= DRAW_PAUSE_MS) {
            s->phase = PH_DRAIN;
            s->phaseStartMs = now;
            mark_all(s);
        }
        break;

    case PH_DRAIN: {
        uint32_t total = (uint32_t)(COLS - 1) * DRAIN_STAGGER_MS + DRAIN_SETTLE_MS;
        if ((now - s->phaseStartMs) >= total) {
            reset_game(s, now);
            break;
        }
        if ((now - s->lastRenderMs) >= RENDER_MIN_MS) {
            for (int c = 0; c < COLS; c++) {
                float off = drain_offset(s, now, c);
                // A column that has already thrown everything past the bottom
                // edge has nothing left to animate: stop repainting it, which
                // is what keeps the drain at two or three strips a frame
                // instead of seven.
                if (off >= 0.0f && off < (float)LAND_H + HOLE_R) mark(s, c);
            }
        }
        break;
    }

    default:
        break;
    }

    if (s->dirty != 0 || s->dirtyAll) {
        flush(s, now);
        s->lastRenderMs = now;
    }
}

// wantsShake is false. Shake belongs only where erasing IS the app's
// identity (sensors.h, decision 0002 section 5), and here it would throw
// away a game in progress on a jolt - the exact thing that section forbids
// generalising the gesture into. The board is cleared by finishing it, or
// by BOOT.
const app_t g_fourApp = {
    .name       = "four",
    .enter      = four_enter,
    .tick       = four_tick,
    .leave      = NULL,
    .landscape  = true,
    .wantsShake = false,
};
