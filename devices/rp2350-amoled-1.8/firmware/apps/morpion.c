/*
 * morpion: noughts and crosses, for a child who cannot read.
 *
 * The owner asked for "morpion" in the same breath as the other games for
 * his niece, and the terms are already settled by the app next door: TWO
 * HUMAN PLAYERS, alternating, one puck passed between them. He removed the
 * machine opponent from Connect Four in as many words - "il faudrait que ce
 * soit chacun son tour avec deux joueurs" - so nothing in this file ever
 * plays a move, and feature-morpion.ts asserts that directly rather than
 * inferring it from the absence of an opponent in the source.
 *
 * The gesture is this device's single idiom, learned once: press, drag,
 * release. The palette, Connect Four and the menu all work this way. Put a
 * thumb down, the cell under it shows itself as the one that would be
 * played, release on it to play, release anywhere else to change your mind.
 *
 * Everything below follows from that plus five facts about who is holding
 * this thing and what the panel does.
 *
 * ---------------------------------------------------------------------
 * 1. NINE CELLS, ONE THUMB, AND THE THUMB IS ON TOP OF THE ANSWER
 * ---------------------------------------------------------------------
 *
 * AGENTS.md measures a child's fingertip contact at ~75px on this panel.
 * The cells here are 110px square, so a cell is comfortably bigger than the
 * thumb choosing it - which is already better than Connect Four's 61px
 * columns - but that is not the problem. The problem is that the thumb sits
 * exactly ON the cell it is choosing and hides the middle of it, and the
 * middle is where a mark would go.
 *
 * Connect Four solved its own version of this with a chute: a full-height
 * capsule washing a whole column, so most of the cue lives above and below
 * the hand. A 3x3 grid is that problem in two dimensions, and the answer
 * generalises exactly:
 *
 *   THE CANDIDATE CELL IS SHOWN AS A CROSS OF TWO LIT BANDS: one running
 *   the full height of the board through the candidate's COLUMN, one
 *   running the full width through its ROW. They intersect in exactly one
 *   cell, and that cell is the answer.
 *
 * Why this survives a thumb, stated as arithmetic rather than as taste:
 * each band is 330px long and 104px wide, and a fingertip is one blob about
 * 75px across. It can eat 75px out of the middle of one band and 75px out
 * of the middle of the other, and what is left is four arms reaching about
 * 130px out from under the hand in four different directions, to the four
 * edges of the board. There is no way to hold a single finger that hides
 * two perpendicular bands. And the reading is unambiguous without seeing
 * the crossing at all: one visible vertical arm names the column, one
 * visible horizontal arm names the row, and a column and a row name a cell.
 *
 * Two more cues sit on top of that, both of them redundant on purpose:
 *
 *   - THE CANDIDATE CELL'S OWN DISC is filled in a DEEPER wash than the
 *     bands, so where the arms cross is visibly hotter than the arms. This
 *     one IS under the thumb most of the time. It is here for the moment
 *     the thumb slides off, and for the adult watching over her shoulder.
 *   - THE GHOST MARK: the mark that would be made, drawn in that cell in
 *     the player's colour at part alpha. Faint means "not yet", solid means
 *     "done" - the same filled/hollow vocabulary Connect Four uses for its
 *     waiting piece, and it says WHICH mark as well as which cell.
 *
 * A cell that is already taken is said with the same vocabulary rather than
 * a new one: the bands and the cell wash GREY, and no ghost is drawn.
 * Colour means it will happen, grey means it will not. No words involved.
 *
 * Tracking is immediate: any accepted touch sample repaints the affected
 * strips in the same tick that saw it, never on a later frame.
 *
 * ---------------------------------------------------------------------
 * 2. TOUCH DROPS CONTACT ~34 TIMES A SECOND, AND RELEASE IS THE VERB
 * ---------------------------------------------------------------------
 *
 * This project has already shipped a feature that worked in the emulator
 * and did not work in the owner's hands on exactly this hazard (the
 * sketchpad's palette - see emulator/wasm/tests/repro-touch-dropout-
 * palette-open.ts). A gesture that ends on RELEASE, over a controller that
 * fakes a release dozens of times per second, is precisely the shape that
 * breaks, and this app is a WORSE case than Connect Four: there, a false
 * release drops a piece in a column nobody chose, which is one bad move in
 * a game with 42 cells. Here it plants a mark in one of nine cells, and in
 * noughts and crosses a single unintended move is very often the whole
 * game.
 *
 * So this file does not believe f->touchReleased at all. It keeps its own
 * lastContactMs and only calls a gesture finished once contact has been
 * ABSENT for RELEASE_GRACE_MS - see that constant for the arithmetic that
 * sets it, which is derived from the measured dropout rate rather than
 * picked.
 *
 * The mirror-image hazard is a STRAY - a phantom contact while nothing is
 * touching the glass, which must never place a mark. A gesture only becomes
 * ARMED after ARM_SAMPLES contact samples sustained at ARM_RATE_HZ, which
 * no burst of strays can satisfy at any spacing. See the ARMING block.
 *
 * AND THERE IS A THIRD ONE HERE THAT CONNECT FOUR DID NOT HAVE: the
 * reported position of a STILL finger wanders, by 80 to 250px, for up to
 * three reports at a time. One axis over seven full-height columns could
 * absorb that; two axes over a 110px grid cannot, and measuring it before
 * writing any defence put one release in ten in a cell the finger was never
 * on. See SETTLE_MS for what separates a wander from a move (how long it
 * lasts, not how far it goes), what the fix costs, and why it cannot be
 * made zero.
 *
 * ---------------------------------------------------------------------
 * 3. NO TEXT. ANYWHERE. NOT WHOSE TURN, NOT WHO WON, NOT A DRAW.
 * ---------------------------------------------------------------------
 *
 * WHOSE TURN IT IS is the thing two people sharing a 1.8 inch screen
 * genuinely lose track of, and Connect Four's own experience is that the
 * cue matters most AT THE INSTANT IT CHANGES, with the previous player's
 * hand still on the puck. Four things say it here, at four different
 * scales, so that missing one is not enough to be lost:
 *
 *   - THE PLAYERS ARE DOUBLE-CODED, by shape AND by colour: X is red, O is
 *     blue. Noughts and crosses already distinguishes its two sides by
 *     shape, which is a vocabulary a child has before she has words; the
 *     red/blue on top of it is this device's own colour pair (the same red
 *     and blue as the sketchpad palette and Connect Four), so a glance at
 *     hue answers the same question as a glance at shape. Neither cue can
 *     fail alone.
 *   - THE WHOLE BOARD IS TINTED: warm grey on red's turn, cool grey on
 *     blue's. Subtle per pixel and enormous in area, which is the one cue
 *     that works in peripheral vision, before anyone has focused on
 *     anything - and the one that covers a player glancing back after
 *     looking away with no memory of what just happened.
 *   - THE HAND-OFF, at the turn change: the new player's CROSS washes in at
 *     full strength over the centre cell and fades out over HANDOFF_MS,
 *     while the new mark POPS into the tray. A colour sweep the height AND
 *     width of the board, arriving at the exact moment the turn changes, is
 *     impossible to miss for the player who has just moved and is
 *     necessarily still looking. It also teaches the cross: the shape that
 *     announces a turn is the same shape that will follow her thumb.
 *   - THE TRAY, which is this app's steady state and its one real
 *     improvement over Connect Four's waiting piece. Down the left of the
 *     visible area, OUTSIDE the board entirely, sits the mark whose turn it
 *     is, bobbing gently. It never moves and a thumb on the board never
 *     covers it, so unlike a waiting piece that rides the hovered column,
 *     the turn cue in this app is never under the hand. See the TRAY block
 *     for why the layout can afford it.
 *
 * THE HAND-OFF IS ALSO A LOCK-OUT, and that is half its job: no gesture may
 * arm during it. Without it, "red releases" and "blue may now play" are the
 * same instant while the hand that just played is still on the puck, and
 * one more press has played the other player's move for them.
 *
 * WHO WON, without the word: the three in a line are STRUCK THROUGH. A
 * bowed ink stroke draws itself along them, end to end, in the winner's
 * colour, exactly the way a hand strikes through a winning row on paper -
 * and it is drawn on over STRIKE_MS rather than appearing, because the
 * drawing of it is what makes it read as a verdict rather than as
 * decoration. The three struck marks then breathe (they pulse in size and
 * bloom a ring outward) and the whole board wears the winner's colour, so
 * the winner's identity is carried by the same red/blue-and-shape
 * vocabulary as everything else on screen.
 *
 * NOBODY WON, without the word: no strike, no pulse, no bloom. A beat of
 * stillness on a board gone neutral grey, and then the wipe. THE ABSENCE OF
 * THE CELEBRATION IS THE DIFFERENCE, which is the only distinction
 * available without words and is also the true one - a draw is precisely
 * the game where the thing that normally happens does not. Connect Four
 * reached the same answer for the same reason, and two games on one device
 * telling her "nobody won" the same way is worth more than either one being
 * cleverer.
 *
 * STARTING THE NEXT GAME, with no text and no button: THE BOARD IS WIPED.
 * A soft pale bar sweeps across it left to right and the marks fade out
 * just behind it, the way a hand wipes a slate. That is the real object's
 * own reset, it needs no menu and no button, and it happens by itself after
 * the celebration (or after the draw's beat of stillness). A touch during
 * the celebration only skips ahead to it, so a touch never MEANS two
 * different things - decision 0002's "no modal state" - it just means "hurry
 * up". A wipe reads as "nothing here now" to somebody who cannot read
 * "new game", and it is the same object the whole time, which a screen that
 * blanked and redrew would not be.
 *
 * ---------------------------------------------------------------------
 * 4. DECISION 0009, AND A GAME WHOSE BOARD IS FOUR STRAIGHT LINES
 * ---------------------------------------------------------------------
 *
 * "j'aimerais que sur toutes nos apps on ait pas de bords trop durs [...]
 * ou d'angles trop droits ou de traits trop droits". A noughts and crosses
 * grid is four dead-straight lines meeting at four right angles: it is
 * about the purest violation of that rule a screen can hold, more so than
 * Connect Four's rectangles, since there the lines were incidental and here
 * they ARE the board.
 *
 * The resolution is the one Connect Four found, and it holds even better
 * here: THERE IS NO GRID DRAWN. Not softened, not rounded: absent.
 *
 * What is drawn is one rounded slab (a 346x346 square with a 46px corner
 * radius, so it reads as a lozenge rather than as a square with filed
 * corners) with NINE ROUND DISCS punched out of it in paper white. The grid
 * exists only as the ARRANGEMENT of nine circles, which is how a printed
 * game board for a child who cannot read is actually drawn - circles to put
 * things in, not lines to stay inside. There is no cell border, no rule and
 * no right angle anywhere in this file. The two lit bands are stadiums
 * (corner radius equal to half-width, so no straight-to-straight junction
 * exists in them at all), every disc is a circle, every halo is a ring.
 *
 * THE MARKS THEMSELVES ARE THE HARDER HALF, because an X is two straight
 * strokes at 45 degrees and that is what an X is. They are drawn as INK,
 * not as geometry: each arm is a gently BOWED quadratic, stamped as a chain
 * of round-capped capsules that taper from 4.5px at the ends to 7.5px in
 * the middle, so what lands on the panel is what a hand does over a short
 * line - "prefer one deliberate curve over many small random ones",
 * decision 0009's own words. The O is the same brush swept once round a
 * circle whose radius wobbles a few percent, starting and ending past
 * itself so the stroke overlaps where a hand would have closed it. Neither
 * mark is a stamp: both are drawn ON, stroke after stroke, when they are
 * played (see PH_INK), which is what makes them read as somebody's writing.
 *
 * Decision 0009 warns specifically that shapes_fill_between_curves_aa_land
 * takes integer column arrays and therefore CANNOT bow - any deviation
 * quantises into a staircase at any amplitude - and that anything which
 * needs to be other than straight must be painted with the float brush.
 * Nothing here uses it. Every edge in this file is anti-aliased by coverage
 * from a float signed distance.
 *
 * Why local primitives rather than shapes.h's float family, which does bow:
 * the same reason four.c gives. shapes.h composites by MIN (darkest wins),
 * which is right for ink on paper and wrong here - this app paints opaque
 * colour over opaque colour (a mark over a disc over a slab) and MIN would
 * turn a red X on a grey slab into a muddy union. It also cannot clip to an
 * x span, which is the entire basis of this file's push discipline
 * (section 5). So blend_land/fill_disc/fill_ring/fill_rrect/fill_capsule
 * below are the same signed-distance-to-coverage idea with source-over
 * blending and a clip.
 *
 * ---------------------------------------------------------------------
 * 5. DECISION 0001, MADE IMPOSSIBLE TO BREAK RATHER THAN GOT RIGHT
 * ---------------------------------------------------------------------
 *
 * Every pushed window's row length must be a multiple of 8 pixels, and no
 * pixel may change outside a pushed rectangle. Both have bitten this
 * project repeatedly, including mid-animation, so this file does what
 * four.c does and removes the arithmetic entirely.
 *
 * gfx maps a landscape rectangle (lx, ly, w, h) to the panel rectangle
 * (PANEL_W-(ly+h), lx, h, w): width and height swap, so THE PUSHED ROW
 * LENGTH IS THE LANDSCAPE HEIGHT. Therefore:
 *
 *   EVERY REDRAW IN THIS APP IS A FULL-HEIGHT LANDSCAPE STRIP.
 *
 * ly is always 0 and h is always LAND_H = 368, a multiple of 8, so the row
 * length is 368 on every single push, at rest and mid-animation alike, with
 * nothing for gfx_push to round and no edge case at the panel boundary.
 *
 * And render_span() redraws EVERYTHING that intersects the strip, from
 * white, every time - slab, discs, bands, marks, strike, halos, tray, wipe
 * - rather than patching what it believes changed. A strip is therefore a
 * pure function of (state, time), so nothing can be left behind inside it,
 * and nothing outside it is touched because no primitive is allowed to draw
 * outside the clip it is handed. Dirty tracking picks WHICH strips to
 * repaint, and being wrong about that can only ever cost a stale strip,
 * never a stray pixel.
 *
 * THE STRIPS ARE THE TRAY PLUS THE THREE BOARD COLUMNS, four in all. The
 * row band spans all three board strips, so a candidate moving to a
 * different ROW dirties all three while one moving along a row dirties only
 * two - which is stated here because it is the one place this app costs
 * more per frame than Connect Four did, and it is the price of the second
 * band being what makes nine cells legible under a thumb.
 *
 * ---------------------------------------------------------------------
 * 6. WHAT DOES NOT SURVIVE, ON PURPOSE
 * ---------------------------------------------------------------------
 *
 * A game does not survive switching apps. The arena is zeroed on switch
 * (app.h) and nothing here fights that. It would cost one file-scope static
 * of a few dozen bytes, which the SRAM budget would genuinely not notice,
 * so this is a decision and not a limitation, and it is the same one
 * four.c's section 7 records:
 *
 *   - Every app on this device forgets. chrono resets, timer resets, the
 *     sketchpad's canvas IS the framebuffer and the runtime clears it. An
 *     app that alone remembered would be the odd object in the drawer.
 *   - A half-finished board she left ten minutes ago, whose plan she has
 *     forgotten, is a memory test. A fresh board is a game. She is five.
 *   - Decision 0002 section 4b: each app is an object and refers to nothing
 *     outside itself. Persisting across a switch is the app knowing that a
 *     switch happened.
 *
 * It matters slightly less here than it did there, which is worth saying so
 * nobody "fixes" this on the grounds that it matters more: a nine-cell
 * board takes under a minute to play, so the window in which a switch can
 * cost anything is small. Do not fix it.
 *
 * SOUND is deliberately not wired, for four.c's reason: sound.h offers
 * exactly one sound, the timer's alarm chime, and a mark being drawn is not
 * that sound. Borrowing the alarm for it would teach her the alarm means
 * nothing in particular. A pen stroke needs its own scratch, which is a
 * sound_synth.c change, not an app change.
 */
#include <stdio.h>
#include <math.h>

#include "app.h"
#include "gfx.h"

/* =====================================================================
 * Geometry, in LANDSCAPE coordinates (LAND_W x LAND_H = 448 x 368).
 *
 * "TOUT L'ECRAN" IS THE VISIBLE SCREEN, NOT THE FRAMEBUFFER. The case hides
 * a band along every edge (gfx.h's PANEL_BEZEL_MARGIN_PX, found from a
 * photograph of the real device after the sketchpad's palette shipped with
 * its outer row running under the plastic). Everything below is measured
 * against SAFE_*, never against LAND_W/LAND_H, so one edit to that constant
 * moves the whole layout together. The emulator has no bezel and therefore
 * cannot show this, which is exactly why the margin has a name.
 *
 * THE BOARD IS SQUARE AND AS TALL AS THE VISIBLE AREA ALLOWS, which decides
 * everything else. Nine equal circles only read as a board if the cells are
 * square, so the cell is capped by the SHORTER axis, the height:
 *
 *   SAFE_H = 348, minus two 8px slab rims, over three rows  ->  CELL = 110
 *
 * and 110px against a child's ~75px fingertip (AGENTS.md) is a genuinely
 * forgiving target, nearly twice Connect Four's 61px column.
 *
 * THE 82px LEFT OVER ACROSS THE WIDTH IS THE TRAY, and getting it for free
 * is the piece of luck this layout is built around. SAFE_W is 428 and the
 * board is 346 wide, so there are 82px of paper that a square board cannot
 * use. Connect Four had to BUY the band its waiting piece sits in by
 * shortening its cells; here the band already exists, on the axis that had
 * spare room, and it sits beside the board rather than above it - which is
 * why the turn cue in this app is somewhere a thumb on the board never
 * reaches. The board is pushed flush to the right of the visible area and
 * the tray takes the whole remainder on the left, rather than the board
 * being centred with 41px of dead paper on each side: 41px holds nothing,
 * 82px holds a legible mark.
 * ================================================================== */
#define GRID 3

#define SAFE_X0 PANEL_BEZEL_MARGIN_PX
#define SAFE_X1 (LAND_W - PANEL_BEZEL_MARGIN_PX)
#define SAFE_Y0 PANEL_BEZEL_MARGIN_PX
#define SAFE_Y1 (LAND_H - PANEL_BEZEL_MARGIN_PX)
#define SAFE_W  (SAFE_X1 - SAFE_X0)
#define SAFE_H  (SAFE_Y1 - SAFE_Y0)

// The slab's rim around the disc grid. 8px, the same order as four.c's 6,
// and it is what keeps the outermost discs from touching the lozenge's own
// curve.
#define SLAB_PAD 8

#define CELL     ((SAFE_H - 2 * SLAB_PAD) / GRID)   // 110
#define BOARD_W  (GRID * CELL)                      // 330
#define SLAB_W   (BOARD_W + 2 * SLAB_PAD)           // 346
#define SLAB_H   SLAB_W                             // square, by construction

#define SLAB_X1  SAFE_X1                            // flush right; see the tray
#define SLAB_X0  (SLAB_X1 - SLAB_W)                 // 92
#define SLAB_Y0  (SAFE_Y0 + (SAFE_H - SLAB_H) / 2)  // 11
#define SLAB_Y1  (SLAB_Y0 + SLAB_H)                 // 357

#define BOARD_X0 (SLAB_X0 + SLAB_PAD)               // 100
#define BOARD_Y0 (SLAB_Y0 + SLAB_PAD)               // 19

#define SLAB_CX  ((SLAB_X0 + SLAB_X1) / 2.0f)
#define SLAB_CY  ((SLAB_Y0 + SLAB_Y1) / 2.0f)
#define SLAB_HW  (SLAB_W / 2.0f)
#define SLAB_HH  (SLAB_H / 2.0f)
// A lozenge, not a filed square: 46 on a 346px side is a corner arc a
// quarter of the way along each edge, which is far enough past "rounded
// rectangle" that no straight-to-curve junction reads as a corner.
#define SLAB_R   46.0f

/* THE DISC. 39px radius in a 110px cell leaves a 32px gutter between two
 * discs, and the gutter is not slack: it is where the lit bands actually
 * show. A band is 104px wide and a disc is 78px across, so what a lit
 * column looks like is a 104px ribbon with round bites taken out of it -
 * three full-width 104x32 blocks between the discs, plus a 13px sliver down
 * each side of every disc, plus the slab's own rim at each end. Shrinking
 * the disc is what buys those blocks. A first pass at r=50 left 10px of
 * gutter and the "band" was a hairline outline nobody would see across a
 * room, which defeats the entire point of section 1. */
#define DISC_R   39.0f

/* THE MARK. An X whose four corners sit ON the circle the O is drawn round
 * is the classic pairing and reads as the same size even though its
 * bounding box is smaller: MARK_ARM is the X's endpoint offset on each
 * axis, so its corners are at MARK_ARM*sqrt(2) = 28.3 from the centre,
 * against the O's MARK_R of 27. Both then have to clear the disc with room
 * for the ink: 28.3 + 7.5 = 35.8 and 27*1.035*1.02 + 7.5 = 36.0 (the O's
 * wobble and its slight ovality included), against DISC_R = 39. */
#define MARK_R    27.0f
#define MARK_ARM  20.0f
#define MARK_STROKE_MID 7.5f
#define MARK_STROKE_END 4.5f
// How far the X's arms bow off their own chord. A hand does not draw a
// straight 56px line; 4px over that span is what it actually does - visible
// as a curve, nowhere near a wobble (decision 0009: "not wobble for its own
// sake").
#define MARK_BOW 4.0f
// The ghost, i.e. the mark that WOULD be made, in the candidate cell. Faint
// enough to read as a proposal, solid enough to survive the deeper wash it
// is drawn on.
#define GHOST_ALPHA 118

/* THE TRAY: the mark whose turn it is, outside the board on the left. */
#define TRAY_X0    SAFE_X0
#define TRAY_X1    SLAB_X0
#define TRAY_CX    ((TRAY_X0 + TRAY_X1) / 2.0f)     // 51
#define TRAY_CY    ((SLAB_Y0 + SLAB_Y1) / 2.0f)     // 184, level with the middle row
// 0.72 puts the tray mark's outer reach at 26px inside a band whose
// half-width is 41: big enough to be the turn cue at arm's length, clearly
// smaller than a played mark so it never reads as a tenth cell.
#define TRAY_SCALE 0.72f

/* THE BANDS. Stadiums (corner radius = half-width), 3px narrower than the
 * cell either side so a hair of slab still shows and the lit lane reads as
 * being INSIDE the board rather than as a bar laid over it - four.c's chute,
 * one per axis. */
#define BAND_HALF  ((float)CELL / 2.0f - 3.0f)      // 52
#define BAND_LONG  (SLAB_W / 2.0f - 3.0f)           // 170

/* THE WIN STRIKE: an ink stroke through the three, overshooting each end so
 * it reads as struck through rather than as joined up.
 *
 * BOTH OF THESE NUMBERS WERE SET FROM THE RENDER, not solved, and the first
 * pass had them wrong in both directions. At 9px half-width the stroke
 * covered the marks it was striking, so a won board showed a blue bar and
 * you had to work out what was under it; 7 leaves each mark legible through
 * its own strike, which matters because the marks are half of what says who
 * won. And a 12px bow over a 315px span is a straight line as far as the
 * eye is concerned - decision 0009's whole complaint - where 18 reads as a
 * hand that swept through rather than a ruler laid across.
 *
 * Both stay inside the strip a winning cell is repainted through: the
 * overshoot is 34 and the bow 18 plus 7 of ink, against a half-cell of 55.
 * A vertical win is the tight case, since there the bow is horizontal. */
#define STRIKE_OVER    34.0f
#define STRIKE_BOW     18.0f
#define STRIKE_R_MID   7.0f
#define STRIKE_R_END   3.0f

/* THE WIN BLOOM: a ring growing out of each struck mark. Its inner radius
 * has to start outside the swollen mark or it spends the part of its life
 * where it is brightest hidden inside what it is meant to be coming out of.
 * HALO_R_MAX = 46 against the half-cell of 55, so it never leaves the strip
 * its own cell is repainted through. */
#define PULSE_SCALE  0.10f
#define HALO_R_MIN   38.0f
#define HALO_R_MAX   46.0f
#define HALO_STROKE  3.0f

/* =====================================================================
 * Timing.
 * ================================================================== */

// The mark is DRAWN ON rather than stamped (section 4). 340ms is long
// enough to see a hand make it, short enough that a child who has already
// decided is not waiting on an animation - and it is also what gives the
// hand-off a natural predecessor beat, the job Connect Four's ~380ms fall
// was doing for free.
#define INK_MS 340
// An X is two strokes with a lift between them: the first over the opening
// 45% of INK_MS, a beat, the second over the closing 45%. Without the beat
// the two arms grow simultaneously and it reads as a shape appearing rather
// than as two strokes being made.
#define INK_STROKE_FRAC 0.45f

// THE HAND-OFF: the beat between one player's mark landing and the other
// being allowed to play, spent announcing whose turn it now is (section 3).
// 420ms, the same number four.c settled on and for the same two reasons -
// the eye needs somewhere to land at the instant the only turn cue changes,
// and the HAND needs a beat to pass the puck into, because the person who
// just played is still holding it.
#define HANDOFF_MS 420
// What fraction of the hand-off the tray mark's pop occupies. Kept well
// short of the whole window so the pop finishes while the announcing cross
// behind it is still clearly lit: the mark arriving and the colour sweep
// are one statement, and four.c's own first version had them barely
// overlapping.
#define HANDOFF_POP_FRAC 0.55f

// The strike draws itself along the winning three. Slower per pixel than a
// mark, because it is longer and because this is the sentence the whole
// game was building to.
#define STRIKE_MS 420
#define PULSE_MS  900.0f

// The celebration runs until touched, but a child who has wandered off
// should not come back to a device stuck on a four-day-old win, and the
// pulse is a repaint. WIN_MAX_MS caps it; WIN_SKIP_MS is how long it is
// protected from the touch that skips it, so the finger that made the
// winning mark (or a stray) cannot end the celebration before she has seen
// it. It is deliberately longer than STRIKE_MS: skipping before the strike
// has finished drawing would throw away the one thing that says who won.
#define WIN_MAX_MS  9000
#define WIN_SKIP_MS 1200

// A full board with nobody winning: a beat of stillness, long enough to
// read as deliberate and short enough not to feel like a hang. The absence
// of a strike and a pulse is what says "nobody won" (section 3).
#define DRAW_PAUSE_MS 900

// THE WIPE. A pale bar sweeps across the board and the marks fade out just
// behind it. WIPE_FADE is how far behind the bar a mark takes to go: at 90
// against a 110px cell pitch, neighbouring columns fade in sequence rather
// than together, which is what makes it read as a sweep rather than as a
// fade to nothing.
#define WIPE_MS         780
#define WIPE_FADE       90.0f
#define WIPE_FRONT_HALF 26.0f
// THE STRIKE IS UN-DRAWN first, over the opening of the wipe: the pen
// retraces its own path backwards and the stroke retreats into the corner
// it started from. Two reasons it is not simply faded out with the marks.
// It spans the whole board, so anything that faded it as ONE object at ONE
// x would have it vanishing while half of it was still ahead of the bar.
// And a retraction is the same gesture reversed, which is a thing a hand
// does, where a stroke dissolving in place is not.
#define WIPE_STRIKE_MS  260

// The idle invitation: with nobody's thumb on the glass, the tray mark
// bobs. It is the steady-state "it is your turn, and you are this one", and
// it is the only motion on an otherwise perfectly still screen, so it
// should read as breathing rather than as drift. It costs one tray strip
// per repaint, which is the cheapest strip on the screen.
#define BOB_MS      1300.0f
#define BOB_AMP     5.0f
#define BOB_STEP_MS 60

// Repaint throttle for animations. Everything animated here is a pure
// function of (state, time) integrated from f->nowMs, so it runs at the
// same wall-clock speed whatever the loop rate is; only the REPAINT is
// throttled. 16ms is ~60fps, which is what the ink stroke and the tray pop
// get since they touch one strip each. The celebration and the wipe get
// WIDE_RENDER_MIN_MS instead: a strike spans all three board strips, and
// pushing three quarters of the panel sixty times a second buys nothing the
// eye can see. 24ms is ~42fps, which is still smooth for a shape moving a
// couple of pixels a frame.
#define RENDER_MIN_MS      16
#define WIDE_RENDER_MIN_MS 24

/* =====================================================================
 * Touch, and the dropout arithmetic that sets RELEASE_GRACE_MS.
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
 * (3 seconds of contact contains about 44 gaps, since a gap begins on ~14.7
 * reports per second at this rate. Three seconds is the case that matters:
 * it is a player holding a thumb on the glass while deciding, which is the
 * longest anyone is stationary and therefore the most exposed they ever
 * are. In this game that is the NORMAL way to play - nine cells and a
 * winning threat on the board is exactly when somebody stops and thinks.)
 *
 * 300ms, the same value four.c arrived at, and the reasoning is copied
 * rather than the number. The two failures being traded are not equally
 * bad. A false lift in the sketchpad splits one stroke into two, a cosmetic
 * defect in something still being drawn. A false lift HERE plants a mark in
 * a cell nobody chose, ends that player's turn and hands the puck over, and
 * nothing in the rules can undo it - and with nine cells rather than
 * forty-two, one unintended mark is very often the game. So this app buys
 * further along the same curve than the sketchpad does, exactly as four.c
 * did, and for a strictly stronger version of its reason.
 *
 * Why not further still: 400ms would buy another twenty-fold and start to
 * feel like a delay, and IT CANNOT BE MADE ZERO in any case - no grace
 * value makes a Bernoulli process impossible, only rare. That is why the
 * test asserts a RATE with a threshold taken from this table rather than
 * demanding perfection: a working implementation sits near 100% and a
 * broken one (believing the runtime's own touchReleased) sits near 0%, and
 * the gate belongs between those, not at the top.
 *
 * This is the one number in this file that should be re-derived if the
 * measured dropout rate ever changes, and the table is here so that
 * re-deriving it is arithmetic rather than taste.
 * ================================================================== */
#define RELEASE_GRACE_MS 300

/* ---------------------------------------------------------------------
 * ARMING, and the stray problem, which is the dropout problem's mirror
 * image and needs a different answer.
 *
 * A stray is a phantom contact reported while nothing is touching the
 * glass. It must never place a mark: a device sitting in a bag playing by
 * itself is, to a child, the toy being broken.
 *
 * Counting samples does not separate the two, and four.c has the scar to
 * prove it: RELEASE_GRACE_MS keeps a candidate gesture alive for 300ms
 * after its last contact, so ANY two strays inside that window armed a
 * gesture and the release verdict then played a move. What actually
 * separates a finger from a stray is not how many samples arrive but how
 * DENSELY: a finger reports contact on about 43% of reports even under the
 * worst measured dropout rate (~26 contacts per second), where the modelled
 * stray process runs at 0.2 per second. So arming needs ARM_SAMPLES
 * contacts AND a sustained rate of at least ARM_RATE_HZ, which gives a
 * hard, non-statistical guarantee rather than a probability:
 *
 *   NO BURST OF FEWER THAN ARM_SAMPLES PHANTOM CONTACTS CAN EVER ARM A
 *   GESTURE, AT ANY SPACING.
 *
 * WHAT IT COSTS. A real finger needs about 4/0.43 = 9 reports, ~155ms, to
 * produce 4 contacts under the worst dropout profile, so the cross appears
 * about a sixth of a second after her thumb lands. That is the initial
 * appearance only: once armed, tracking is immediate, every accepted sample
 * repainting in its own tick.
 * ------------------------------------------------------------------- */
#define ARM_SAMPLES 4
#define ARM_MS      40
#define ARM_RATE_HZ 15u

/* ---------------------------------------------------------------------
 * THE THIRD HAZARD, WHICH IS THIS APP'S OWN AND NOT CONNECT FOUR'S: THE
 * REPORTED POSITION OF A STILL FINGER WANDERS.
 *
 * emulator/src/touchsim.ts models it because it was measured, not imagined:
 * a TOUCH_POLL_SELFTEST session with the owner's finger held deliberately
 * still counted ten confirmed jumps in forty seconds, the reported centroid
 * moving 80 to 250px and holding the wrong spot for up to three reports
 * before correcting itself.
 *
 * Connect Four could largely shrug at that. It reads ONE axis, its columns
 * are the full height of the screen, and a jump that stays inside a column
 * changes nothing. This app reads BOTH axes over a 110px grid, so a 150px
 * wander is two cells away, and if it is what the controller happens to be
 * saying when the finger lifts, the mark goes there.
 *
 * MEASURED, BEFORE ANY OF THE BELOW WAS WRITTEN: at the calibrated jitter
 * rate, roughly one release in ten placed the mark in a cell the finger was
 * never on (repro-touch-dropout-morpion.ts, scenario E). In a nine-move
 * game that is about one wrong mark per game, in a game where one wrong
 * mark is usually the whole result. That is not a defect worth reporting
 * and shipping.
 *
 * WHAT SEPARATES A WANDER FROM A MOVE IS HOW LONG IT LASTS, not how far it
 * goes. An episode is at most three reports; dropouts stretch the wall
 * clock it covers (a dropped report does not consume the episode, see
 * touchsim.ts's refresh) to about 116ms for the longest ones, and it then
 * reverts to the truth. A finger that has genuinely arrived somewhere stays
 * there: the player is looking at the board deciding, and she has to
 * decelerate and lift, which is far longer than that.
 *
 * So the candidate cell and the cell a release PLAYS are separated:
 *
 *   - hoverCell tracks every accepted sample IMMEDIATELY. The cross follows
 *     the thumb with no lag at all, which is what the owner asked for in
 *     Connect Four ("voir en surbrillance ou le plot va tomber") and what
 *     makes the gesture feel like direct manipulation.
 *   - playCell only becomes hoverCell once hoverCell has survived
 *     SETTLE_MS of CONTACT. A wander shorter than that never gets to be a
 *     move.
 *
 * SETTLE_MS is 90 and that is a trade, stated rather than hidden. It
 * rejects every one-report episode (about 39ms of wall clock at the
 * measured dropout rate) and most two-report ones (about 78ms), which is
 * where the bulk of them are, and it does NOT reject the longest
 * three-report ones. Pushing it to 150 would catch those too and would
 * start rejecting something real: a player who slides onto a cell and lifts
 * promptly would have her previous cell played instead, which is a lie the
 * screen can be caught in, where a wander is at worst a 50ms flicker of the
 * cross that corrects itself.
 *
 * IT CANNOT BE MADE ZERO, and the reason is worth being blunt about: "the
 * finger moved there and lifted" and "the controller lied for the last
 * 100ms" are the same signal, and no causal filter can tell them apart. The
 * test therefore asserts the property that IS keepable - the mark always
 * lands in a cell the controller actually reported, never one this app
 * invented - and reports the rate as news.
 * ------------------------------------------------------------------- */
#define SETTLE_MS 90

/* =====================================================================
 * Colour.
 *
 * RED and BLUE, this device's own pair: the maximally separated colours on
 * this panel, opposite ends of the hue circle, both dark enough to hold
 * their edge against white paper AND against the grey slab. They are the
 * sketchpad palette's own red and blue (sketch.c palette_color(): 0xF800,
 * 0x001F) and Connect Four's, on purpose - the device should have one
 * vocabulary of colour across its apps, not one per app.
 *
 * X IS RED AND PLAYS FIRST; O IS BLUE. The shape is the primary
 * distinction, because that is what noughts and crosses IS and a child has
 * shapes before she has words; the colour is a second, independent channel
 * saying the same thing, so a player who has not yet learned which shape is
 * hers can still play by colour and vice versa.
 *
 * The slab is a near-neutral light grey rather than a colour, because a
 * coloured board would compete with a player. Its three tints are four.c's
 * exactly, so the two board games change temperature by the same amount in
 * the same direction and a child learns the cue once.
 *
 * Written as functions rather than static const arrays for the reason
 * sketch.c's palette_color() gives: px_swap() is a real function call and a
 * static initialiser needs a constant expression.
 * ================================================================== */
#define P_NONE 0
#define P_X    1  // red, plays first
#define P_O    2  // blue

static uint8_t other_player(uint8_t p) { return p == P_X ? P_O : P_X; }

static uint16_t col_mark(uint8_t player) {
    return player == P_X ? px_swap(0xF800)   // red
                         : px_swap(0x001F);  // blue
}

// The bands. Half strength rather than a whisper, for four.c's measured
// reason: a first pass at #FFCACD read as a faint tint against the #DEDEDE
// slab on a bright screen, and these ribbons are the one thing standing
// between a child and not knowing which cell she is on. They stay well
// clear of a real mark's saturation, so a lit cell can never be mistaken
// for a played one.
static uint16_t col_band(uint8_t player) {
    return player == P_X ? px_swap(0xFD55)   // #FFAAAD
                         : px_swap(0xADFF);  // #ADBEFF
}

// The candidate cell's own disc: deeper than the bands, so where the two
// arms cross is visibly hotter than the arms themselves.
static uint16_t col_cell(uint8_t player) {
    return player == P_X ? px_swap(0xFB6E)   // #FF6E73
                         : px_swap(0x6BFF);  // #6E7CFF
}

// This cell is taken: the bands and the cell go grey, in the same two
// strengths as the colours they replace, so the layout does not change -
// only its temperature does.
static uint16_t col_band_taken(void) { return px_swap(0xB596); }  // #B5B2B5
static uint16_t col_cell_taken(void) { return px_swap(0xD6BA); }  // #D9D6D9

static uint16_t col_slab(uint8_t player) {
    if (player == P_X) return px_swap(0xE6DA);  // #E6DBD6, warm
    if (player == P_O) return px_swap(0xD6DC);  // #D6DBE6, cool
    return px_swap(0xDEFB);                      // #DEDEDE, neutral
}

/* =====================================================================
 * State. One arena allocation, per app.h.
 *
 * WHAT IT ACTUALLY COSTS, measured and printed by enter() rather than
 * estimated: 10240 bytes of the 64KB arena, 15.6%. The game is nine bytes
 * of it. The rest is two buffers and it is worth saying which, because "a
 * 3x3 board is nothing" is true and is not what this app spends:
 *
 *   ~9.2KB  cov[], the scratch a TRANSLUCENT stroke rasterises through -
 *           see ink_path_soft() for why source-over blending forces a
 *           two-pass union and cannot just chain dabs like an opaque one.
 *   ~950B   the two GLYPH PATHS, the hand-inked X and O in cell-local
 *           pixels, built once in enter() and thereafter only scaled and
 *           translated. In the arena rather than in a file-scope table for
 *           app.h's reason (state belongs here so its cost is one number in
 *           one place), and precomputed rather than generated per frame for
 *           a second one: every sinf/cosf is a wasm-to-JS call in the
 *           emulator, and building them at draw time would be a few hundred
 *           of those per repainted strip.
 *   ~70B    the game, the gesture and the phase clocks.
 * ================================================================== */
enum {
    PH_PLAY,      // whoever's turn it is may play, once the hand-off has elapsed
    PH_INK,       // their mark is being drawn on
    PH_WIN,       // the strike draws through the three, then they breathe
    PH_DRAWPAUSE, // a full board, nobody won: a beat before the wipe
    PH_WIPE,      // the board is wiped clean and the next game begins
};

#define GLYPH_MAX_PTS 26

// The scratch coverage buffer a TRANSLUCENT stroke needs - see
// ink_path_soft() for why one exists at all. 96 is the widest a mark can
// ever be (its ink reaches 36px from the cell centre, plus a pixel of
// feather each side), and the only strokes that are ever translucent are
// single marks: the ghost, and a mark fading under the wipe.
#define COV_MAX 96

typedef struct {
    float x[GLYPH_MAX_PTS];
    float y[GLYPH_MAX_PTS];  // cell-local, centre at (0,0)
    float r[GLYPH_MAX_PTS];  // the pen's half-width at that point
    int   n;
} glyph_path_t;

typedef struct {
    uint8_t  board[GRID * GRID];   // P_NONE / P_X / P_O, index r*GRID + c
    uint8_t  turn;
    uint32_t turnStartMs;          // when it became theirs: the hand-off's t=0
    bool     handoffLive;          // a hand-off was still animating last tick
    uint8_t  phase;
    uint32_t phaseStartMs;

    // The gesture, whoever's it is. See section 2 of this file's header.
    bool     contactSeen;          // a gesture is in progress (dropouts included)
    bool     armed;                // it has survived ARM_SAMPLES/ARM_MS
    uint32_t gestureStartMs;
    uint32_t lastContactMs;        // last sample that actually reported contact
    int      contactCount;
    int      hoverCell;            // the candidate cell, shown immediately, or -1
    uint32_t hoverSinceMs;         // when the thumb arrived on it
    int      playCell;             // ...and the one a release would play: see SETTLE_MS

    // The mark being drawn on.
    int      inkCell;
    uint8_t  inkPlayer;

    // The three in a line.
    uint8_t  winCells[GRID];
    int      winCount;
    uint8_t  winner;

    uint32_t lastRenderMs;
    uint8_t  dirty;                // bit 0: the tray. bits 1..3: board columns.
    bool     dirtyAll;

    glyph_path_t oPath;
    glyph_path_t xPath[2];

    uint8_t  cov[COV_MAX * COV_MAX];   // ink_path_soft()'s scratch
} morpion_state_t;

static morpion_state_t *s_state;

/* =====================================================================
 * Small helpers: geometry and the board.
 * ================================================================== */
static float cell_cx(int c) { return (float)(BOARD_X0 + CELL / 2 + c * CELL); }
static float cell_cy(int r) { return (float)(BOARD_Y0 + CELL / 2 + r * CELL); }

// Which cell a landscape point belongs to. CLAMPED rather than rejected, on
// both axes, so there is no point on the canvas that means nothing - the
// same "no dead space a touch can fall into" rule menu.c's
// column_rect_land() and four.c's col_from_x() both follow. The tray
// therefore belongs to column 0, which is worth stating plainly because the
// tray is a visible object and touching an object usually means something:
// here it means "column 0", and the cross lights up to say so before
// anything is committed. Nothing is lost by that, because the gesture is
// press-DRAG-release precisely so a wrong landing costs a slide rather than
// a move.
static int cell_from_land(int lx, int ly) {
    int c = (lx - BOARD_X0) / CELL;
    if (lx < BOARD_X0) c = 0;   // integer division truncates toward zero,
    int r = (ly - BOARD_Y0) / CELL;
    if (ly < BOARD_Y0) r = 0;   // so negatives need this on both axes
    if (c < 0) c = 0;
    if (c > GRID - 1) c = GRID - 1;
    if (r < 0) r = 0;
    if (r > GRID - 1) r = GRID - 1;
    return r * GRID + c;
}

static const uint8_t LINES[8][GRID] = {
    { 0, 1, 2 }, { 3, 4, 5 }, { 6, 7, 8 },   // rows
    { 0, 3, 6 }, { 1, 4, 7 }, { 2, 5, 8 },   // columns
    { 0, 4, 8 }, { 2, 4, 6 },                 // diagonals
};

// The line through `cell` that `p` has just completed, written into out[].
// Returns 0 when there is none. The first match wins: two lines can only
// complete on the same move in positions this game cannot reach from an
// empty board with alternating play.
static int win_line(const morpion_state_t *s, int cell, uint8_t p, uint8_t *out) {
    for (int i = 0; i < 8; i++) {
        bool holds = false;
        for (int k = 0; k < GRID; k++) if (LINES[i][k] == (uint8_t)cell) holds = true;
        if (!holds) continue;
        bool all = true;
        for (int k = 0; k < GRID; k++) if (s->board[LINES[i][k]] != p) { all = false; break; }
        if (!all) continue;
        for (int k = 0; k < GRID; k++) out[k] = LINES[i][k];
        return GRID;
    }
    return 0;
}

static bool board_full(const morpion_state_t *s) {
    for (int i = 0; i < GRID * GRID; i++) if (s->board[i] == P_NONE) return false;
    return true;
}

/* =====================================================================
 * Drawing primitives: signed distance -> coverage -> source-over blend,
 * clipped to a landscape x span.
 *
 * Same technique as four.c's and sketch.c's, with the same two differences
 * from shapes.h's family that section 4 of this file's header argues for:
 * composition is source-over (opaque colour over opaque colour, not ink
 * onto paper), and every function takes the clip span section 5's push
 * discipline is built on.
 *
 * The sqrtf is confined to the ~1px feather at each edge: the interior is
 * accepted and the exterior rejected by squared-distance comparisons. That
 * matters on the board and it matters more in the emulator, where every
 * sqrtf is a wasm-to-JS call.
 *
 * gfx.h rotates RECTANGLES, not pixels, so a landscape app placing
 * individual pixels maps its own coordinates - gfx.h's own documented
 * mapping, (lx, ly) -> (PANEL_W-1-ly, lx), the same single exception
 * shapes.c's aa_composite_land() and four.c's blend_land() take.
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
                float cov = oOut - d;
                float covIn = d - (rIn - 0.5f);
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
// which is what the two bands use.
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

// A round-capped capsule from (x0,y0,r0) to (x1,y1,r1): the pen, one dab of
// it. sketch.c's draw_capsule, source-over and clipped instead of
// MIN-composited. The radius is interpolated along the segment, which is
// what makes a tapered stroke possible at all - the standard approximation
// (project, then subtract the interpolated radius), exact for r0 == r1 and
// visually indistinguishable at the gentle tapers this file uses.
static void fill_capsule(float x0, float y0, float r0, float x1, float y1, float r1,
                         uint16_t colorPx, int clipX0, int clipX1, int alpha) {
    if (alpha <= 0) return;
    float rMax = r0 > r1 ? r0 : r1;
    if (rMax <= 0.0f) return;
    float loX = x0 < x1 ? x0 : x1, hiX = x0 > x1 ? x0 : x1;
    float loY = y0 < y1 ? y0 : y1, hiY = y0 > y1 ? y0 : y1;

    int minX = (int)floorf(loX - rMax - 1.0f); if (minX < clipX0) minX = clipX0;
    int maxX = (int)ceilf(hiX + rMax + 1.0f);  if (maxX > clipX1 - 1) maxX = clipX1 - 1;
    int minY = (int)floorf(loY - rMax - 1.0f); if (minY < 0) minY = 0;
    int maxY = (int)ceilf(hiY + rMax + 1.0f);  if (maxY > LAND_H - 1) maxY = LAND_H - 1;
    if (minX > maxX || minY > maxY) return;

    float ex = x1 - x0, ey = y1 - y0;
    float len2 = ex * ex + ey * ey;
    if (len2 < 1e-6f) len2 = 1e-6f;

    for (int y = minY; y <= maxY; y++) {
        float py = (float)y + 0.5f;
        for (int x = minX; x <= maxX; x++) {
            float px = (float)x + 0.5f;
            float t = ((px - x0) * ex + (py - y0) * ey) / len2;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            float dx = px - (x0 + t * ex), dy = py - (y0 + t * ey);
            float d2 = dx * dx + dy * dy;
            float rr = r0 + (r1 - r0) * t;
            float rOut = rr + 0.5f;
            if (d2 >= rOut * rOut) continue;
            int a = alpha;
            float rIn = rr - 0.5f;
            if (rIn <= 0.0f || d2 > rIn * rIn) {
                float cov = rOut - sqrtf(d2);
                if (cov <= 0.0f) continue;
                if (cov > 1.0f) cov = 1.0f;
                a = (int)(cov * (float)alpha);
            }
            blend_land(x, y, colorPx, a);
        }
    }
}

/* ---------------------------------------------------------------------
 * INK: a stroke along a path, optionally only partway along it.
 *
 * The path is a point list with a per-point pen half-width, and the stroke
 * is that list stamped as a chain of capsules - sketch.c's own construction
 * and shapes_fill_capsule_aa_land's, which is decision 0009's instruction
 * for anything that must not be straight ("if a shape needs to be anything
 * other than straight, it must be painted with the float brush").
 *
 * `frac` is how far along the path the pen has got, 0 to 1, which is what
 * lets a mark be DRAWN ON rather than appear. It is measured in path
 * parameter (index fraction) rather than in arc length: every path in this
 * file is sampled at even parameter steps along a smooth curve, so the two
 * differ by a few percent and only in the middle of the stroke, where a
 * human eye watching a pen move has nothing to compare it against.
 *
 * Consecutive capsules overlap heavily, and where they do the interior is
 * opaque on both passes, so the double-blend is confined to the ~1px
 * feather at a join between two same-coloured dabs. That is why this is a
 * chain of capsules rather than a min-distance field over the whole path:
 * the artefact is a fraction of a pixel of extra hardness at a join, and
 * the field would cost every pixel of the bounding box a pass over every
 * segment.
 * ------------------------------------------------------------------- */
static void ink_path(const float *xs, const float *ys, const float *rs, int n,
                     float frac, uint16_t colorPx, int clipX0, int clipX1, int alpha) {
    if (n < 2 || frac <= 0.0f || alpha <= 0) return;
    if (frac > 1.0f) frac = 1.0f;
    float p = frac * (float)(n - 1);
    int full = (int)p;
    if (full > n - 1) full = n - 1;
    float rem = p - (float)full;
    for (int i = 0; i < full; i++) {
        fill_capsule(xs[i], ys[i], rs[i], xs[i + 1], ys[i + 1], rs[i + 1],
                     colorPx, clipX0, clipX1, alpha);
    }
    if (full < n - 1 && rem > 0.0f) {
        float ex = xs[full + 1] - xs[full], ey = ys[full + 1] - ys[full];
        float er = rs[full + 1] - rs[full];
        fill_capsule(xs[full], ys[full], rs[full],
                     xs[full] + ex * rem, ys[full] + ey * rem, rs[full] + er * rem,
                     colorPx, clipX0, clipX1, alpha);
    }
}

/* ---------------------------------------------------------------------
 * THE SAME STROKE, TRANSLUCENT, and it needs a completely different pass.
 *
 * Source-over blending is not idempotent: painting alpha 118 twice over the
 * same pixel is not alpha 118, it is 195. A stroke is a chain of capsules
 * that overlap each other heavily by construction (that is what makes it
 * one continuous line rather than a row of dashes), so drawing a
 * translucent one with ink_path() above does not produce a faint stroke -
 * it produces an almost-opaque one, unevenly, darker wherever more dabs
 * happen to pile up. feature-morpion.ts caught exactly that on the ghost:
 * 383 of its pixels came out at full ink saturation, i.e. indistinguishable
 * from a mark that had actually been played, which is the one thing the
 * ghost must never look like.
 *
 * So a translucent stroke is rasterised in two passes: accumulate the UNION
 * of every dab's coverage into a scratch buffer (max, not sum - a union,
 * which is what a single stroke of a pen is), then blend that buffer onto
 * the framebuffer exactly once. The result is a uniformly faint stroke with
 * anti-aliased edges and no seam at any join.
 *
 * The scratch buffer is why this is not simply how every stroke is drawn:
 * it bounds the shape to COV_MAX square, which is fine for a mark and is
 * not fine for the win strike, and it costs a clear plus a second pass over
 * the box. Opaque strokes have no idempotence problem (painting opaque over
 * opaque is opaque), so they keep the cheap chain. The rule is exactly:
 * alpha 256 chains, anything less accumulates.
 * ------------------------------------------------------------------- */
static void ink_path_soft(morpion_state_t *s,
                          const float *xs, const float *ys, const float *rs, int n,
                          float frac, uint16_t colorPx, int clipX0, int clipX1, int alpha) {
    if (n < 2 || frac <= 0.0f || alpha <= 0) return;
    if (frac > 1.0f) frac = 1.0f;

    // The drawn part of the path, partial last segment included, as a plain
    // polyline: the coverage pass below has no notion of "how far along".
    float px[GLYPH_MAX_PTS + 1], py[GLYPH_MAX_PTS + 1], pr[GLYPH_MAX_PTS + 1];
    float p = frac * (float)(n - 1);
    int full = (int)p;
    if (full > n - 1) full = n - 1;
    float rem = p - (float)full;
    int m = 0;
    for (int i = 0; i <= full && i < n && m < GLYPH_MAX_PTS + 1; i++) {
        px[m] = xs[i]; py[m] = ys[i]; pr[m] = rs[i]; m++;
    }
    if (full < n - 1 && rem > 0.0f && m < GLYPH_MAX_PTS + 1) {
        px[m] = xs[full] + (xs[full + 1] - xs[full]) * rem;
        py[m] = ys[full] + (ys[full + 1] - ys[full]) * rem;
        pr[m] = rs[full] + (rs[full + 1] - rs[full]) * rem;
        m++;
    }
    if (m < 2) return;

    float loX = px[0], hiX = px[0], loY = py[0], hiY = py[0], rMax = pr[0];
    for (int i = 1; i < m; i++) {
        if (px[i] < loX) loX = px[i];
        if (px[i] > hiX) hiX = px[i];
        if (py[i] < loY) loY = py[i];
        if (py[i] > hiY) hiY = py[i];
        if (pr[i] > rMax) rMax = pr[i];
    }
    int x0 = (int)floorf(loX - rMax - 1.0f); if (x0 < clipX0) x0 = clipX0;
    int x1 = (int)ceilf(hiX + rMax + 1.0f);  if (x1 > clipX1 - 1) x1 = clipX1 - 1;
    int y0 = (int)floorf(loY - rMax - 1.0f); if (y0 < 0) y0 = 0;
    int y1 = (int)ceilf(hiY + rMax + 1.0f);  if (y1 > LAND_H - 1) y1 = LAND_H - 1;
    if (x1 < x0 || y1 < y0) return;
    int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    if (bw > COV_MAX || bh > COV_MAX) {
        // Cannot happen for anything this app draws translucent (see
        // COV_MAX), and if it ever does, a slightly uneven stroke beats a
        // missing one.
        ink_path(xs, ys, rs, n, frac, colorPx, clipX0, clipX1, alpha);
        return;
    }
    for (int i = 0; i < bw * bh; i++) s->cov[i] = 0;

    for (int seg = 0; seg + 1 < m; seg++) {
        float ax = px[seg], ay = py[seg], ar = pr[seg];
        float bx = px[seg + 1], by = py[seg + 1], br = pr[seg + 1];
        float segR = ar > br ? ar : br;
        int sx0 = (int)floorf((ax < bx ? ax : bx) - segR - 1.0f); if (sx0 < x0) sx0 = x0;
        int sx1 = (int)ceilf((ax > bx ? ax : bx) + segR + 1.0f);  if (sx1 > x1) sx1 = x1;
        int sy0 = (int)floorf((ay < by ? ay : by) - segR - 1.0f); if (sy0 < y0) sy0 = y0;
        int sy1 = (int)ceilf((ay > by ? ay : by) + segR + 1.0f);  if (sy1 > y1) sy1 = y1;
        float ex = bx - ax, ey = by - ay;
        float len2 = ex * ex + ey * ey;
        if (len2 < 1e-6f) len2 = 1e-6f;
        for (int y = sy0; y <= sy1; y++) {
            float qy = (float)y + 0.5f;
            for (int x = sx0; x <= sx1; x++) {
                float qx = (float)x + 0.5f;
                float t = ((qx - ax) * ex + (qy - ay) * ey) / len2;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                float dx = qx - (ax + t * ex), dy = qy - (ay + t * ey);
                float d2 = dx * dx + dy * dy;
                float rr = ar + (br - ar) * t;
                float rOut = rr + 0.5f;
                if (d2 >= rOut * rOut) continue;
                int c = 255;
                float rIn = rr - 0.5f;
                if (rIn <= 0.0f || d2 > rIn * rIn) {
                    float cvg = rOut - sqrtf(d2);
                    if (cvg <= 0.0f) continue;
                    if (cvg > 1.0f) cvg = 1.0f;
                    c = (int)(cvg * 255.0f);
                }
                int idx = (y - y0) * bw + (x - x0);
                if (c > s->cov[idx]) s->cov[idx] = (uint8_t)c;
            }
        }
    }

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int c = s->cov[(y - y0) * bw + (x - x0)];
            if (c == 0) continue;
            blend_land(x, y, colorPx, (c * alpha) / 255);
        }
    }
}

// A glyph path placed on the board: the stored cell-local path scaled about
// its own centre and moved to (cx, cy). Copied into a local array rather
// than drawn point by point because both passes need the whole list to
// interpolate the partial last segment.
static void ink_glyph_path(morpion_state_t *s, const glyph_path_t *g,
                           float cx, float cy, float scale,
                           float frac, uint16_t colorPx, int clipX0, int clipX1, int alpha) {
    float xs[GLYPH_MAX_PTS], ys[GLYPH_MAX_PTS], rs[GLYPH_MAX_PTS];
    for (int i = 0; i < g->n; i++) {
        xs[i] = cx + g->x[i] * scale;
        ys[i] = cy + g->y[i] * scale;
        rs[i] = g->r[i] * scale;
    }
    if (alpha >= 256) ink_path(xs, ys, rs, g->n, frac, colorPx, clipX0, clipX1, alpha);
    else ink_path_soft(s, xs, ys, rs, g->n, frac, colorPx, clipX0, clipX1, alpha);
}

// A whole mark. `frac` draws it on: an O is one continuous sweep, an X is
// two strokes with a lift between them (INK_STROKE_FRAC), because two arms
// growing at once reads as a shape appearing rather than as somebody
// writing.
static void draw_mark(morpion_state_t *s, uint8_t player, float cx, float cy,
                      float scale, float frac, uint16_t colorPx,
                      int clipX0, int clipX1, int alpha) {
    if (player == P_O) {
        ink_glyph_path(s, &s->oPath, cx, cy, scale, frac, colorPx, clipX0, clipX1, alpha);
        return;
    }
    float f0 = frac / INK_STROKE_FRAC;
    if (f0 > 1.0f) f0 = 1.0f;
    ink_glyph_path(s, &s->xPath[0], cx, cy, scale, f0, colorPx, clipX0, clipX1, alpha);
    float f1 = (frac - (1.0f - INK_STROKE_FRAC)) / INK_STROKE_FRAC;
    if (f1 > 0.0f) {
        if (f1 > 1.0f) f1 = 1.0f;
        ink_glyph_path(s, &s->xPath[1], cx, cy, scale, f1, colorPx, clipX0, clipX1, alpha);
    }
}

/* ---------------------------------------------------------------------
 * Building the two glyphs, once, in enter().
 * ------------------------------------------------------------------- */

// The pen's profile along a stroke: thin where it lands and lifts, full in
// the middle. One half-sine, which is what a pen pressed and released
// actually does and what sketch.c's own tapered strokes look like.
static float pen_taper(float u) {
    return MARK_STROKE_END + (MARK_STROKE_MID - MARK_STROKE_END) * sinf(u * 3.14159265f);
}

// A bowed arm of the X: a quadratic from (x0,y0) to (x1,y1) whose control
// point is pushed off the chord's midpoint by MARK_BOW along the chord's
// own perpendicular. Flattened into GLYPH points; seven is plenty for a
// 56px span whose maximum deviation from a straight line is 2px, and the
// capsules between them are round-capped so no facet can show at a join.
static void build_x_arm(glyph_path_t *g, float x0, float y0, float x1, float y1, float bow) {
    const int NP = 7;
    float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-3f) len = 1e-3f;
    float px = -dy / len, py = dx / len;         // the chord's perpendicular
    float ctrlX = mx + px * bow * 2.0f;          // a quadratic reaches half
    float ctrlY = my + py * bow * 2.0f;          // its control offset at t=0.5
    g->n = NP;
    for (int i = 0; i < NP; i++) {
        float t = (float)i / (float)(NP - 1);
        float it = 1.0f - t;
        g->x[i] = it * it * x0 + 2.0f * it * t * ctrlX + t * t * x1;
        g->y[i] = it * it * y0 + 2.0f * it * t * ctrlY + t * t * y1;
        g->r[i] = pen_taper(t);
    }
}

// The O: one sweep round a circle, slightly oval and with a few percent of
// radius wobble, starting and ending past itself so the stroke overlaps
// where a hand would have closed it. That overlap is the whole difference
// between a drawn O and a printed circle, and it costs nothing but the
// extra 0.17 radians of sweep.
static void build_o(glyph_path_t *g) {
    const int NP = GLYPH_MAX_PTS;
    const float start = -1.75f, sweep = 6.45f;   // 2*pi is 6.283: it overshoots
    g->n = NP;
    for (int i = 0; i < NP; i++) {
        float u = (float)i / (float)(NP - 1);
        float a = start + u * sweep;
        float wob = 1.0f + 0.035f * sinf(3.0f * a + 0.7f);
        g->x[i] = cosf(a) * MARK_R * wob;
        g->y[i] = sinf(a) * MARK_R * wob * 1.02f;
        g->r[i] = pen_taper(u);
    }
}

/* =====================================================================
 * Rendering.
 *
 * render_span() is a pure function of (state, time) over one full-height
 * landscape strip: it starts from white and redraws every element that
 * intersects the strip, in back-to-front order. It never patches. See
 * section 5 of this file's header for why this, and not tighter dirty
 * rectangles, is the design.
 * ================================================================== */

// Overshoot-then-settle easing ("back out"): grows past 1.0 before
// returning to exactly 1.0 at t=1, which is what makes a scale-up read as a
// pop rather than a plain grow. The standard closed-form cubic, computed
// with plain multiplication rather than powf - the same function, for the
// same reason, as sketch.c's palette pop-in and four.c's waiting piece.
static float ease_out_back(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

// How far into the hand-off this turn is, 0 at the instant the turn changed
// and 1 once it is over; negative when there is none running.
static float handoff_u(const morpion_state_t *s, uint32_t nowMs) {
    if (s->phase != PH_PLAY) return -1.0f;
    uint32_t dt = nowMs - s->turnStartMs;
    if (dt >= (uint32_t)HANDOFF_MS) return -1.0f;
    return (float)dt / (float)HANDOFF_MS;
}

static bool gesture_live(const morpion_state_t *s, uint32_t nowMs) {
    return s->phase == PH_PLAY && handoff_u(s, nowMs) < 0.0f;
}

// Which cell the cross is lit on, and in whose colour. The cross STAYS UP
// while the mark is being drawn on, rather than blinking out the instant
// the thumb lets go: the ink is the answer to the promise the cross was
// making, and watching it appear inside the cross that was already there is
// what makes the promise legible.
static int cue_cell(const morpion_state_t *s) {
    if (s->phase == PH_PLAY && s->armed) return s->hoverCell;
    if (s->phase == PH_INK) return s->inkCell;
    return -1;
}

static uint8_t cue_player(const morpion_state_t *s) {
    if (s->phase == PH_INK) return s->inkPlayer;
    return s->turn;
}

// The colour the whole slab wears, which is the peripheral-vision cue of
// section 3. PH_WIN wears the WINNER's, so the board itself also says who
// won; the wipe and the nobody-won beat wear neutral, because no side owns
// those moments and the neutral IS the news in the second case.
static uint8_t slab_player(const morpion_state_t *s) {
    if (s->phase == PH_WIN) return s->winner;
    if (s->phase == PH_DRAWPAUSE || s->phase == PH_WIPE) return P_NONE;
    if (s->phase == PH_INK) return s->inkPlayer;
    return s->turn;
}

// The wipe's leading edge, in landscape x. Starts clear of the board's left
// side and ends clear of its right, with WIPE_FADE of run-out so the last
// column has somewhere to finish fading.
static float wipe_front(const morpion_state_t *s, uint32_t nowMs) {
    float u = (float)(nowMs - s->phaseStartMs) / (float)WIPE_MS;
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    float x0 = (float)BOARD_X0 - WIPE_FRONT_HALF - 4.0f;
    float x1 = (float)(BOARD_X0 + BOARD_W) + WIPE_FADE + WIPE_FRONT_HALF;
    return x0 + (x1 - x0) * u;
}

// How much of a mark at landscape x is left, during the wipe: full until
// the bar reaches it, gone WIPE_FADE past it. 256 outside PH_WIPE.
static int wipe_alpha(const morpion_state_t *s, uint32_t nowMs, float x) {
    if (s->phase != PH_WIPE) return 256;
    float f = wipe_front(s, nowMs);
    float a = (x + WIPE_FADE - f) / WIPE_FADE;
    if (a <= 0.0f) return 0;
    if (a >= 1.0f) return 256;
    return (int)(a * 256.0f);
}

static void render_span(morpion_state_t *s, uint32_t nowMs, int lx0, int lx1) {
    if (lx0 < 0) lx0 = 0;
    if (lx1 > LAND_W) lx1 = LAND_W;
    if (lx1 <= lx0) return;

    // 1. paper.
    gfx_fill_rect_land(lx0, 0, lx1 - lx0, LAND_H, PX_WHITE);

    // 2. the slab, wearing whose turn it is (section 3).
    fill_rrect(SLAB_CX, SLAB_CY, SLAB_HW, SLAB_HH, SLAB_R,
               col_slab(slab_player(s)), lx0, lx1, 256);

    // 3. THE CROSS: the column band and the row band, over the slab and
    //    under the discs, so what shows of them is the ribbon between and
    //    beside the discs (section 1). Or, at a turn change, the hand-off's
    //    own announcement washing out over the centre cell.
    int cue = cue_cell(s);
    uint8_t cueP = cue_player(s);
    float ho = handoff_u(s, nowMs);
    // "Taken" is only a thing to say while somebody is CHOOSING. During
    // PH_INK the cell under the cross is occupied by definition - start_ink
    // commits the mark the instant it begins being drawn - and washing the
    // cross grey there would turn the moment the promise is kept into the
    // moment it is refused, in the same vocabulary the app uses for a cell
    // that will not take a mark.
    bool cueTaken = (s->phase == PH_PLAY) && cue >= 0 && s->board[cue] != P_NONE;
    if (cue >= 0) {
        uint16_t band = cueTaken ? col_band_taken() : col_band(cueP);
        int cc = cue % GRID, cr = cue / GRID;
        fill_rrect(cell_cx(cc), SLAB_CY, BAND_HALF, BAND_LONG, BAND_HALF, band, lx0, lx1, 256);
        fill_rrect(SLAB_CX, cell_cy(cr), BAND_LONG, BAND_HALF, BAND_HALF, band, lx0, lx1, 256);
    } else if (ho >= 0.0f) {
        // Full strength at the instant the turn changes, then linearly out
        // to exactly nothing by the end. Linear rather than eased, for
        // four.c's measured reason: a squared fade had almost gone by the
        // time the tray mark finished popping in, and the two halves of the
        // announcement are one statement that has to be legible together.
        int a = (int)(256.0f * (1.0f - ho));
        uint16_t band = col_band(s->turn);
        fill_rrect(cell_cx(1), SLAB_CY, BAND_HALF, BAND_LONG, BAND_HALF, band, lx0, lx1, a);
        fill_rrect(SLAB_CX, cell_cy(1), BAND_LONG, BAND_HALF, BAND_HALF, band, lx0, lx1, a);
    }

    // 4. the nine discs, punched out of the slab in paper white - except
    //    the candidate's own, which is filled in the deeper wash so that
    //    where the two arms cross is hotter than the arms. There is no cell
    //    border to draw, which is what keeps the board free of any line at
    //    all (section 4).
    for (int r = 0; r < GRID; r++) {
        for (int c = 0; c < GRID; c++) {
            int cell = r * GRID + c;
            uint16_t fillPx = PX_WHITE;
            if (cell == cue) fillPx = cueTaken ? col_cell_taken() : col_cell(cueP);
            fill_disc(cell_cx(c), cell_cy(r), DISC_R, fillPx, lx0, lx1, 256);
        }
    }

    // 5. the marks. A struck mark swells and shrinks (PULSE_SCALE) so the
    //    white of its own disc opens up around it at the trough and the
    //    breath is legible in both directions.
    float pulse = 1.0f, haloR = 0.0f;
    int haloAlpha = 0;
    if (s->phase == PH_WIN) {
        float u = fmodf((float)(nowMs - s->phaseStartMs), PULSE_MS) / PULSE_MS;
        pulse = 1.0f + PULSE_SCALE * sinf(u * 6.2831853f);
        haloR = HALO_R_MIN + (HALO_R_MAX - HALO_R_MIN) * u;
        haloAlpha = (int)(256.0f * (1.0f - u));
    }

    for (int r = 0; r < GRID; r++) {
        for (int c = 0; c < GRID; c++) {
            int cell = r * GRID + c;
            uint8_t v = s->board[cell];
            if (v == P_NONE) continue;
            float cx = cell_cx(c), cy = cell_cy(r);
            if (cx + DISC_R * 1.4f < (float)lx0 || cx - DISC_R * 1.4f > (float)lx1) continue;

            float scale = 1.0f;
            if (s->phase == PH_WIN) {
                for (int k = 0; k < s->winCount; k++) if (s->winCells[k] == (uint8_t)cell) scale = pulse;
            }
            // The one being drawn on right now is the same mark at a
            // partial frac, not a different object.
            float frac = 1.0f;
            if (s->phase == PH_INK && cell == s->inkCell) {
                frac = (float)(nowMs - s->phaseStartMs) / (float)INK_MS;
                if (frac > 1.0f) frac = 1.0f;
            }
            int a = wipe_alpha(s, nowMs, cx);
            if (a <= 0) continue;
            draw_mark(s, v, cx, cy, scale, frac, col_mark(v), lx0, lx1, a);
        }
    }

    // 5b. the bloom, in a pass of its own AFTER every mark: a halo grows
    //     well past its own disc, across the slab and over its neighbours,
    //     so drawing it inline would leave it half-covered by whichever
    //     cell happened to be painted next.
    if (s->phase == PH_WIN) {
        for (int k = 0; k < s->winCount; k++) {
            int cell = s->winCells[k];
            fill_ring(cell_cx(cell % GRID), cell_cy(cell / GRID), haloR, haloR - HALO_STROKE,
                      col_mark(s->winner), lx0, lx1, haloAlpha);
        }
    }

    // 6. THE STRIKE through the three, over everything on the board, drawn
    //    on end to end. It is the answer to "who won" and it says it the
    //    way a hand says it on paper (section 3).
    if ((s->phase == PH_WIN || s->phase == PH_WIPE) && s->winCount == GRID) {
        int a = 256;
        float frac = 1.0f;
        if (s->phase == PH_WIN) {
            frac = (float)(nowMs - s->phaseStartMs) / (float)STRIKE_MS;
            if (frac > 1.0f) frac = 1.0f;
        } else {
            // Un-drawn ahead of the bar, the same gesture reversed: see
            // WIPE_STRIKE_MS.
            float u = (float)(nowMs - s->phaseStartMs) / (float)WIPE_STRIKE_MS;
            frac = 1.0f - u;
            if (frac <= 0.0f) a = 0;
        }
        if (a > 0) {
            int c0 = s->winCells[0], c2 = s->winCells[GRID - 1];
            float x0 = cell_cx(c0 % GRID), y0 = cell_cy(c0 / GRID);
            float x1 = cell_cx(c2 % GRID), y1 = cell_cy(c2 / GRID);
            float dx = x1 - x0, dy = y1 - y0;
            float len = sqrtf(dx * dx + dy * dy);
            if (len < 1e-3f) len = 1e-3f;
            float ux = dx / len, uy = dy / len;
            x0 -= ux * STRIKE_OVER; y0 -= uy * STRIKE_OVER;
            x1 += ux * STRIKE_OVER; y1 += uy * STRIKE_OVER;
            float ctrlX = (x0 + x1) * 0.5f - uy * STRIKE_BOW * 2.0f;
            float ctrlY = (y0 + y1) * 0.5f + ux * STRIKE_BOW * 2.0f;
            const int NP = 11;
            float xs[NP], ys[NP], rs[NP];
            for (int i = 0; i < NP; i++) {
                float t = (float)i / (float)(NP - 1), it = 1.0f - t;
                xs[i] = it * it * x0 + 2.0f * it * t * ctrlX + t * t * x1;
                ys[i] = it * it * y0 + 2.0f * it * t * ctrlY + t * t * y1;
                rs[i] = STRIKE_R_END + (STRIKE_R_MID - STRIKE_R_END) * sinf(t * 3.14159265f);
            }
            ink_path(xs, ys, rs, NP, frac, col_mark(s->winner), lx0, lx1, a);
        }
    }

    // 7. THE GHOST: the mark that would be made, in the candidate cell, at
    //    part alpha. Faint means not yet, solid means done. Not drawn on a
    //    cell that is already taken - the grey wash under it has already
    //    said so, and a ghost on top of a played mark would be two marks in
    //    one cell.
    if (cue >= 0 && s->phase == PH_PLAY && s->board[cue] == P_NONE) {
        draw_mark(s, s->turn, cell_cx(cue % GRID), cell_cy(cue / GRID), 1.0f, 1.0f,
                  col_mark(s->turn), lx0, lx1, GHOST_ALPHA);
    }

    // 8. THE TRAY: whose turn it is, outside the board, where a thumb on
    //    the board never goes (section 3). Absent while a mark is being
    //    drawn on, while the game is over and while the board is being
    //    wiped, and that ABSENCE is what says a touch means something else
    //    now (decision 0002: no modal state, and the screen is what says so).
    if (s->phase == PH_PLAY) {
        float cy = TRAY_CY;
        float scale = TRAY_SCALE;
        if (ho >= 0.0f) {
            // The hand-off's pop: it does not appear, it ARRIVES.
            float u = ho / HANDOFF_POP_FRAC;
            scale *= ease_out_back(u > 1.0f ? 1.0f : u);
        } else if (!s->armed) {
            // The idle invitation. It floats UP from its resting place and
            // settles back, never below: a cue that rises reads as offering
            // itself.
            float phase = fmodf((float)nowMs, BOB_MS) / BOB_MS * 6.2831853f;
            cy -= BOB_AMP * 0.5f * (1.0f - cosf(phase));
        }
        draw_mark(s, s->turn, TRAY_CX, cy, scale, 1.0f, col_mark(s->turn), lx0, lx1, 256);
    }

    // 9. the wipe's own bar, over everything: a soft pale stadium the
    //    height of the board, which is what makes the marks behind it read
    //    as WIPED rather than as faded out on their own.
    if (s->phase == PH_WIPE) {
        fill_rrect(wipe_front(s, nowMs), SLAB_CY, WIPE_FRONT_HALF, BAND_LONG,
                   WIPE_FRONT_HALF, PX_WHITE, lx0, lx1, 210);
    }
}

/* ---------------------------------------------------------------------
 * Dirty tracking and the push. Every push is a full-height strip, so its
 * row length after gfx's rotation is LAND_H = 368, a multiple of 8, always
 * - see section 5 of this file's header comment.
 *
 * Strip 0 is the tray, strips 1..3 are the three board columns. They tile
 * [0, LAND_W) with no gap, so a strip is always a legal window and the
 * union of every strip is the whole panel.
 * ------------------------------------------------------------------- */
#define STRIPS (1 + GRID)

static void mark(morpion_state_t *s, int strip) {
    if (strip < 0 || strip >= STRIPS) return;
    s->dirty |= (uint8_t)(1u << strip);
}

static void mark_cell(morpion_state_t *s, int cell) {
    if (cell < 0) return;
    mark(s, 1 + cell % GRID);
}

// The row band spans all three board columns, so anything that moves it
// dirties all of them. This is the one place this app costs more per frame
// than Connect Four did, and it is what the second band buys.
static void mark_board(morpion_state_t *s) {
    for (int c = 0; c < GRID; c++) mark(s, 1 + c);
}

static void mark_all(morpion_state_t *s) { s->dirtyAll = true; }

static void span_for_strip(int i, int *lx0, int *lx1) {
    if (i == 0) { *lx0 = 0; *lx1 = SLAB_X0; return; }
    int c = i - 1;
    *lx0 = (c == 0) ? SLAB_X0 : (BOARD_X0 + c * CELL);
    *lx1 = (c == GRID - 1) ? LAND_W : (BOARD_X0 + (c + 1) * CELL);
}

static void flush(morpion_state_t *s, uint32_t nowMs) {
    if (s->dirtyAll) {
        render_span(s, nowMs, 0, LAND_W);
        gfx_push_land(0, 0, LAND_W, LAND_H);
        s->dirtyAll = false;
        s->dirty = 0;
        return;
    }
    if (s->dirty == 0) return;

    // Coalesce runs of adjacent dirty strips: a run is one render, one push.
    int i = 0;
    while (i < STRIPS) {
        if ((s->dirty & (1u << i)) == 0) { i++; continue; }
        int start = i;
        while (i < STRIPS && (s->dirty & (1u << i)) != 0) i++;
        int a0, a1, b0, b1;
        span_for_strip(start, &a0, &a1);
        span_for_strip(i - 1, &b0, &b1);
        (void)a1; (void)b0;
        render_span(s, nowMs, a0, b1);
        gfx_push_land(a0, 0, b1 - a0, LAND_H);
    }
    s->dirty = 0;
}

/* =====================================================================
 * The game itself.
 * ================================================================== */
static void start_ink(morpion_state_t *s, uint32_t nowMs, int cell, uint8_t player) {
    s->phase = PH_INK;
    s->phaseStartMs = nowMs;
    s->inkCell = cell;
    s->inkPlayer = player;
    s->board[cell] = player;     // committed the instant it starts being drawn:
                                  // the ink IS the move, not a preview of one
    mark(s, 0);                   // the tray mark goes: it is on the board now
    mark_cell(s, cell);
    printf("morpion: mark cell=%d col=%d row=%d player=%d\r\n",
           cell, cell % GRID, cell / GRID, (int)player);
}

// Hands the board to `player`: starts their hand-off (the announcement plus
// the lock-out, section 3) and abandons whatever gesture was in progress.
static void hand_over_to(morpion_state_t *s, uint32_t nowMs, uint8_t player) {
    s->turn = player;
    s->turnStartMs = nowMs;
    s->handoffLive = true;
    s->phase = PH_PLAY;
    s->phaseStartMs = nowMs;
    s->hoverCell = -1;
    s->playCell = -1;
    s->contactSeen = false;
    s->armed = false;
    s->contactCount = 0;
    mark_all(s);
}

static void reset_game(morpion_state_t *s, uint32_t nowMs) {
    for (int i = 0; i < GRID * GRID; i++) s->board[i] = P_NONE;
    s->winCount = 0;
    s->winner = P_NONE;
    s->inkCell = -1;
    // X starts every game rather than the loser or the alternate side. Both
    // players are hands, so there is no fairness argument either way, and a
    // fixed starter is one less thing on a screen that may not use words to
    // explain itself: the board comes up red, and it always comes up red.
    hand_over_to(s, nowMs, P_X);
    printf("morpion: new game\r\n");
}

// A mark has finished being drawn: decide what happens next.
static void mark_done(morpion_state_t *s, uint32_t nowMs) {
    int cell = s->inkCell;
    s->winCount = win_line(s, cell, s->inkPlayer, s->winCells);
    if (s->winCount > 0) {
        s->winner = s->inkPlayer;
        s->phase = PH_WIN;
        s->phaseStartMs = nowMs;
        mark_all(s);
        printf("morpion: win player=%d\r\n", (int)s->winner);
        return;
    }
    if (board_full(s)) {
        s->phase = PH_DRAWPAUSE;
        s->phaseStartMs = nowMs;
        mark_all(s);
        printf("morpion: draw\r\n");
        return;
    }
    // Nobody has won and there is room left: the puck goes to the other
    // player. This is the ONLY place a turn changes, and it is the only
    // thing that happens after a mark lands - there is no reply to compute
    // and nothing to schedule.
    hand_over_to(s, nowMs, other_player(s->inkPlayer));
}

static void start_wipe(morpion_state_t *s, uint32_t nowMs) {
    s->phase = PH_WIPE;
    s->phaseStartMs = nowMs;
    s->contactSeen = false;
    s->armed = false;
    s->hoverCell = -1;
    s->playCell = -1;
    mark_all(s);
}

/* ---------------------------------------------------------------------
 * The gesture, whoever's it is. Identical for both players - one idiom,
 * learned once, and neither side gets a different one. See section 2 of
 * this file's header for why none of this trusts f->touchReleased.
 * ------------------------------------------------------------------- */
static void gesture_tick(morpion_state_t *s, const app_frame_t *f) {
    if (!gesture_live(s, f->nowMs)) {
        // Any gesture in progress is abandoned rather than carried across a
        // phase boundary or a hand-off: a finger still down when the next
        // turn opens starts a FRESH gesture and has to arm again. That is
        // most of what stops the hand which just played from playing the
        // other player's move too - the hand-off's lock-out buys the beat,
        // and this makes sure the beat is not spent quietly accumulating
        // contact samples toward an arm the instant it ends.
        if (s->contactSeen || s->armed) {
            bool wasArmed = s->armed;
            s->contactSeen = false;
            s->armed = false;
            s->contactCount = 0;
            s->hoverCell = -1;
            s->playCell = -1;
            if (wasArmed) mark_board(s);
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
            mark(s, 0);   // the bobbing tray mark stops
        }

        if (s->armed) {
            // Touch arrives in PANEL coordinates; gfx.h documents only the
            // landscape-to-panel direction, so the inverse is algebra on
            // that same mapping - the same inversion menu.c's
            // panel_to_land() and four.c's gesture do. BOTH axes matter
            // here, unlike Connect Four where only the column did.
            int lx = f->touchY;
            int ly = PANEL_W - 1 - f->touchX;
            int cell = cell_from_land(lx, ly);
            if (cell != s->hoverCell) {
                int old = s->hoverCell;
                if (old < 0 || (old / GRID) != (cell / GRID)) {
                    mark_board(s);          // the row band moved, or arrived
                } else {
                    mark_cell(s, old);      // it slid along one row
                    mark_cell(s, cell);
                }
                s->hoverCell = cell;
                s->hoverSinceMs = f->nowMs;
                if (old < 0) s->playCell = cell;   // the gesture's first cell:
                                                     // a tap has to play something,
                                                     // and a wander here is corrected
                                                     // by the settle below within
                                                     // SETTLE_MS of the truth coming
                                                     // back
                printf("morpion: hover cell=%d c=%d r=%d\r\n", cell, cell % GRID, cell / GRID);
            } else if ((f->nowMs - s->hoverSinceMs) >= (uint32_t)SETTLE_MS && s->playCell != cell) {
                // The thumb has genuinely stayed here, so this is now the
                // cell a release plays. Updated ONLY on a contact sample:
                // doing it on the clock alone would let the release grace's
                // own 300ms of silence settle a cell the controller named
                // once and never confirmed, which is the whole failure this
                // exists to prevent (see SETTLE_MS).
                s->playCell = cell;
            }
        }
        return;
    }

    // No contact this tick. That is almost always a dropout, not a lift.
    if (!s->contactSeen) return;
    if ((f->nowMs - s->lastContactMs) < RELEASE_GRACE_MS) return;

    bool wasArmed = s->armed;
    // The SETTLED cell, not the last one the controller happened to name.
    // See SETTLE_MS: the two differ exactly when the controller's reported
    // position wandered in the last fraction of a second of contact, which
    // it measurably does.
    int cell = s->playCell;
    s->contactSeen = false;
    s->armed = false;
    s->contactCount = 0;
    s->hoverCell = -1;
    s->playCell = -1;

    if (!wasArmed || cell < 0) return;

    if (s->board[cell] != P_NONE) {
        // A taken cell: the release does nothing at all, which is what the
        // grey wash has been saying the whole time the thumb was over it.
        // The turn does not change.
        mark_board(s);
        mark(s, 0);
        printf("morpion: release over a taken cell=%d, ignored\r\n", cell);
        return;
    }
    start_ink(s, f->nowMs, cell, s->turn);
}

/* =====================================================================
 * enter / tick.
 * ================================================================== */
static void morpion_enter(void) {
    s_state = APP_STATE(morpion_state_t);
    morpion_state_t *s = s_state;

    build_o(&s->oPath);
    build_x_arm(&s->xPath[0], -MARK_ARM, -MARK_ARM,  MARK_ARM,  MARK_ARM,  MARK_BOW);
    build_x_arm(&s->xPath[1],  MARK_ARM, -MARK_ARM, -MARK_ARM,  MARK_ARM, -MARK_BOW);

    // Nothing in this app decides anything, so nothing has to be
    // unpredictable and there is no generator to seed. That is also what
    // makes a scripted game in the emulator exactly reproducible, which the
    // tests lean on.
    reset_game(s, 0);
    s->dirtyAll = false;         // enter() must not push: the runtime pushes
    s->dirty = 0;                // the whole panel once after it returns
    render_span(s, 0, 0, LAND_W);

    printf("morpion: state=%d bytes (arena %d)\r\n",
           (int)sizeof(morpion_state_t), (int)APP_ARENA_BYTES);
}

static void morpion_tick(const app_frame_t *f) {
    morpion_state_t *s = s_state;
    uint32_t now = f->nowMs;

    // BOOT abandons the game and wipes the board, the same "one physical
    // button, one obvious job" chrono gives it. It is an adult's escape
    // hatch (and the emulator's), not something she needs.
    if (f->bootClicked && s->phase != PH_WIPE) {
        start_wipe(s, now);
    }

    gesture_tick(s, f);

    switch (s->phase) {
    case PH_PLAY:
        if (handoff_u(s, now) >= 0.0f) {
            // The hand-off's own animation: the cross fading out and the
            // tray mark popping in, at the full animation rate because this
            // one is short and is the moment that has to be seen. The cross
            // spans the whole board, so this is three strips plus the tray.
            if ((now - s->lastRenderMs) >= RENDER_MIN_MS) { mark_board(s); mark(s, 0); }
        } else if (s->handoffLive) {
            // THE FRAME THE HAND-OFF ENDS ON IS REPAINTED EXPLICITLY, and
            // that is not belt and braces, it is a bug this file shipped
            // and feature-morpion.ts caught on its first run. The board
            // strips are otherwise only marked when something on them
            // changes, and the idle bob marks the TRAY, so a hand-off whose
            // last animated frame never arrived left its announcing cross
            // painted on the board with nothing scheduled to clear it. The
            // way to reach that state is not exotic: enter() renders at
            // t=0, when the hand-off it just started is at u=0 and the
            // cross is at full strength, and the first real tick can be a
            // second later. The app opened onto a board with a bright red
            // cross on it that never went away.
            //
            // Connect Four does not have this bug and the difference is
            // instructive rather than luck: its hand-off cue and its idle
            // bob live in the SAME column, so the bob's own repaint
            // happened to clean up after the announcement. Splitting the
            // turn cue out of the board (the tray) removed that coincidence
            // along with the problem it was hiding.
            s->handoffLive = false;
            mark_board(s);
            mark(s, 0);
        } else if (!s->armed && (now - s->lastRenderMs) >= BOB_STEP_MS) {
            // The idle bob, on its own slower clock and in the cheapest
            // strip on the screen. A 5px travel over 1.3s moves under half a
            // pixel per 60fps frame, so repainting at the animation rate
            // would push sixty near-identical frames a second for a change
            // nobody can see.
            mark(s, 0);
        }
        break;

    case PH_INK:
        if ((now - s->phaseStartMs) >= (uint32_t)INK_MS) {
            mark_cell(s, s->inkCell);   // the finished mark replaces the
                                          // partial one in the same tick
            mark_done(s, now);
            break;
        }
        if ((now - s->lastRenderMs) >= RENDER_MIN_MS) mark_cell(s, s->inkCell);
        break;

    case PH_WIN:
        if ((now - s->phaseStartMs) >= WIN_MAX_MS ||
            (f->touchDown && (now - s->phaseStartMs) >= WIN_SKIP_MS)) {
            start_wipe(s, now);
            break;
        }
        if ((now - s->lastRenderMs) >= WIDE_RENDER_MIN_MS) {
            // The strike and the halos both reach past their own cells, but
            // never past the strip that cell is repainted through (see
            // STRIKE_OVER and HALO_R_MAX), so marking the winning cells'
            // own columns covers every pixel either of them can touch.
            for (int k = 0; k < s->winCount; k++) mark_cell(s, s->winCells[k]);
        }
        break;

    case PH_DRAWPAUSE:
        if ((now - s->phaseStartMs) >= DRAW_PAUSE_MS) start_wipe(s, now);
        break;

    case PH_WIPE: {
        if ((now - s->phaseStartMs) >= (uint32_t)WIPE_MS) {
            reset_game(s, now);
            break;
        }
        if ((now - s->lastRenderMs) >= WIDE_RENDER_MIN_MS) {
            if ((now - s->phaseStartMs) < (uint32_t)WIPE_STRIKE_MS && s->winCount > 0) {
                // The strike is lifting off and it spans the board.
                mark_board(s);
            } else {
                // Only the strips the bar and its fade tail are actually
                // over. A mark that has finished fading does not change
                // again, so a strip the front has left behind needs no
                // further repaint - the same reasoning that keeps four.c's
                // drain at two or three strips a frame instead of seven.
                float front = wipe_front(s, now);
                float lo = front - WIPE_FADE - 40.0f;
                float hi = front + WIPE_FRONT_HALF + 4.0f;
                for (int i = 0; i < STRIPS; i++) {
                    int a0, a1;
                    span_for_strip(i, &a0, &a1);
                    if ((float)a1 >= lo && (float)a0 <= hi) mark(s, i);
                }
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
// away a game in progress on a jolt. The board is cleared by finishing it,
// or by BOOT.
const app_t g_morpionApp = {
    .name       = "morpion",
    .enter      = morpion_enter,
    .tick       = morpion_tick,
    .leave      = NULL,
    .landscape  = true,
    .wantsShake = false,
};
