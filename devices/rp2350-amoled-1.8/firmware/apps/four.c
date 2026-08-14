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
 *     width, so it has no corners at all) 58px wide runs the WHOLE height
 *     of the screen, tinted in a pale wash of whoever is about to play. Her
 *     thumb hides ~75px of a 364px shape, so most of it is always visible,
 *     above and below her hand.
 *   - THE WAITING PIECE. A filled disc in the player's colour sits above
 *     the column, the same size as the piece it is about to become. It went
 *     away for one round, replaced by an arrow, and came straight back:
 *     "la fleche est mega moche. Je pense que tu peux l'enlever et remets la
 *     balle au-dessus, quitte a reduire la hauteur."
 *   - THE LANDING RING. The lowest empty hole of that column is drawn as a
 *     thick ring in the player's full colour: the outline of the piece that
 *     is about to be there. This is literally "ou le plot va tomber", and
 *     it is almost always below her thumb, since pieces stack from the
 *     bottom.
 *
 * THE RING EARNS ITS PLACE MORE NOW, NOT LESS. When the cue above the
 * column was an arrow, the case for keeping both was that the arrow said
 * WHICH COLUMN from a fixed place and the ring said WHICH HOLE. The waiting
 * piece says exactly what the arrow said, so nothing changed on that side -
 * and the ring is still the only thing on screen that answers which hole,
 * and still the only one of the two that moves as the column fills. On a
 * six-row board the landing hole is wherever the stack happens to have
 * reached. Dropping the ring would lose information nothing else carries.
 *
 * A full column is said with the same vocabulary rather than a new one: the
 * chute washes grey, there is no landing ring (nothing will land), and the
 * waiting piece goes hollow. Filled means it will happen, hollow means it
 * will not. No words are involved.
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
 * Whose turn it is: SEE SECTION 6. It used to be a secondary cue, because
 * the device played the other side and you always knew it was your turn
 * again the moment its piece landed. Two people passing one small puck do
 * not have that, so it is now the single most important thing on screen and
 * three things say it at once.
 *
 * THE WAITING PIECE CARRIES THAT JOB, and it is the stronger of the two
 * things that have held it. For one round it was an arrow, because the
 * board was made full height and there was nowhere above it for a disc to
 * sit; the owner's verdict on that was immediate and it is the right one on
 * the cue's own terms too. A chevron is a stroke, a piece is a filled 42px
 * disc, and this is the one continuously-present turn cue: the slab tint is
 * deliberately near invisible per pixel and the hand-off's colour sweep
 * lasts HANDOFF_MS and is gone. The steady state - hand-off faded, nobody
 * touching - is a coloured disc bobbing above a tinted board, and that is
 * the frame feature-four.ts photographs and the owner judges.
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
 * 6. TWO PLAYERS. NOTHING PLAYS BY ITSELF. AND WHOSE TURN IT IS.
 * ---------------------------------------------------------------------
 *
 * THE DEVICE USED TO PLAY THE OTHER SIDE, and this section is where that
 * went, kept rather than deleted because the reasoning was confident and
 * wrong and that is worth a paragraph.
 *
 * The argument was: this is a toy for a child who mostly plays alone, so
 * the device should take the other side, and it must play deliberately
 * badly (one ply, and only ALLOWED to act on what it saw 55%/35% of the
 * time) or it beats a five year old every game and she never opens the app
 * again. That was built, and it worked - the owner's verdict on playing it
 * was "ouais c'est pas mal du tout le jeu !". Then, in the same breath:
 * "Il faudrait que ce soit chacun son tour avec deux joueurs. Et quand je
 * place un rouge ca place direct un bleu."
 *
 * The second sentence is not a bug report. It is him describing the
 * opponent working exactly as designed, as the thing he does not want. The
 * mistake was about WHO THIS IS FOR: it is not a child playing alone
 * against a machine, it is two people passing one puck back and forth. So
 * the opponent is gone - deleted, not hidden behind a flag, along with
 * everything that existed only to make a machine legible (the aiming
 * animation, the 620ms think, the weakness roll, the one-ply score, the
 * play-near-the-action term). Both sides are placed by a hand, with the
 * same press-slide-release gesture. Nothing in this file ever moves a piece
 * on its own, and feature-four.ts asserts exactly that: over five seconds
 * of nobody touching the glass, NOT ONE PIXEL BELOW THE HOPPER CHANGES.
 *
 * WHOSE TURN IT IS NOW CARRIES THE WHOLE BURDEN. With a machine playing you
 * always knew, because you had just moved. Two humans sharing a 1.8 inch
 * screen genuinely lose track, no text is allowed, and the only instant the
 * cue changes is the instant it matters. Three things say it, at three
 * different scales, so that missing one is not enough to be lost:
 *
 *   - THE WHOLE BOARD IS TINTED. The slab is warm grey on red's turn and
 *     cool grey on blue's (col_slab()). It is subtle per pixel and enormous
 *     in area, which is the one cue that works in peripheral vision, before
 *     you have focused on anything: whoever is handed the puck sees the
 *     object has changed temperature before they read anything on it. It
 *     also covers the case nothing else does, someone glancing back after
 *     looking away, with no memory of what just happened.
 *   - THE HAND-OFF. At the turn change the new player's chute washes in
 *     full at the centre column and fades out over HANDOFF_MS, while the
 *     new waiting piece POPS in above it (ease_out_back, the same balloon
 *     the palette uses). A colour sweep the height of the screen, arriving
 *     at the exact moment the turn changes, is impossible to miss if you
 *     are looking at the screen at all - which the player who just dropped
 *     necessarily is.
 *   - THE WAITING PIECE, afterwards, bobbing at the top in that player's
 *     colour. This is the steady-state cue, and it is the one that was
 *     carrying this alone before.
 *
 * THE HAND-OFF IS ALSO A LOCK-OUT, and that is half its job. No gesture may
 * arm during it. Without the machine's move in between, "red releases" and
 * "blue may now play" used to be the same instant, and the hand that just
 * dropped a piece is still on the puck: one more press and it has played
 * the other player's move for them. HANDOFF_MS is the beat that makes
 * passing the puck possible, and it is spent showing whose turn it now is
 * rather than waiting for a machine to think.
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
 * THE BOARD IS FULL WIDTH, EDGE TO EDGE. The owner, after playing it: "je
 * pense que j'aimerais qu'elle prenne absolument tout l'ecran en largeur,
 * et faire un test pour prendre tout l'ecran en hauteur aussi."
 *
 * "TOUT L'ECRAN" IS THE VISIBLE SCREEN, NOT THE FRAMEBUFFER, and that
 * distinction is the whole reason this block does its arithmetic on SAFE_*
 * rather than on LAND_W/LAND_H. The framebuffer is the panel's full
 * 368x448 and the firmware addresses all of it, but the case hides a band
 * along every edge - found from a photograph of the real device, after the
 * sketchpad's own palette shipped with its outer row and columns running
 * under the plastic. gfx.h's PANEL_BEZEL_MARGIN_PX is that band, in one
 * shared place; this file uses it and does not keep a second opinion about
 * it. See that constant's own comment for why the number is deliberately
 * rough and where to correct it.
 *
 * THE EMULATOR CANNOT SHOW THIS. It has no bezel, so a layout that bleeds
 * to the framebuffer's edge looks perfect in every screenshot in this
 * repository and loses its edges in a child's hands. That asymmetry is
 * worse here than it was for the palette: a colour cell losing three pixels
 * of its corner is ugly, but a COLUMN losing part of its width is a hit
 * target that cannot be reached, and the outermost columns are the two a
 * thumb already has the most trouble with.
 *
 * So: seven columns share the VISIBLE width, which makes a column CELL
 * (61px at a 10px margin) against a child's ~75px fingertip (AGENTS.md).
 * The column under the thumb is now narrower than the thumb, by more than
 * before. Nothing about that is new in kind, it is worse in degree, and it
 * is the reason the highlight and the arrow matter MORE after this change
 * rather than less.
 *
 * FULL HEIGHT IS A COMPILE-TIME VARIANT, because he asked for it as an
 * experiment ("faire un test pour") rather than as a decision, and the two
 * versions have to be looked at side by side. Build the other one with:
 *
 *   EMU_EXTRA_DEFINES=-DFOUR_FULL_HEIGHT=0 bun run emulator/wasm/build.ts
 *
 * and render both with tools/preview-four.ts.
 *
 * WHY FULL HEIGHT IS THE DEFAULT, AND IT IS NOT A TASTE ARGUMENT. The holes
 * are circles, so the gutter between two of them is the cell pitch minus
 * the hole's diameter, and it is only even if the cells are square. The
 * visible area is 428 x 348 at a 10px margin:
 *
 *   variant           cells      hole r   gutter h / v
 *   ----------------  ---------  -------  ------------
 *   full height       61 x 58      25       11 / 8
 *   with a strip      61 x 49      21       19 / 7
 *
 * 428/7 = 61.1 and 348/6 = 58 are nearly the same number, and 61 against 49
 * is not, so full width very nearly forces full height: the moment the
 * board stops filling the height, its holes stop looking evenly spaced and
 * start looking like a grid that was stretched. The bezel narrowed both
 * numbers without changing that conclusion.
 *
 * THE WAITING PIECE IS GONE and an ARROW does its job (section 3). That is
 * what makes full height possible at all: the strip at the top of the panel
 * existed to hold a 42px disc, and nothing needs to sit outside the board
 * any more.
 * ================================================================== */
#define COLS 7
#define ROWS 6

/* The visible canvas: everything below is measured against THIS, never
 * against LAND_W/LAND_H. One edit to gfx.h's PANEL_BEZEL_MARGIN_PX moves
 * the whole layout, cells, holes and piece together, because nothing here
 * is a literal that was computed by hand from a particular margin. */
#define SAFE_X0 PANEL_BEZEL_MARGIN_PX
#define SAFE_X1 (LAND_W - PANEL_BEZEL_MARGIN_PX)
#define SAFE_Y0 PANEL_BEZEL_MARGIN_PX
#define SAFE_Y1 (LAND_H - PANEL_BEZEL_MARGIN_PX)
#define SAFE_W  (SAFE_X1 - SAFE_X0)
#define SAFE_H  (SAFE_Y1 - SAFE_Y0)

/* THE CELL IS SQUARE, AND 49 IS THE LARGEST ONE THAT FITS.
 *
 * Solved rather than chosen. Six rows of CELL, plus a hopper tall enough to
 * hold a whole waiting piece above the slab, has to fit inside SAFE_H:
 *
 *   piece bottom  = SAFE_Y0 + 2*HOLE_R  = SAFE_Y0 + CELL - 8
 *   slab top      = SAFE_Y1 - SLAB_PAD - ROWS*CELL - SLAB_PAD
 *
 * and the first must clear the second. At a 10px bezel that solves to
 * CELL <= 49.1, so CELL is 49: holes of r=20.5, an 8px gutter in BOTH
 * directions, a 343x294 board, and 42px of paper either side.
 *
 * WHY NOT FULL WIDTH, WHICH IS WHAT WAS ASKED FOR. Because with a hopper it
 * buys nothing. Seven columns across the visible width is a 61px pitch, but
 * the hole's radius is capped by the SHORTER pitch - the vertical one, which
 * six rows plus a hopper pin at 49 - so the holes come out at r=20.5 either
 * way. Identical holes. All the extra 12px per column does is push them
 * apart: the gutter goes from 8/8 to 20/8, which is the stretched-grid look
 * the owner would be seeing instead of a board. Full width was his phrasing
 * for "use the screen", and this uses the screen without distorting the one
 * thing on it that has to look regular.
 *
 * WHAT IT COST, said plainly: 42px of paper down each side, and it is the
 * price of the waiting piece coming back. "la fleche est mega moche. Je
 * pense que tu peux l'enlever et remets la balle au-dessus, quitte a reduire
 * la hauteur." A ball above the board needs a band to sit in; a band costs
 * height; square cells then cost width. He accepted the height explicitly
 * and the width follows from it arithmetically.
 *
 * THE SLAB HUGS THE HOLE GRID, with a 6px rim, and that was the last open
 * question. It was briefly a build variant so that the alternative - the
 * slab stretched to the visible edges with the same holes inside it - could
 * be rendered beside it (preview/four-*-ballwide.png in this file's git
 * history). The narrow one won on the same argument that decided the cell
 * size: the holes are identical either way, so the wide slab spends 12px a
 * column on grey and leaves the holes floating in a field instead of
 * reading as a board. The variant is deleted rather than left behind a
 * define nobody builds, which is how a second code path rots; this
 * paragraph is the part worth keeping.
 */
#define CELL      49
#define SLAB_PAD  6
#define BOARD_W   (COLS * CELL)
#define BOARD_X0  (SAFE_X0 + (SAFE_W - BOARD_W) / 2)
#define BOARD_Y0  (SAFE_Y1 - SLAB_PAD - ROWS * CELL)
#define HOLE_R    ((float)CELL / 2.0f - 4.0f)

// The waiting piece's resting centre, tucked as high in the hopper as the
// visible area allows so the band below it is as short as it can be.
#define HOPPER_CY ((float)SAFE_Y0 + HOLE_R)

#define SLAB_X0   (BOARD_X0 - SLAB_PAD)
#define SLAB_X1   (BOARD_X0 + BOARD_W + SLAB_PAD)
#define SLAB_Y0   (BOARD_Y0 - SLAB_PAD)
#define SLAB_Y1   SAFE_Y1

#define SLAB_CX  ((SLAB_X0 + SLAB_X1) / 2.0f)
#define SLAB_CY  ((SLAB_Y0 + SLAB_Y1) / 2.0f)
#define SLAB_HW  ((SLAB_X1 - SLAB_X0) / 2.0f)
#define SLAB_HH  ((SLAB_Y1 - SLAB_Y0) / 2.0f)
#define SLAB_R   34.0f  // a lozenge, not a filed square

// The chute: the highlight, running the whole VISIBLE height. A stadium,
// since its corner radius equals its half-width. Kept 3px narrower than the
// column either side so a hair of slab still shows, and the lit lane reads
// as being INSIDE the board rather than as a bar laid over it.
#define CHUTE_HW  ((float)CELL / 2.0f - 2.0f)
#define CHUTE_Y0  ((float)SAFE_Y0 + 1.0f)
#define CHUTE_Y1  ((float)SAFE_Y1 - 1.0f)
#define CHUTE_CY  ((CHUTE_Y0 + CHUTE_Y1) / 2.0f)
#define CHUTE_HH  ((CHUTE_Y1 - CHUTE_Y0) / 2.0f)

// Where a dropped piece enters from: one cell above the top row, so it
// slides in from off the board (and, at full height, from off the panel)
// rather than appearing in mid-air.
#define FALL_START_Y HOPPER_CY

// The landing ring's stroke. Thick enough to read as "a piece outline" at
// arm's length rather than as a hairline circle.
#define GHOST_STROKE (HOLE_R / 3.0f)

// How far beyond a column's own cell any of this app's drawing may reach.
// The span a dirty column is repainted through is CELL/2 + SPAN_PAD from
// the column's centre (see span_for_cols), so every radius below must stay
// inside that: the widest is the win halo at HALO_R_MAX.
#define SPAN_PAD 10

/* =====================================================================
 * Timing.
 * ================================================================== */

// Gravity, in px/ms^2, chosen from the drop it has to perform rather than
// from physics: the longest fall on this board is FALL_START_Y to the
// bottom row, ~366px at full height, and 0.005 puts that at
// sqrt(2*366/0.005) = ~380ms - long
// enough to watch the piece travel, short enough that a child who has
// already decided is not waiting on an animation.
#define GRAVITY 0.005f
#define BOUNCE_KEEP 0.30f  // restitution: a soft knock, not a rubber ball
#define BOUNCE_MIN_V 0.10f // below this the piece is done bouncing
#define BOUNCE_MAX 2

// THE HAND-OFF: the beat between one player's piece landing and the other
// being allowed to play, spent announcing whose turn it now is (section 6).
//
// It is a real affordance, not a replacement for the machine's think time
// that used to sit here. Two things need it. The eye needs somewhere to
// land at the instant the only turn cue changes, which is what the fading
// chute and the popping piece give it. And the HAND needs it: the person
// who just dropped a piece still has the puck, and without a beat their
// next press is the other player's move. 420ms is long enough to read as
// deliberate and to pass the puck into, short enough that a player who
// already knows their column is not waiting on it - and the piece is still
// falling for the first ~350ms of the previous move anyway, so what a
// player actually experiences is a continuous handover rather than a pause.
#define HANDOFF_MS 420
// What fraction of the hand-off the waiting piece's pop occupies. See the
// pop's own comment in render_span for why it is not the whole window.
#define HANDOFF_POP_FRAC 0.55f

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
// swell share a phase. Both are derived from HOLE_R so that the two layout
// variants (see the geometry block) cannot drift apart. HALO_R_MAX must
// also stay inside the span a dirty column is repainted through,
// CELL/2 + SPAN_PAD = 40, with room for the anti-aliased edge: at full
// height that is 37 against 40.
#define HALO_R_MIN (HOLE_R + PULSE_GROW + 1.5f)
#define HALO_R_MAX (HALO_R_MIN + 4.5f)
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

// The idle invitation: with nobody's thumb on the glass, the waiting piece
// bobs. This is the steady-state "it is your turn, and you are the one this
// colour", the cue that keeps saying it long after the hand-off's own sweep
// has gone, and it costs one column strip per frame at a throttled rate.
// Amplitude and period were both raised from 4px/1600ms when the device
// stopped playing the other side: it went from a nicety to one of the three
// things carrying the turn (section 6), and it is the only motion on an
// otherwise perfectly still screen, so it should read as breathing rather
// than as drift.
#define BOB_MS 1300.0f
#define BOB_AMP 5.0f
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
 *   grace   P(one gap outlasts it)   P(any does, over 3s of holding still)
 *   ------  ----------------------   ------------------------------------
 *   100ms   3.3e-2                   ~77%   unusable
 *   150ms   6.4e-3                   ~25%
 *   220ms   9.3e-4                   ~4.0%  (sketch.c's LIFT_DEBOUNCE_MS)
 *   260ms   1.4e-4                   ~0.6%
 *   300ms   3.1e-5                   ~0.14%
 *   400ms   1.4e-6                   ~0.006%
 *
 * (3 seconds of contact contains about 44 gaps, since a gap begins on
 * ~14.7 reports per second at this rate. Three seconds is the case that
 * matters: it is a player holding a thumb on the glass while deciding,
 * which is the longest anyone is stationary and therefore the most exposed
 * they ever are.)
 *
 * 260ms was chosen first, over sketch.c's 220ms, because the two failures
 * are not equally bad: a false lift in the sketchpad splits one stroke into
 * two, a cosmetic defect in something still being drawn, where a false lift
 * HERE drops a piece in a column nobody chose, ends that player's turn, and
 * hands the puck over. Nothing in the rules of Connect Four can undo it.
 *
 * IT IS 300ms NOW, and the extra 40ms was bought with a measurement rather
 * than a hunch: repro-touch-dropout-four-drop.ts's own 3-second-hold
 * scenario failed one trial in 25, which is exactly the 0.6% the table
 * predicts and NOT a bug in the state machine. A player thinking for a few
 * seconds a dozen times a game would have met it. 300ms cuts that by four
 * and a half times for 40ms of extra latency on a drop that is already
 * followed by a ~380ms fall - so what a player sees is still a piece
 * beginning to move as their hand clears the glass.
 *
 * IT CANNOT BE MADE ZERO. No grace value makes a Bernoulli process
 * impossible, only rare; 400ms would buy another twenty-fold and start to
 * feel like a delay. That is why the test asserts a RATE with a threshold
 * derived from this table rather than demanding perfection: a working
 * implementation sits near 100% and a broken one (believing the runtime's
 * own touchReleased) sits near 0%, and the gate belongs between those, not
 * at the top.
 *
 * This is the one number in this file that should be re-derived if the
 * measured dropout rate ever changes, and the table above is here so that
 * re-deriving it is arithmetic rather than taste.
 * ================================================================== */
#define RELEASE_GRACE_MS 300

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
 * Red plays first. Neither colour belongs to the device any more (section
 * 6); they are just the two people holding it.
 *
 * Both are the sketchpad palette's own red and blue (sketch.c
 * palette_color(): 0xF800, 0x001F), on purpose: the device should have one
 * vocabulary of colour across its apps, not one per app.
 *
 * The slab is a near-neutral light grey rather than the real toy's blue,
 * because a blue board would compete with a blue player. The pale washes
 * are the two player colours lifted most of the way to white, so a
 * highlighted column reads as "this one is yours" without ever competing
 * with an actual piece.
 *
 * Written as functions rather than static const arrays for the reason
 * sketch.c's palette_color() gives: px_swap() is a real function call and a
 * static initialiser needs a constant expression.
 * ================================================================== */
#define P_NONE  0
#define P_RED   1  // plays first
#define P_BLUE  2

static uint8_t other_player(uint8_t p) { return p == P_RED ? P_BLUE : P_RED; }

static uint16_t col_piece(uint8_t player) {
    return player == P_RED ? px_swap(0xF800)   // red
                           : px_swap(0x001F);  // blue
}

// The washes are deliberately at HALF strength rather than a whisper: a
// first render at #FFCACD read as a faint tint against the #DEDEDE slab on a
// bright screen, and this stripe is the one thing standing between a child
// and not knowing which column she is on. They stay well clear of a real
// piece's own saturation, so a washed column can never be mistaken for a
// column with a piece in it.
static uint16_t col_wash(uint8_t player) {
    return player == P_RED ? px_swap(0xFD55)   // #FFAAAD
                           : px_swap(0xADFF);  // #ADBEFF
}

static uint16_t col_wash_full(void) {
    return px_swap(0xB596);  // #B5B2B5: this column cannot take a piece
}

static uint16_t col_slab(uint8_t player) {
    if (player == P_RED)  return px_swap(0xE6DA);  // #E6DBD6, warm
    if (player == P_BLUE) return px_swap(0xD6DC);  // #D6DBE6, cool
    return px_swap(0xDEFB);                         // #DEDEDE, neutral
}

/* =====================================================================
 * State. One arena allocation, per app.h.
 * ================================================================== */
enum {
    PH_PLAY,     // whoever's turn it is may play; their gesture is live once
                  // the hand-off has elapsed (turnStartMs, section 6)
    PH_FALL,     // a piece is falling
    PH_WIN,      // the winning line breathes
    PH_DRAWPAUSE,// a full board, nobody won: a beat before the drain
    PH_DRAIN,    // the board empties and the next game begins
};

typedef struct {
    uint8_t  board[ROWS * COLS];   // P_NONE / P_RED / P_BLUE, row 0 is the TOP row
    uint8_t  height[COLS];         // pieces in each column, 0..ROWS
    uint8_t  turn;                 // whose move it is
    uint32_t turnStartMs;          // when it became theirs: the hand-off's t=0
    uint8_t  phase;
    uint32_t phaseStartMs;

    // The gesture, whoever's it is. See section 2 of this file's header.
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

    // The winning line: up to 6 cells, since a run can be longer than four.
    uint8_t  winCells[6];
    int      winCount;
    uint8_t  winner;

    uint32_t lastRenderMs;
    uint8_t  dirty;                // bit c set: column c must be repainted
    bool     dirtyAll;
} four_state_t;

static four_state_t *s_state;

/* =====================================================================
 * Small helpers: geometry and the board.
 * ================================================================== */
static float col_x(int c)  { return (float)(BOARD_X0 + CELL / 2 + c * CELL); }
static float row_y(int r)  { return (float)(BOARD_Y0) + (float)CELL / 2.0f + (float)(r * CELL); }

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

// Overshoot-then-settle easing ("back out"): grows past 1.0 before returning
// to exactly 1.0 at t=1, which is what makes a scale-up read as a pop rather
// than a plain grow. The standard closed-form cubic, computed with plain
// multiplication rather than powf - the same function, for the same reason,
// as sketch.c's palette pop-in, which is where this device's "little
// balloon" idiom comes from. It peaks near 1.10x, so the waiting piece
// touches 23px against a span that reaches 33 from the column centre.
static float ease_out_back(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

// How far into the hand-off this turn is, 0 at the instant the turn changed
// and 1 once it is over; negative when there is no hand-off running. See
// section 6: this drives the announcement (a chute washing in and fading
// out, a waiting piece popping in) AND the lock-out that keeps the hand
// which just played from playing the other player's move too.
static float handoff_u(const four_state_t *s, uint32_t nowMs) {
    if (s->phase != PH_PLAY) return -1.0f;
    uint32_t dt = nowMs - s->turnStartMs;
    if (dt >= (uint32_t)HANDOFF_MS) return -1.0f;
    return (float)dt / (float)HANDOFF_MS;
}

static bool gesture_live(const four_state_t *s, uint32_t nowMs) {
    return s->phase == PH_PLAY && handoff_u(s, nowMs) < 0.0f;
}

// Which column is currently highlighted, and in whose colour.
static int hilite_col(const four_state_t *s) {
    if (s->phase == PH_PLAY && s->armed) return s->hoverCol;
    // The chute and its landing ring STAY UP while the piece falls, rather
    // than blinking out the instant the thumb lets go. The fall is the answer
    // to the promise the ring was making, and watching the piece slide down
    // its own lit channel into the exact ring that was already drawn is what
    // makes the promise legible; clearing the highlight first would leave the
    // piece falling through a board that no longer says where it is going.
    if (s->phase == PH_FALL) return s->fallCol;
    return -1;
}

static uint8_t hilite_player(const four_state_t *s) {
    if (s->phase == PH_FALL) return s->fallPlayer;
    return s->turn;
}

// The colour the whole slab wears, which is the peripheral-vision third of
// section 6's turn cue. PH_WIN wears the WINNER's, so the board itself also
// says who won; the drain and the nobody-won beat wear neutral, because no
// side owns those moments.
static uint8_t slab_player(const four_state_t *s) {
    if (s->phase == PH_WIN) return s->winner;
    if (s->phase == PH_DRAWPAUSE || s->phase == PH_DRAIN) return P_NONE;
    if (s->phase == PH_FALL) return s->fallPlayer;
    return s->turn;
}

// Which column the waiting piece sits over, or -1 when there is none. There is none
// during a fall, a celebration or a drain, and that ABSENCE is what says a
// touch means something else now (decision 0002: no modal state, and the
// screen is what says so).
//
// Note that it is NOT -1 merely because nobody is touching the glass: the
// piece sits at parkCol and keeps saying whose turn it is. That is the
// steady state, and after the hand-off has faded it is the ONLY thing left
// saying it besides the slab tint - see section 3.
static int waiting_col(const four_state_t *s) {
    if (s->phase != PH_PLAY) return -1;
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

    // 2. the slab, wearing whose turn it is (section 6).
    fill_rrect(SLAB_CX, SLAB_CY, SLAB_HW, SLAB_HH, SLAB_R, col_slab(slab_player(s)), lx0, lx1, 256);

    // 3. the chute: the highlight (a pale wash of whoever is about to play,
    //    or grey when that column cannot take a piece); or, at a turn change,
    //    the hand-off's own announcement washing out; or, during the drain, a
    //    white channel the pieces slide out through.
    int hc = hilite_col(s);
    float ho = handoff_u(s, nowMs);
    if (s->phase == PH_DRAIN) {
        for (int c = 0; c < COLS; c++) {
            if (drain_offset(s, nowMs, c) < 0.0f) continue;
            fill_rrect(col_x(c), CHUTE_CY, CHUTE_HW, CHUTE_HH, CHUTE_HW, PX_WHITE, lx0, lx1, 256);
        }
    } else if (hc >= 0) {
        bool full = landing_row(s, hc) < 0;
        uint16_t wash = full ? col_wash_full() : col_wash(hilite_player(s));
        fill_rrect(col_x(hc), CHUTE_CY, CHUTE_HW, CHUTE_HH, CHUTE_HW, wash, lx0, lx1, 256);
    } else if (ho >= 0.0f) {
        // Full strength at the instant the turn changes, then linearly out to
        // exactly nothing by the end. It was squared at first, which faded it
        // so fast that it had almost gone by the time the waiting piece
        // finished popping in: the two halves of the announcement barely
        // overlapped, and a capture of the moment showed a blue stripe and no
        // piece. They are one statement and have to be legible together.
        //
        // NO LANDING RING goes with it: this says "your turn, this colour",
        // not "your piece will land here", and a chute still lit once the
        // hand-off is over would be claiming a column nobody has chosen.
        fill_rrect(col_x(s->parkCol), CHUTE_CY, CHUTE_HW, CHUTE_HH, CHUTE_HW,
                   col_wash(s->turn), lx0, lx1, (int)(256.0f * (1.0f - ho)));
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
        // Always, now. The suppression this used to carry existed because
        // the arrow lived INSIDE row 0's hole and would have been drawn
        // concentrically with the ring on a column holding five pieces. The
        // waiting piece sits above the board instead, so there is no hole
        // the two can ever share.
        if (lr >= 0) {
            fill_ring(col_x(hc), row_y(lr), HOLE_R, HOLE_R - GHOST_STROKE,
                      col_piece(hilite_player(s)), lx0, lx1, 256);
        }
    }

    // 6. THE WAITING PIECE, above the column it will fall into: a filled
    //    disc in that player's colour, the same size as the piece it is
    //    about to become. Back after a round as an arrow - "la fleche est
    //    mega moche. Je pense que tu peux l'enlever et remets la balle
    //    au-dessus, quitte a reduire la hauteur" - and it is the stronger
    //    cue of the two anyway (section 3): a filled 42px disc against a
    //    chevron's stroke, at the same job.
    int wc = waiting_col(s);
    if (wc >= 0) {
        float wy = HOPPER_CY;
        float scale = 1.0f;
        if (ho >= 0.0f) {
            // The hand-off's pop: it does not appear, it ARRIVES.
            // ease_out_back overshoots past full size before settling, which
            // is the whole reason this reads as a pop rather than a grow -
            // the same balloon sketch.c's palette uses. Compressed into the
            // first HANDOFF_POP_FRAC of the hand-off so that it finishes
            // while the chute behind it is still clearly lit: the piece
            // arriving and the colour sweep are one statement.
            float u = ho / HANDOFF_POP_FRAC;
            scale = ease_out_back(u > 1.0f ? 1.0f : u);
        } else if (!s->armed) {
            // The idle invitation: it is your turn, and you are this colour.
            // It floats UP from its resting place and settles back, never
            // below - a cue that rises reads as offering itself, and the
            // resting position is already as high in the hopper as the
            // visible area allows, so there is nowhere below to go.
            float phase = fmodf((float)nowMs, BOB_MS) / BOB_MS * 6.2831853f;
            wy -= BOB_AMP * 0.5f * (1.0f - cosf(phase));
        }
        uint8_t wp = hilite_player(s);
        // Hollow when the column under it cannot take a piece: filled means
        // it will happen, hollow means it will not, which is the same
        // vocabulary the grey wash under it uses.
        if (landing_row(s, wc) < 0) {
            fill_ring(col_x(wc), wy, HOLE_R * scale, (HOLE_R - GHOST_STROKE) * scale,
                      col_piece(wp), lx0, lx1, 256);
        } else {
            fill_disc(col_x(wc), wy, HOLE_R * scale, col_piece(wp), lx0, lx1, 256);
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
    s->fallY = FALL_START_Y;
    s->fallV = 0.0f;
    s->bouncesLeft = BOUNCE_MAX;
    mark(s, col);
    printf("four: drop col=%d row=%d player=%d\r\n", col, s->fallRow, (int)player);
}

// Hands the board to `player`: starts their hand-off (the announcement plus
// the lock-out, section 6), parks the waiting piece back at the centre, and
// abandons whatever gesture was in progress. The centre rather than where
// the last piece went, which is what this did when only one human played: it
// is the same neutral starting point for both of them, and it makes the
// hand-off's pop happen at a place the eye already knows to look, every
// single turn, rather than wherever the previous player happened to finish.
// Where the arrow rests between gestures: the centre column, or the nearest
// one to it that can still take a piece. The fallback matters because the
// arrow is now the turn cue (section 3) and a greyed-out arrow over a full
// column says "not here" rather than "your turn" - so the steady state must
// never park on one. Scanned outward from the centre, so it also stays as
// close to the middle as the board allows.
static int park_col(const four_state_t *s) {
    int mid = COLS / 2;
    for (int d = 0; d < COLS; d++) {
        if (mid - d >= 0 && landing_row(s, mid - d) >= 0) return mid - d;
        if (mid + d < COLS && landing_row(s, mid + d) >= 0) return mid + d;
    }
    return mid; // every column full: the game is over anyway
}

static void hand_over_to(four_state_t *s, uint32_t nowMs, uint8_t player) {
    s->turn = player;
    s->turnStartMs = nowMs;
    s->phase = PH_PLAY;
    s->phaseStartMs = nowMs;
    s->parkCol = park_col(s);
    s->hoverCol = -1;
    s->contactSeen = false;
    s->armed = false;
    s->contactCount = 0;
    mark_all(s);
}

static void reset_game(four_state_t *s, uint32_t nowMs) {
    for (int i = 0; i < ROWS * COLS; i++) s->board[i] = P_NONE;
    for (int c = 0; c < COLS; c++) s->height[c] = 0;
    s->winCount = 0;
    s->winner = P_NONE;
    // Red starts every game rather than the loser or the alternate side.
    // Both players are hands now, so there is no fairness argument either
    // way, and a fixed starter is one less thing on a screen that may not
    // use words to explain itself: the board comes up red, and it always
    // comes up red.
    hand_over_to(s, nowMs, P_RED);
    printf("four: new game\r\n");
}

// A piece has finished falling: commit it, and decide what happens next.
static void land_piece(four_state_t *s, uint32_t nowMs) {
    int c = s->fallCol, r = s->fallRow;
    mark(s, c);   // the falling piece has to be replaced by the settled one
                   // in the same tick, whatever the branches below then do
    s->board[r * COLS + c] = s->fallPlayer;
    s->height[c]++;

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

    // Nobody has won and there is room left: the puck goes to the other
    // player. This is the ONLY place a turn changes, and it is the only thing
    // that happens after a piece lands - there is no reply to compute and
    // nothing to schedule.
    hand_over_to(s, nowMs, other_player(s->fallPlayer));
}

/* ---------------------------------------------------------------------
 * The gesture, whoever's it is. Identical for both players - one idiom,
 * learned once, and neither side gets a different one. See section 2 of this
 * file's header comment for why none of this trusts f->touchReleased.
 * ------------------------------------------------------------------- */
static void gesture_tick(four_state_t *s, const app_frame_t *f) {
    bool live = gesture_live(s, f->nowMs);

    if (!live) {
        // Any gesture in progress is abandoned rather than carried across a
        // phase boundary or a hand-off: a finger still down when the next
        // turn opens starts a FRESH gesture and has to arm again. That is
        // most of what stops the hand that just played from playing the other
        // player's move too - the hand-off's lock-out buys the beat, and this
        // makes sure the beat is not spent quietly accumulating contact
        // samples toward an arm the instant it ends.
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

    if (landing_row(s, col) < 0) {
        // A full column: the release does nothing at all, which is what the
        // grey wash and the greyed-out arrow have been saying the whole time
        // the thumb was over it. The turn does not change, and parkCol is
        // deliberately NOT moved here - parking the arrow on a full column
        // would leave the steady state showing a grey arrow, i.e. no turn
        // cue at all until somebody touched the glass again.
        mark(s, col);
        printf("four: release over a full column c=%d, ignored\r\n", col);
        return;
    }
    s->parkCol = col;
    start_fall(s, f->nowMs, col, s->turn);
}

/* =====================================================================
 * enter / tick.
 * ================================================================== */
static void four_enter(void) {
    s_state = APP_STATE(four_state_t);
    four_state_t *s = s_state;

    // No generator to seed any more: with both sides played by a hand there
    // is nothing in this app that has to decide anything, so nothing that has
    // to be unpredictable. That is also what makes a scripted game in the
    // emulator exactly reproducible, which the tests lean on.
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
        if (handoff_u(s, now) >= 0.0f) {
            // The hand-off's own animation: the chute fading out and the
            // waiting piece popping in, both inside the centre column, at the
            // full animation rate because this one is short and is the moment
            // that has to be seen.
            // Nothing catches the exact frame the hand-off ENDS on, and
            // nothing needs to: both animated quantities converge on their
            // settled values (scale 1, alpha 0), so a last frame at u=0.97 is
            // already indistinguishable from the settled one, and the bob
            // below repaints this same column within BOB_STEP_MS regardless.
            if ((now - s->lastRenderMs) >= RENDER_MIN_MS) mark(s, s->parkCol);
        } else if (!s->armed && (now - s->lastRenderMs) >= BOB_STEP_MS) {
            // The idle bob, on its own slower clock. A 5px travel over 1.3s
            // moves under half a pixel per 60fps frame, so repainting at the
            // animation rate would push sixty near-identical frames a second
            // for a change nobody can see; BOB_STEP_MS is the rate at which
            // the bob actually moves about a pixel.
            mark(s, s->parkCol);
        }
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
