/*
 * tables: multiplication-tables practice, the 6/7/8 tables mixed, for a
 * niece who reads digits fluently and is drilling them.
 *
 * WHICH HOUSE CONVENTIONS THIS APP KEEPS, AND WHICH IT DROPS - read this
 * before treating this file as a precedent for any other app.
 *
 * Every other app on this device is built for a two-year-old who cannot
 * read: no text, no digits, pictures only (see AGENTS.md's header and
 * decision 0013's "she cannot read a page number"). This app is for a
 * different child, one who is LEARNING her times tables, so digits are not
 * a barrier here, they are the entire point. Kept:
 *
 *   - decision 0001, the 8-pixel push rule (gfx enforces it regardless);
 *   - decision 0009, nothing drawn with a ruler where a curve or a round
 *     cap can carry it instead - every icon in this file (the checkmark,
 *     the backspace arrow, the multiply and equals marks, the two counter
 *     glyphs, the numpad digits themselves) is built from shapes.h's float
 *     primitives (capsule/disc/annulus), reached through this file's own
 *     portrait wrappers (see PORTRAIT DRAWING HELPERS below) - the same
 *     brush every other app's ink uses, never a filled rectangle standing
 *     in for a shape. The two counter PILLS (see THE COUNTER PILLS below)
 *     are the one flat-colour exception, and decision 0009 already accepts
 *     it: they are drawn with shapes.h's older, non-anti-aliased
 *     "generation 1" technique (shapes_fill_half_width_table plus a row of
 *     gfx_fill_rect calls), the exact machinery timer.c's own progress-ring
 *     TRACK already uses for a flat colour fill - the AA primitives cannot
 *     carry colour at all (they composite by converting every colour to a
 *     grey level, see shapes.c's aa_composite_land), which is also why
 *     every icon and digit drawn ON TOP of a pill still goes through the AA
 *     float brush unchanged, in black, the same ink-on-a-tinted-background
 *     trick this file's answer band already used before pills existed (see
 *     RIGHT/WRONG COLOUR below);
 *   - the press-drag-release idiom (menu.c, four.c, morpion.c) and its
 *     arm/release-grace filtering against the FT3168's measured jitter -
 *     see "THE GESTURE" below for what is reused verbatim and what is
 *     re-derived;
 *   - PANEL_BEZEL_MARGIN_PX: every element in this layout is inset from
 *     all four physical panel edges, same as everywhere else.
 *
 * Dropped, deliberately, and only here:
 *
 *   - "no text, pictures only". The question, the answer she types and the
 *     two counters are digits on purpose - she reads them, and a picture
 *     of "6" is worse than the numeral 6 for a child drilling numerals.
 *     The two counters (wrong/right) are told apart by an EXPLICIT glyph -
 *     a cross, a checkmark - rather than by a word or an arbitrary shape
 *     she would have to learn to decode first: the owner's own exact
 *     mockup (2026-08-17) replaced an earlier ring/disc pairing that had
 *     no inherent connection to what it counted (a bare ring does not mean
 *     "attempted" to anyone who has not been told) with marks that already
 *     mean what they mean everywhere else - see COUNTER ICONS below.
 *   - decision 0002 section 4b's "no chrome inside an app" is not violated
 *     (nothing here is navigation chrome), but this app is the first to
 *     put a live floating overlay - the loupe - on screen, which no other
 *     app needed before it had a small-target input to correct.
 *
 * ==========================================================================
 * THE INPUT DESIGN, and why it changed mid-build.
 *
 * The obvious first design was a numpad sized the way menu.c's grid is: a
 * child's fingertip contact measures ~75-100px on this panel (AGENTS.md),
 * so twelve keys (0-9, backspace, submit) at a size a thumb can hit
 * reliably would have consumed nearly the whole 368x448 glass, leaving
 * little room for the question or the counters, and would still have
 * inherited the FT3168's measured 80-250px centroid jitter on every
 * boundary between two adjacent keys - the worst case named in AGENTS.md's
 * own numpad warning.
 *
 * The owner proposed the actual fix, in his own words: "un pavé numérique
 * a la clavier iPhone ou elle peut rester enfoncee sur son doigt pour
 * selectionner et ca fait une loupe" - an iOS-style numpad where holding a
 * finger down opens a magnifier showing which key is under it, she slides
 * to correct without lifting, and it commits on release. This dissolves
 * the arithmetic problem rather than solving it: a key no longer has to be
 * bigger than a fingertip, because she is not required to land on one
 * precisely. She lands roughly, reads the loupe, slides if it is wrong,
 * lifts when it is right. That is what freed the numpad keys down to a
 * fraction of a fingertip (see THE LOUPE and THE NUMPAD below) - small
 * enough to leave real room for a full-width question band above the pad
 * and the counters below it, legible rather than finger-sized, because
 * legibility is what a shrunk target can now be judged on instead of
 * hit-rate. See this app's own report for what that comes out to against a
 * child's fingertip.
 *
 * It is also this device's own idiom rather than a thirteenth new gesture:
 * press-drag-release, with the candidate lit under the thumb before
 * anything commits, is exactly what the palette, Connect Four's column aim
 * and the menu already do. The loupe is that same gesture with a
 * magnifier drawn on top, because a numpad is the one place on this device
 * where "which target is my thumb over" cannot be answered by lighting up
 * a whole column and a whole row the way morpion's cross does (twelve
 * targets in a tight grid, not one cell on a 3x3 board) - the child needs
 * to see the actual character, not just a highlighted region.
 *
 * TWO DETAILS THAT DECIDE WHETHER IT FEELS RIGHT, both from the owner's
 * own framing of the iOS keyboard:
 *
 *   - the loupe must sit above the finger, offset, never under it, or it
 *     shows her nothing. iOS solves this by floating the bubble above the
 *     touch point, which breaks down at the top of a keyboard where there
 *     is no room left above. THIS FILE'S ANSWER: the loupe's height is NOT
 *     dynamic at all. It sits in a fixed-height band reserved above the
 *     numpad (LOUPE_ZONE_H, see THE LOUPE below) and only its horizontal
 *     position tracks the finger. That is a stronger fix than "give the
 *     top row extra headroom": it removes the top-row special case
 *     entirely, because the loupe is never vertically near ANY row it
 *     could run out of room above, and it collapses the redraw problem
 *     from an arbitrary floating rectangle (which could overlap numpad
 *     keys anywhere on the pad, the same erase/redraw-neighbours cost
 *     menu.c's halo comment describes) down to one fixed-height horizontal
 *     band that never overlaps a key at all, because nothing but the
 *     loupe is ever drawn there. See THE LOUPE's own comment for the
 *     redraw cost this buys, and loupe_update()'s own comment for the
 *     flicker this design still let through until it was found and fixed.
 *   - releasing outside the pad must cancel, not commit - the way she
 *     corrects a wrong press without a delete key, and the same rule the
 *     menu already enforces by name for its own cancel band ("nothing
 *     hidden", decision 0013). See THE GESTURE below for how the numpad's
 *     own cancel region is defined.
 *
 * WHAT COMMITS IS WHAT THE LOUPE SHOWED. Same invariant menu.c states for
 * its own halo ("what launches is always what was lit"), carried over
 * unchanged: the release verdict reads the identical `hoverCell` the loupe
 * was just drawn from, so the device can never disagree with her about
 * which key she was looking at when she let go.
 * ==========================================================================
 */
#include <math.h>
#include <stdio.h>

#include "app.h"
#include "digits.h"
#include "gfx.h"
#include "sensors.h"
#include "shapes.h"
#include "storage.h"

/* =========================================================================
 * LAYOUT. Portrait 368x448 - the panel's own native space, unrotated.
 * Everything below is inset from all four physical panel edges by
 * PANEL_BEZEL_MARGIN_PX (10px): in portrait space, same as landscape, every
 * one of the four edges (x=0, x=PANEL_W, y=0, y=PANEL_H) is a real panel
 * edge the case hides part of. The usable canvas is therefore 348x428.
 *
 * THE OWNER'S THIRD DRAWING, AND THE ORIENTATION CORRECTION BEHIND IT. His
 * first reMarkable sketch (2026-08-17) settled the broad shape - a
 * full-width question band across the top, the numpad lower-left, the
 * counters lower-right - and his exact mockup the same day settled the
 * details a sketch cannot (see git history for that layout: a full-width
 * rule under the question, the counters in a drawn box, both landscape). A
 * THIRD drawing, later, was held PORTRAIT and redrew the arrangement again:
 * the question across the top, the numpad below it, and two counter pills
 * at the very bottom, side by side. Which physical grip that drawing meant
 * took two passes to settle, and the second reversed the first:
 *
 *   - First relayed as "stay in landscape, just shuffle the bands" - the
 *     owner's own words at the time, "tu peux juste shuffle les trucs pour
 *     avoir le mode paysage" - which is why an earlier version of this file
 *     kept a landscape rotation and widened the pad inside the 448x368
 *     landscape canvas instead of drawing portrait at all.
 *   - Corrected once the owner specified the physical grip precisely: PWR
 *     upper, BOOT lower, both on the panel's own right edge. Checked
 *     against the code rather than taken on trust (the relayed reading
 *     contradicted AGENTS.md's own documented grip, so it needed settling
 *     before anything was built to it): the emulator's own "TOP" turn
 *     setting (data-deg="0" in emulator/src/index.html) is the panel's
 *     native, UNROTATED orientation, and emu_shim.c's own button
 *     descriptor - `{"id":"boot",...,"edge":"right","at":0.38}` before
 *     `{"id":"pwr",...,"edge":"right","at":0.62}`, both fractions measured
 *     top-down along that edge - puts BOOT above PWR there, i.e. BOOT
 *     upper, PWR lower. The owner's OWN choice, once that contradiction was
 *     put to him with both readings spelled out, was BOOT upper/PWR lower:
 *     the SAME grip AGENTS.md already documents ("screen facing you,
 *     buttons on the right: PWR lower, BOOT upper"), not a 180-degree flip
 *     of it. So this app is genuinely, plainly portrait: `landscape =
 *     false` in g_tablesApp below, drawn in the panel's own native
 *     x-right/y-down space, no rotation of any kind - not a landscape app
 *     shuffled to look portrait, and not a point-reflected grip either.
 *
 * Four bands, stacked top to bottom in this single portrait column: the
 * question band across the very top (the blank at the right end of that
 * line, carrying a blinking caret - see THE BLINKING CURSOR below), the
 * loupe's own reserved zone under it, the numpad below that, and the two
 * counter pills at the very bottom - see THE NUMPAD and THE COUNTER PILLS
 * below for the arithmetic that fits all four into 428px of usable height,
 * and this app's own report for the resulting key size checked against a
 * child's fingertip rather than assumed.
 * ========================================================================= */
#define OX PANEL_BEZEL_MARGIN_PX
#define OY PANEL_BEZEL_MARGIN_PX
#define USABLE_W (PANEL_W - 2 * PANEL_BEZEL_MARGIN_PX) // 348
#define USABLE_H (PANEL_H - 2 * PANEL_BEZEL_MARGIN_PX) // 428

/* =========================================================================
 * PORTRAIT DRAWING HELPERS.
 *
 * This app draws in PANEL space directly (portrait, native, unrotated -
 * see LAYOUT above), but shapes.h's anti-aliased primitives
 * (shapes_fill_capsule_aa_land, shapes_fill_disc_aa_land) only ever take
 * LANDSCAPE coordinates: they are the one deliberate exception to "gfx
 * owns rotation" (shapes.c's own header comment), mapping each pixel
 * through gfx.h's landscape<->panel relation themselves, so there is no
 * portrait-space sibling to call instead. digits.c already solved exactly
 * this problem for DIGITS_PORTRAIT (its own soft_capsule/soft_dot
 * helpers): a capsule is two points and a radius, and gfx.h's
 * landscape/panel relation is an isometry (it preserves distances and
 * angles), so mapping the two endpoints through the inverse relation -
 * portrait (x,y) -> landscape (y, PANEL_W - x) - before calling the _land
 * primitive is the ENTIRE conversion; there is no second rasteriser to
 * write. These two wrappers are exactly that inverse, lifted from
 * digits.c's own soft_capsule/soft_dot rather than re-derived, so both
 * files agree on the one mapping rather than carrying two copies of it.
 */
static void shapes_fill_capsule_aa(float x0, float y0, float r0,
                                    float x1, float y1, float r1, uint16_t color) {
    float edge = (float)PANEL_W;
    shapes_fill_capsule_aa_land(y0, edge - x0, r0, y1, edge - x1, r1, color);
}
static void shapes_fill_disc_aa(float cx, float cy, float r, uint16_t color) {
    shapes_fill_disc_aa_land(cy, (float)PANEL_W - cx, r, color);
}
// Same portrait<->landscape isometry as the two wrappers above, for the
// calibration progress dots' own hollow ring (see NUMPAD TOUCH CALIBRATION
// below numpad_hit(), tables_calib_draw_progress()) - the only caller
// today, but grouped here with its two siblings rather than beside that
// caller, per this block's own header.
static void shapes_fill_annulus_aa(float cx, float cy, float rOuter, float rInner, uint16_t color) {
    shapes_fill_annulus_aa_land(cy, (float)PANEL_W - cx, rOuter, rInner, color);
}

// gfx_push() takes an INCLUSIVE panel rectangle (minX,minY,maxX,maxY); every
// call site in this file thinks in (x,y,w,h), the same shape gfx_push_land()
// used to take, so this converts once, here, rather than repeating
// `x+w-1, y+h-1` arithmetic at a dozen call sites - exactly the kind of
// off-by-one gfx.h's own header comment warns an inclusive form invites,
// and there IS a live, unfixed hardware bug elsewhere in this tree where a
// pushed rectangle failed to cover its own right edge and left vertical
// streaks on the real panel (a different defect, in gfx.c's own push path,
// not this file - but the same CLASS of mistake), which is exactly the
// reason to get this conversion right in one place instead of several.
static void gfx_push_wh(int x, int y, int w, int h) {
    gfx_push(x, y, x + w - 1, y + h - 1);
}

// A plain round-half-up to the nearest int. Used wherever a float bias -
// which, since NUMPAD TOUCH CALIBRATION below, may be a fitted line's
// value rather than just the old flat TOUCH_THUMB_BIAS_Y constant - has to
// become an integer pixel offset (numpad_hit() below). floorf is already
// an available math function on both targets (emu_abi.h's import list /
// pico-sdk's libm), so this needs nothing this file does not already have.
static int32_t tables_round_i32(float v) { return (int32_t)floorf(v + 0.5f); }

/* ---- THE QUESTION BAND ---------------------------------------------------
 *
 * The full-width row across the top. QROW_H is chosen so that, together
 * with LOUPE_ZONE_H, the numpad's own four rows and the counter pills'
 * band below it, all FOUR stacked pieces sum to exactly USABLE_H - see THE
 * NUMPAD and THE COUNTER PILLS below for that arithmetic.
 */
#define QROW_Y0 OY
// 64: 10px of margin above the equation (QDIGIT_H=44, centred at QROW_CY)
// and real clearance below the underline (Q_UNDERLINE_Y, THE QUESTION
// BAND'S OWN LAYOUT below) before the loupe zone starts - the same margins
// the very first version of this band shipped with. Nothing shares this
// row's width any more (no rule below it, no column beside it), so QROW_H
// is sized purely by what it has to contain, not by anything next to it.
#define QROW_H  64
#define QROW_CY (OY + QROW_H / 2) // 42

/* ---- THE NUMPAD ---------------------------------------------------------
 *
 * 3 columns x 4 rows, phone-dial order (1 2 3 / 4 5 6 / 7 8 9 / back 0
 * check), 12 cells, centred under the question band - with the loupe still
 * doing the precision work (see this file's header), a cell only has to be
 * roughly aimable and legibly labelled, not fingertip-sized. See this
 * app's own report for CELL_W x CELL_H checked against a child fingertip
 * rather than assumed.
 *
 * CELL_W IS NOT "however much width is left over". Stretching the pad to
 * nearly the full USABLE_W (tried at CELL_W=140 during this app's
 * landscape detour) makes a cell a 3.7:1 sliver - each of the three digit
 * columns reads as its own thin, isolated vertical strip with a wide gap
 * of dead white space to its neighbour, not a grid a child's eye
 * assembles into a keypad at all (found on a rendered preview, not by
 * inspection). CELL_W=100 keeps a cell closer to a real numpad key's own
 * proportions (1.75:1 against CELL_H=57) and leaves real margin on both
 * sides instead, which reads as "a pad, centred", not "digits, stranded".
 * The counter pills below are NOT tied to this width - see THE COUNTER
 * PILLS below for why they span close to the full USABLE_W on their own
 * regardless of how wide the pad ends up.
 *
 * QROW_H + LOUPE_ZONE_H + NUMPAD_H + the counter pills' own band height
 * must sum to exactly USABLE_H (428): 64 + 80 + 228 + 56 = 428 (see THE
 * COUNTER PILLS below for the last term). LOUPE_ZONE_H is untouched from
 * this app's very first version - the loupe itself is preserved exactly,
 * never resized to make room for anything else, per the owner's own word
 * that its behaviour is settled and not to be touched. Portrait's own
 * 428px of usable height is markedly more generous than the 348px this
 * app had to fit the same four bands into during its landscape detour,
 * which is why CELL_H (57) comes out taller here than any earlier version
 * of this pad managed - see this app's own report for the exact
 * comparison against a child's fingertip.
 */
#define NUMPAD_COLS 3
#define NUMPAD_ROWS 4
#define CELL_W 100
#define CELL_H 57
#define NUMPAD_W (CELL_W * NUMPAD_COLS)             // 300
#define NUMPAD_H (CELL_H * NUMPAD_ROWS)             // 228
#define LOUPE_ZONE_H 80                              // = LOUPE_BOX_H exactly, see THE LOUPE below - unchanged since this app's first version
#define NUMPAD_X0 (OX + (USABLE_W - NUMPAD_W) / 2)   // 34 - centred, real margin on each side
#define NUMPAD_Y0 (OY + QROW_H + LOUPE_ZONE_H)       // 154

#define CELL_BACK  9
#define CELL_ZERO  10
#define CELL_CHECK 11
// Cells 0..8 are digits 1..9 (cell i -> digit i+1); CELL_ZERO is digit 0.

static bool cell_is_digit(int cell) { return (cell >= 0 && cell <= 8) || cell == CELL_ZERO; }
static int  cell_digit_value(int cell) { return cell == CELL_ZERO ? 0 : cell + 1; }
// The inverse of cell_digit_value() - which CELL a given digit (0..9) draws
// as. Needed by NUMPAD TOUCH CALIBRATION (below numpad_hit()) to turn a
// PROMPTED DIGIT back into the cell_rect() it should measure against; no
// caller needed this direction before calibration moved onto the numpad.
static int digit_to_cell(int digit) { return digit == 0 ? CELL_ZERO : digit - 1; }

static void cell_rect(int cell, int *bx, int *by, int *bw, int *bh) {
    // The zero owns the whole bottom row, so its rect is three cells wide -
    // see numpad_hit(). Everything downstream (the hover disc, the glyph, the
    // push) reads this one function, so the wide key is one place, not four.
    if (cell == CELL_ZERO) {
        *bx = NUMPAD_X0;
        *by = NUMPAD_Y0 + (NUMPAD_ROWS - 1) * CELL_H;
        *bw = NUMPAD_W;
        *bh = CELL_H;
        return;
    }
    int row = cell / NUMPAD_COLS, col = cell % NUMPAD_COLS;
    *bx = NUMPAD_X0 + col * CELL_W;
    *by = NUMPAD_Y0 + row * CELL_H;
    *bw = CELL_W;
    *bh = CELL_H;
}

// -1 for "not over any cell" (the loupe zone above, the counter pills
// below, or outside the panel entirely) - the numpad's own cancel region.
// A release verdict that reads -1 here commits nothing, the same rule
// menu.c's cancel band enforces for the app grid.
// THE THUMB LANDS BELOW WHERE IT AIMS, so the hit grid sits below the drawn
// one. The owner, after using it: "if i aim the 5 with my thumb the touchpoint
// will be slightly under the exact one. it should start a little at the top of
// 8." That is the ordinary geometry of a thumb - the contact patch's centroid
// sits behind and below the point of the thumb the eye is aiming with - and no
// amount of filtering fixes it, because the controller is reporting the
// contact honestly.
//
// So subtract the bias from the reported y before deciding which row was
// meant. In the reported coordinates every key's zone therefore moves DOWN by
// this much: the 5's zone reaches into the top of the 8's drawn cell, exactly
// as he describes, and the top row gains the same slack above it that the
// bottom row loses below.
//
// Applied ONLY to the row, not the column. The same physics does not push a
// thumb sideways: left and right of the intended point are symmetric, and
// biasing x would just make one column harder to hit than the other.
//
// The loupe is unaffected by construction: it draws whatever cell this
// function names, so what she sees magnified is what this bias chose, and
// "what commits is what the loupe showed" still holds.
//
// 22px, raised from 14 after use. The owner: "tu peux encore augmenter le gap
// du numpad entre le touch et la key, je me rends compte que parfois je vise
// encore plus bas que prevu." So the first value was in the right direction
// and short of it - which is the expected shape for this number, since nobody
// can introspect where their own thumb lands and only trying it says.
//
// 22 is about 40% of CELL_H and 1.7mm of glass at this panel's ~322ppi. That
// is a lot of a key, and it is the reason the LOUPE matters: she sees the
// magnified digit before committing, so a bias that guesses wrong is visible
// and correctable in the same gesture rather than silently wrong.
//
// THE OWNER ASKED FOR EXACTLY THE CEILING THIS PARAGRAPH USED TO WARN ABOUT:
// "limite ça devrait être 50% du chiffre d'en dessous" - pressing the top half
// of the key BELOW the one you want should still give you the one you want.
// That is a bias of half a key, CELL_H/2 = 28.5, so 29: at 28 a touch exactly
// halfway into the lower key still lands on the lower key, and half was the
// whole request.
//
// The objection this paragraph used to raise was real and is now ANSWERED IN
// CODE rather than by staying small: at a half-key bias the top row loses its
// own upper half off the pad, because numpad_hit() gated on the BIASED y and a
// touch on the top row's drawn upper half biased its way clean off the pad and
// was rejected. So the gate now reads the RAW touch at the top edge and clamps
// into the first row (see numpad_hit()). The top row keeps every pixel it
// draws; the other three sit half a key below theirs, which is the ask.
// The remaining honest fix, if half a key still reads low, is taller keys.
//
// THE LITERATURE, found after the owner had already moved this twice by
// feel: Holz and Baudisch, "Understanding Touch" (CHI 2011). Users aim by
// placing a point on the TOP of the fingernail over the target, while the
// screen senses the contact patch on the underside - a parallax between
// where you aim and what gets sensed, not a filtering problem. The
// traditional contact-centroid model (what this controller reports, and
// what numpad_hit() below reads) leaves about 4mm of error; their
// projected-centre correction reduces it to 1.6mm. At this panel's
// ~322ppi (12.7 px/mm - AGENTS.md's "a finger is about 100 pixels wide"
// section has the same derivation), 4mm is about 51px, against a key
// (CELL_H) that is 57px tall: the offset this bias is standing in for is
// nearly a whole key, and this constant's own 22px is closer to the
// LITERATURE'S RESIDUAL (1.6mm ~= 20px) than to its full correction. See
// TOUCH_THUMB_BIAS_Y_DEFAULT's own tunable range below, which is sized off
// these numbers rather than off the half-key ceiling two paragraphs up.
// 40, not the half-key 29 the paragraph above derives: the owner tried a half
// key on the panel and asked for more ("et sur le numpad flash avec 40 pour
// thumbbias"). 40px is 3.1mm, which sits between the literature's residual
// (1.6mm) and its full correction (4mm ~= 51px), so this is inside the range
// Holz and Baudisch measured rather than past it. The zones now sit MORE than
// a key's half below their drawings: aiming at a key's own drawn centre gives
// the key ABOVE it, which is the point.
#define TOUCH_THUMB_BIAS_Y_DEFAULT 40.0f

// THIS IS NOW A FALLBACK, NOT THE LAST WORD. 40 was picked by feel, twice
// (this constant's own history above). NUMPAD TOUCH CALIBRATION, right
// after numpad_hit() below, replaces it with a MEASURED line once the owner
// runs that mode; TOUCH_THUMB_BIAS_Y_DEFAULT remains exactly what a
// never-calibrated puck falls back to - see tables_effective_bias() for the
// precedence between this constant, a stored calibration, and the
// TABLES_LIVE_TUNE bench slider just below.

/* ---- DEVELOPMENT: live tuning of the thumb bias (TABLES_LIVE_TUNE) ------
 *
 * Same mechanism as sketch.c's SKETCH_LIVE_TUNE and clock.c's
 * CLOCK_LIVE_TUNE (see either file's own comment for the full design, and
 * sensors.h's "DEVELOPMENT: live tuning" section for how all three join
 * one registry): the owner has already moved this constant twice by feel,
 * each time a source edit and a reflash, and the literature above says the
 * honest range to explore is much wider than the half-key ceiling this
 * file used to reason from.
 *
 * ONE TUNABLE: thumbbias, TOUCH_THUMB_BIAS_Y itself. Range 0..CELL_H
 * (0..57px): 0 is the raw, uncorrected reading (no bias at all, a useful
 * reference point to feel the difference against); 57 is one full key,
 * "nearly a whole key" in the literature's own terms above, and a ceiling
 * this file's own numpad_hit() enforces structurally (see that function:
 * a bias of a whole CELL_H would push the top row's zone off the pad's own
 * top edge). The DEFAULT does not change - that is the owner's call, made
 * by feel, same as it has been twice already - only how far the slider can
 * reach changes.
 */
#ifndef TABLES_LIVE_TUNE
#define TABLES_LIVE_TUNE 0
#endif

#if TABLES_LIVE_TUNE
static float g_tuneThumbBiasY = TOUCH_THUMB_BIAS_Y_DEFAULT;
#define TOUCH_THUMB_BIAS_Y g_tuneThumbBiasY

// WHETHER THE SLIDER HAS ACTUALLY BEEN TOUCHED THIS SESSION - see
// tables_effective_bias() (past tables_state_t, below) for why this exists
// at all: TABLES_LIVE_TUNE is compiled into EVERY emulator build (the
// devlink tuning registry serves sketch.c/clock.c/tables.c from the same
// binary), so "TABLES_LIVE_TUNE is on" cannot by itself mean "the owner is
// at this instant moving the slider" - it is true on every single run,
// including every automated test. Set the moment tables_tune_set() is
// actually called for THIS tunable (not merely because the build has the
// gate compiled in), cleared again on tables_enter() (a fresh session) -
// see tables_tune_reset_moved() below.
static bool g_tuneThumbBiasMoved = false;

typedef struct {
    const char *protoName;  // devlink/emulator-facing identifier, same
                             // convention as sketch.c's own sketch_tunable_t.
    const char *defineName;
    float *value;
    float min, max, def;
} tables_tunable_t;

static tables_tunable_t g_tablesTunables[] = {
    { "thumbbias", "TOUCH_THUMB_BIAS_Y", &g_tuneThumbBiasY, 0.0f, (float)CELL_H, TOUCH_THUMB_BIAS_Y_DEFAULT },
};
#define TABLES_TUNABLE_COUNT ((int)(sizeof(g_tablesTunables) / sizeof(g_tablesTunables[0])))

// A local strcmp-equivalent, same reasoning as sketch_tune_name_eq's own
// comment (sketch.c): this file compiles for wasm32-freestanding too
// (shim/ stands in for stdlib.h/math.h/stdio.h but not string.h), and one
// short hand-written name does not justify a shim header.
static bool tables_tune_name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return false;
        a++; b++;
    }
    return *a == *b;
}

static tables_tunable_t *tables_tune_find(const char *name) {
    for (int i = 0; i < TABLES_TUNABLE_COUNT; i++) {
        if (tables_tune_name_eq(g_tablesTunables[i].protoName, name)) return &g_tablesTunables[i];
    }
    return NULL;
}

int tables_tune_count(void) { return TABLES_TUNABLE_COUNT; }

bool tables_tune_describe(int index, const char **name, float *min, float *max, float *def) {
    if (index < 0 || index >= TABLES_TUNABLE_COUNT) return false;
    *name = g_tablesTunables[index].protoName;
    *min = g_tablesTunables[index].min;
    *max = g_tablesTunables[index].max;
    *def = g_tablesTunables[index].def;
    return true;
}

const char *tables_tune_define_name(int index) {
    if (index < 0 || index >= TABLES_TUNABLE_COUNT) return NULL;
    return g_tablesTunables[index].defineName;
}

bool tables_tune_get(const char *name, float *out) {
    tables_tunable_t *t = tables_tune_find(name);
    if (!t) return false;
    *out = *t->value;
    return true;
}

bool tables_tune_set(const char *name, float value, float *outApplied) {
    tables_tunable_t *t = tables_tune_find(name);
    if (!t) return false;
    if (value < t->min) value = t->min;
    if (value > t->max) value = t->max;
    *t->value = value;
    if (outApplied) *outApplied = value;
    // THE SLIDER IS NOW ACTIVELY IN USE - see g_tuneThumbBiasMoved's own
    // comment. Compares by POINTER (which tunable's storage this call just
    // wrote), not by name, so a future second tunable added to
    // g_tablesTunables does not silently start tripping this flag too.
    if (t->value == &g_tuneThumbBiasY) g_tuneThumbBiasMoved = true;
    return true;
}

// A fresh session (tables_enter()) starts by trusting a stored calibration
// (or the default) again - "while it is being moved" (this file's own
// CALIBRATION section, on the tunable-vs-calibration precedence) does not
// mean "was ever moved in a session that ended", or a slider nudged once
// during development would silently outrank every calibration forever.
static void tables_tune_reset_moved(void) { g_tuneThumbBiasMoved = false; }
#else // !TABLES_LIVE_TUNE
#define TOUCH_THUMB_BIAS_Y TOUCH_THUMB_BIAS_Y_DEFAULT

// Reads as empty/false in a normal build, same "0 when the gate is off"
// contract sketch.c's own gate-off stubs use.
int tables_tune_count(void) { return 0; }
bool tables_tune_describe(int index, const char **name, float *min, float *max, float *def) {
    (void)index; (void)name; (void)min; (void)max; (void)def;
    return false;
}
const char *tables_tune_define_name(int index) { (void)index; return NULL; }
bool tables_tune_get(const char *name, float *out) { (void)name; (void)out; return false; }
bool tables_tune_set(const char *name, float value, float *outApplied) {
    (void)name; (void)value; (void)outApplied;
    return false;
}
static void tables_tune_reset_moved(void) {} // no tunable exists to have moved
#endif // TABLES_LIVE_TUNE

// `biasY` is TOUCH_THUMB_BIAS_Y by default, but see tables_effective_bias()
// (NUMPAD TOUCH CALIBRATION below): once a calibration has been run,
// the caller passes a value that is itself a function of `ly` (a fitted
// `alpha + beta*y` line, not a flat constant), computed once by the caller
// before this function ever sees it - numpad_hit() itself stays exactly as
// blind to where the number came from as it always was.
static int numpad_hit(int lx, int ly, float biasY) {
    const int biased = ly - tables_round_i32(biasY);
    if (lx < NUMPAD_X0 || lx >= NUMPAD_X0 + NUMPAD_W) return -1;
    // THE TWO EDGES ARE NOT SYMMETRIC, on purpose.
    //
    // At the TOP the gate reads the RAW touch, then clamps into the first row.
    // Gating on the biased y instead (which is what this did until the bias
    // reached half a key) threw away the top row's own upper half: a finger on
    // the drawn "7" biased its way off the pad and was rejected, so the row
    // could only be hit from its lower part. A key you can see and cannot press
    // is worse than any aiming error this bias corrects.
    //
    // At the BOTTOM the gate reads the BIASED y, which is what lets the zero
    // row keep reaching a bias-worth BELOW the pad, into the counters' band.
    // That band draws two small pills and takes no touch of its own, so the
    // reach costs nothing and gives the row a child aims at lowest the most
    // room.
    if (ly < NUMPAD_Y0 || biased >= NUMPAD_Y0 + NUMPAD_H) return -1;
    ly = biased < NUMPAD_Y0 ? NUMPAD_Y0 : biased;
    int col = (lx - NUMPAD_X0) / CELL_W;
    int row = (ly - NUMPAD_Y0) / CELL_H;
    if (col >= NUMPAD_COLS) col = NUMPAD_COLS - 1;
    if (row >= NUMPAD_ROWS) row = NUMPAD_ROWS - 1;
    // The bottom row is ONE key, the zero, three cells wide. Backspace and
    // check both left with the check key (see TABLE_BASES above), and two
    // dead cells either side of a lone zero would be a keypad with holes in
    // it. A triple-width zero is instead the easiest target on the pad,
    // which suits the digit a child reaches for last and least confidently.
    if (row == NUMPAD_ROWS - 1) return CELL_ZERO;
    return row * NUMPAD_COLS + col;
}

/* =========================================================================
 * NUMPAD TOUCH CALIBRATION - what TOUCH_THUMB_BIAS_Y_DEFAULT above is a
 * stand-in for until this mode has run once.
 *
 * REPLACES A FIVE-CROSSHAIR VERSION, AND WHY. The first build of this mode
 * showed five lone crosshairs (four near the numpad's own corners, one at
 * its centre) on an otherwise blank screen and asked for five taps each.
 * It measured beautifully and it measured the wrong gesture. The owner,
 * after trying it: "the way I type on a numpad is not the same as when I
 * press buttons somewhere. I think I'm being much more precise when you
 * show me the arrows than when I'm on a numpad." A lone crosshair invites a
 * POINTING gesture - slow, deliberate, a finger laid flat on an isolated
 * target. A numpad invites a TYPING gesture - quicker, the thumb arriving
 * at a different angle, attention on the number rather than the pixel.
 * Calibrating with crosshairs measured, very precisely, a gesture he never
 * makes while playing - the classic calibration trap, measuring the
 * calibration task instead of the real one.
 *
 * THE FIX: the calibration screen IS the numpad, drawn exactly as
 * tables_tick()'s own ordinary gameplay draws it - draw_numpad_all(), the
 * same twelve cells, the same glyphs, the same loupe, the same
 * arm/commit-confirm/release-grace gesture (see THE COMMIT PATH below) -
 * so the hand does what it normally does. Only the QUESTION BAND changes:
 * instead of a multiplication fact it shows the single digit to press next.
 *
 * THE SEQUENCE, the owner's own design: prompt 3, 4, 9, then 5 six times -
 * nine samples total, CALIB_SEQ_DIGITS below.
 *
 *   - 3 sits top row, right column; 4 sits middle row, left column; 9 sits
 *     the last digit row, right column (see THE NUMPAD's own phone-dial
 *     layout: digit_to_cell(3)/(4)/(9) resolve to cell 2/3/8, rows 0/1/2).
 *     Together they span the pad both VERTICALLY (which is the only thing
 *     that gives a fitted slope any leverage at all - offset_y is modelled
 *     as a function of y, see below) and HORIZONTALLY (left column vs
 *     right), which is what would show up as an x offset if one exists -
 *     printed (dx below) even though nothing here FITS x, see WHAT IS AND
 *     ISN'T KEPT below.
 *   - 5, at the centre, six times, measures the SCATTER of the user's own
 *     ordinary gesture - not a fresh spot, the same key struck six times in
 *     a row, the way a hand actually repeats a keystroke. That number is
 *     what decides whether a slope means anything at all: if the six 5s
 *     spread as widely as the difference between the top and bottom of the
 *     pad, the slope IS that spread, not a real trend, and a flat constant
 *     is the honest answer instead. Without the repeats there would be no
 *     way to tell a real trend from noise wearing a trend's shape - see
 *     tables_calib_finish()'s own trend-vs-spread comparison, which is
 *     printed, never decided silently.
 *
 * NO REJECTION, EVER. Any touch that lands anywhere on the numpad - the
 * digit prompted or a different one entirely - is accepted as this
 * prompt's own sample, and the sequence advances regardless. There is no
 * "wrong key, try again". If the app corrected him he would start aiming
 * carefully, and this mode would measure his caution instead of his
 * ordinary typing - exactly the trap the crosshair version fell into, from
 * the opposite direction. tables_calib_on_commit() below never inspects
 * which cell the touch actually resolved to for this reason (it is logged,
 * never gated on).
 *
 * THE TARGET IS THE PROMPTED KEY'S DRAWN CENTRE - cell_rect()'s own centre,
 * the thing his eye aims at - never the key's TOUCH zone (numpad_hit()'s
 * biased hit-test exists to correct FOR the gap between the two, so it
 * cannot also be the thing measuring that gap).
 *
 * THE SAMPLE IS THE RAW TOUCH, BEFORE ANY BIAS. tables_gesture_tick()
 * below stores s->rawX/s->rawY from f->touchX/f->touchY - the controller's
 * own reported contact point - every tick a finger is down, completely
 * independent of numpad_hit()'s biased cell resolution (which only ever
 * decides which cell HIGHLIGHTS, never what gets recorded). Verify this by
 * reading tables_calib_on_commit(): it computes dx/dy from `rawX`/`rawY` as
 * handed to it by tables_gesture_tick(), never from s->hoverCell or
 * anything numpad_hit() touched.
 *
 * THE COMMIT PATH. tables_gesture_tick() below is the SAME function
 * ordinary gameplay uses to turn a press into a digit - the identical
 * ARM_SAMPLES/ARM_MS/ARM_RATE_HZ arm, the identical COMMIT_CONFIRM_MS
 * hover-settle, the identical RELEASE_GRACE_MS release-grace, the same
 * loupe. Calibration does not sample on first contact, and does not run a
 * second, hand-rolled tap detector next to the real one (an earlier draft
 * of this mode did exactly that, before the numpad itself was in the
 * picture at all) - it is the literal same call, with the branch AFTER a
 * genuine commit deciding whether the digit goes to action_digit() or to
 * tables_calib_on_commit() (see tables_tick() below). So the point this
 * mode records is provably what the real game would have recorded for the
 * same press, not a plausible stand-in for it.
 *
 * THE GESTURE THAT OPENS/ABORTS IT is unchanged: PWR double-press, checked
 * before the phase dispatch so it works from any screen calibration might
 * interrupt; a second double-press mid-pass aborts outright, saving
 * nothing (tables_calib_pwr_double_press()/tables_calib_abort() below).
 *
 * WHAT IS AND ISN'T KEPT FROM THE MODEL. offset_y = alpha + beta*y is
 * unchanged - one packed uint32, exactly as before (see PACKING below).
 * dx is printed for every sample (see THE SEQUENCE above) so the owner can
 * SEE whether left/right matters, but it is never fitted: the shipped
 * model only ever corrects the y axis, and adding a second axis to what is
 * stored is a bigger change than this rewrite asked for.
 *
 * THE TUNABLE VS. A STORED CALIBRATION is unchanged (tables_effective_bias()
 * below) - still worth naming the bug this whole mode was quietly hiding
 * from itself before today: TABLES_LIVE_TUNE is compiled ON in every
 * emulator build (the devlink tuning registry serves sketch.c/clock.c/
 * tables.c from one binary), so "the gate is on" was true on every single
 * automated run of the OLD calibration mode too, and g_tuneThumbBiasMoved
 * existing at all is what stopped that fact from silently discarding every
 * calibration this mode ever produced in a test - see that flag's own
 * comment for the full reasoning, unchanged by this rewrite.
 * ========================================================================= */
#define CALIB_SEQ_LEN 9
// 3 (top row, right), 4 (middle row, left), 9 (bottom digit row, right),
// then 5 (dead centre) six times - see this section's own header for why
// this exact order and count.
static const int CALIB_SEQ_DIGITS[CALIB_SEQ_LEN] = { 3, 4, 9, 5, 5, 5, 5, 5, 5 };

#define CALIB_DOUBLE_PRESS_WINDOW_MS 500u // clock.c's own DOUBLE_PRESS_WINDOW_MS, same reasoning

// PACKING (storage.h's ONE-record rule). alpha as a signed Q11.4 fixed
// point (1/16 px per count, range +-2048px - the shipped constant is 40px,
// so this is two orders of magnitude of headroom); beta as a signed Q1.14
// (1/16384 per count, range +-2.0 - keys 3/5/9 span 2*CELL_H = 114px of the
// pad's own digit rows, so a slope this fit could plausibly produce is
// well under +-1.0, another order of magnitude of headroom past that).
// Both ranges were sized AFTER running this mode by hand and
// reading what the fitted numbers actually looked like (see this app's own
// report), not guessed - and tables_calib_pack() below LOGS rather than
// silently saturates if a fit somehow lands outside either range anyway.
#define CALIB_ALPHA_SCALE 16.0f
#define CALIB_BETA_SCALE  16384.0f

static uint32_t tables_calib_pack(float alpha, float beta) {
    int32_t a = tables_round_i32(alpha * CALIB_ALPHA_SCALE);
    int32_t b = tables_round_i32(beta * CALIB_BETA_SCALE);
    bool clippedA = a > 32767 || a < -32768;
    bool clippedB = b > 32767 || b < -32768;
    if (a > 32767) a = 32767; else if (a < -32768) a = -32768;
    if (b > 32767) b = 32767; else if (b < -32768) b = -32768;
    if (clippedA || clippedB) {
        printf("tables: calib WARNING - fit exceeded the packed range and was clamped (alpha_clamped=%d beta_clamped=%d)\r\n",
               clippedA ? 1 : 0, clippedB ? 1 : 0);
    }
    return ((uint32_t)(uint16_t)(int16_t)b << 16) | (uint32_t)(uint16_t)(int16_t)a;
}

static void tables_calib_unpack(uint32_t packed, float *outAlpha, float *outBeta) {
    int16_t a = (int16_t)(packed & 0xFFFFu);
    int16_t b = (int16_t)((packed >> 16) & 0xFFFFu);
    *outAlpha = (float)a / CALIB_ALPHA_SCALE;
    *outBeta = (float)b / CALIB_BETA_SCALE;
}

/* ---- THE LOUPE ------------------------------------------------------------
 *
 * A fixed-HEIGHT band above the numpad, LOUPE_ZONE_H tall, otherwise blank
 * - between the question band and the pad, not at the very top of the app
 * (the question band owns that top row instead). Only the bubble's
 * horizontal centre tracks the finger; its vertical centre (LOUPE_CY)
 * never moves. See this file's header for why that is a stronger fix than
 * dynamic vertical placement, not a simplification of one: it removes the
 * "runs out of room above the top row" case for every row at once, and it
 * means the zone this bubble can ever touch is fixed and blank, so
 * redrawing it is always a plain white fill - never a re-render of numpad
 * cells underneath or of the question band above, because nothing else is
 * ever drawn there. LOUPE_ZONE_H is exactly LOUPE_BOX_H - the tightest
 * this zone can be without clipping the bubble. draw_loupe_at() draws a
 * 30x48 digit inside it, whose half-diagonal is sqrt(15^2+24^2) = ~28.3px,
 * so a radius-34 disc clears every corner by ~5.7px. See loupe_update()'s
 * own comment for a real bug this design let through - a flicker under a
 * held-still finger, found and fixed 2026-08-17 - and why the fix belongs
 * in WHEN this zone redraws, not in this geometry.
 */
#define LOUPE_R    34.0f
#define LOUPE_PAD  6
#define LOUPE_BOX_W ((int)(LOUPE_R * 2.0f) + 2 * LOUPE_PAD) // 80
#define LOUPE_BOX_H ((int)(LOUPE_R * 2.0f) + 2 * LOUPE_PAD) // 80
#define LOUPE_CY   (OY + QROW_H + LOUPE_ZONE_H / 2)          // 114
#define LOUPE_CX_MIN (NUMPAD_X0 + (int)LOUPE_R + LOUPE_PAD)          // 74
#define LOUPE_CX_MAX (NUMPAD_X0 + NUMPAD_W - (int)LOUPE_R - LOUPE_PAD) // 294

static uint16_t loupe_bubble_color(void) { return px_swap(0xDEFB); } // #DEDEDE, same grey as menu.c's halo

/* ---- RIGHT/WRONG COLOUR --------------------------------------------------
 *
 * The device is used monochrome everywhere else (gfx.h's own header: "the
 * panel is used as monochrome... the 6-bit green channel doubles as an
 * 8-bit ink/coverage value"), because shapes.h's anti-aliased primitives
 * all funnel through that trick (aa_composite_land converts every colour
 * to a grey level - see shapes.c). AGENTS.md is explicit that colour IS
 * available on this panel, so this app uses it in exactly the places that
 * trick cannot reach anyway: a plain, hard-edged background wash behind
 * the answer, painted with gfx_fill_rect (which writes the real RGB565
 * value, no grey conversion - the same "flat colour, no AA" treatment
 * timer.c's own progress-ring TRACK already uses), and the two counter
 * pills below the pad (see THE COUNTER PILLS below, which reuses this
 * same flat-fill approach for a rounded shape rather than a plain
 * rectangle). All the ink on top of either - the digits, the icons -
 * stays black, so the colour is background tinting, not a second inking
 * system.
 *
 * WHY A WASH AT ALL, tied to "what happens after a wrong answer matters
 * more than the mark itself" (this file's own design brief). A right/wrong
 * verdict she has to read costs a beat of attention a colour does not: two
 * washes she can tell apart at a glance, seen the instant CHECK is
 * released, before she has read anything. Green is unambiguous the same
 * way a green light is; amber (not red) for wrong on purpose - this is a
 * child learning, not a form rejecting bad input, and amber reads as
 * "not yet" rather than as an alarm - consistent with WRONG_RETRY's whole
 * point (try again, no penalty for the attempt itself).
 */
static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    return px_swap(v);
}
static uint16_t tint_right(void) { return rgb565(214, 238, 214); } // pale green
static uint16_t tint_wrong(void) { return rgb565(250, 232, 196); } // pale amber

/* ---- THE QUESTION BAND'S OWN LAYOUT ---------------------------------------
 *
 * One horizontal line, in reading order: base, a multiply mark, factor, an
 * equals mark, then the blank she fills in. The owner's exact mockup
 * (2026-08-17, "7 x 2 = |___") reads as ONE tight phrase with no dead air
 * anywhere in it - which the first version of this band did not do: it
 * reserved a fixed two-digit-wide slot for the factor (it runs 1..10) and
 * always CENTRED whatever was actually typed inside that slot, so a
 * one-digit factor like "1" sat in the middle of a box instead of hugging
 * the multiply mark, and read as "6 x    1 =" - a hole, not a phrase. The
 * fix: base/multiply/factor-START stay fixed (the factor always BEGINS at
 * the same x), but everything from the factor's own width onward - where
 * it ends, where "=" starts, where the blank starts - is a function of
 * whether THIS question's factor is one digit or two, computed fresh from
 * s->factIndex (question_factor_w() and the question_*() positions below
 * it). A single-digit factor now sits flush against the multiply mark; a
 * "10" still fits before "=" without overlapping it. Every fixed position
 * below is a running total (start, then start+width+gap, ...), the same
 * style NUMPAD_Y0 above already uses for a vertical stack.
 *
 * SMALLER THAN THE LANDSCAPE VERSION OF THIS BAND, ON PURPOSE. Portrait's
 * USABLE_W is 348px, not the 428px this band had to fit inside during the
 * app's landscape detour - a genuine width constraint, independent of how
 * much VERTICAL room portrait otherwise buys back. Q_LPAD, Q_GAP, QDIGIT_W
 * and QICON_BOX are all trimmed from that landscape version so the widest
 * real case - a two-digit factor ("10") together with a two-digit answer
 * slot - still fits with margin: Q_LPAD(14) + QDIGIT_W(34) + Q_GAP(14) +
 * QICON_BOX(26) + Q_GAP(14) + Q2W(72) + Q_GAP(14) + QICON_BOX(26) +
 * Q_GAP(14) + Q2W(72) = 300, and the answer's own slot (question_slot_x0()
 * in the worst case, 238) plus ANSWER_BOX_W (88) = 326, both comfortably
 * inside 348.
 */
#define Q_LPAD  14
#define Q_GAP   14
#define QDIGIT_W 34
#define QDIGIT_H 44
#define QDIGIT_T 10
#define QICON_BOX 26                    // multiply/equals icon's own reserved width
#define Q2W (QDIGIT_W * 2 + DIGIT_GAP)  // 72 - a 2-digit factor or answer

#define Q_X0        (OX + Q_LPAD)                    // 24
#define Q_BASE_CX   (Q_X0 + QDIGIT_W / 2)             // 41
#define Q_MUL_X0    (Q_X0 + QDIGIT_W + Q_GAP)         // 72
#define Q_MUL_CX    (Q_MUL_X0 + QICON_BOX / 2)        // 85
#define Q_FACTOR_X0 (Q_MUL_X0 + QICON_BOX + Q_GAP)    // 112 - the factor always STARTS here
// question_factor_w()/question_factor_cx()/question_eq_x0()/question_eq_cx()/
// question_slot_x0()/question_slot_cx() - everything from here on that
// depends on whether THIS question's factor is one digit or two - are
// defined just above redraw_question() below: they need tables_state_t and
// fact_factor(), both declared later in the file than this constants block.

#define Q_SLOT_W    Q2W                                // 72 - up to two digits, always reserved
#define Q_UNDERLINE_Y (QROW_CY + QDIGIT_H / 2 + 2)    // 66 - just below the digit baseline
#define Q_UNDERLINE_R 2.0f

// The answer box: what redraw_answer() erases and redraws wholesale on
// every keystroke - the digits (or their absence), the underline, and (by
// clearing s->cursorDrawn) whatever the blinking cursor had drawn there.
// Wider than the digit slot itself (Q_CURSOR_CLEARANCE) so the caret's
// rightmost resting place - one past a completed 2-digit answer - is
// inside this same box rather than needing its own separate erase rect.
// Its X0 moves with question_slot_x0() (the factor's width shifts it), its
// WIDTH does not.
//
// ANSWER_BOX_H stops right at the underline's own bottom edge (68) rather
// than filling the rest of QROW_H: kept tight because a smaller erase-and-
// redraw rectangle on every keystroke costs less, and there is nothing
// below the underline in this band worth including.
#define Q_CURSOR_CLEARANCE 16
#define ANSWER_BOX_W  (Q_SLOT_W + Q_CURSOR_CLEARANCE) // 88
#define ANSWER_BOX_Y0 QROW_Y0
#define ANSWER_BOX_H  ((int)(Q_UNDERLINE_Y + Q_UNDERLINE_R) - QROW_Y0) // 58 (ends at y=68)

/* ---- THE COUNTER PILLS -----------------------------------------------------
 *
 * The owner's third drawing (see LAYOUT above) replaces the boxed, stacked
 * counters with two COLOURED PILLS side by side along the very bottom: a
 * pink/magenta "X 1" on the left, a green "check 3" on the right, in his
 * own sketch. He then corrected the left pill's colour once this was built
 * and rendered: "plutot que rose fais la case X de la couleur orange que
 * tu utilisais" - orange rather than pink, and specifically the orange
 * already used elsewhere on this device rather than inventing a new one
 * (see THE PILLS' COLOUR below for exactly which constant that is and
 * where it comes from). The right (correct) pill stays green.
 *
 * WHY A FLAT COLOUR NEEDS A DIFFERENT DRAW TECHNIQUE THAN EVERYTHING ELSE
 * IN THIS FILE. Every icon and every digit in this app is drawn with
 * shapes.h's anti-aliased float brush (reached through this file's own
 * portrait wrappers, PORTRAIT DRAWING HELPERS above), which composites by
 * converting whatever colour it is given down to a GREY LEVEL first
 * (aa_composite_land in shapes.c - correct for black ink on white or on a
 * pale wash, since grey IS the channel this panel's monochrome ink already
 * rides on, but it cannot carry an actual saturated colour). A pill's own
 * fill has to be a real RGB565 colour, so it is drawn with shapes.h's
 * OLDER, non-anti-aliased "generation 1" technique instead -
 * shapes_fill_half_width_table() plus a row of gfx_fill_rect() calls,
 * exactly what timer.c's own progress-ring TRACK already does for a flat
 * colour fill (see shapes.h's own header comment on why both generations
 * still coexist deliberately). The icon and the digit drawn ON TOP of a
 * pill are still the ordinary AA black ink, unchanged - this is the same
 * "coloured wash underneath, black ink on top" trick this file's answer
 * band already used before pills existed (see RIGHT/WRONG COLOUR above),
 * just with a rounded pill shape standing in for a plain rectangle.
 *
 * A COLOURED PILL IS ALSO WHAT NOW TELLS A CHILD "THIS IS A READOUT, NOT
 * MORE KEYS" - the job an earlier boxed layout gave to a drawn border (see
 * git history for draw_counter_box(), removed with this redesign): a
 * solid block of colour reads as its own kind of thing even faster than an
 * outline does, and it is also what makes "wrong" and "right" legible to a
 * child who cannot yet read the words, which is this app's own stated
 * reason for using colour at all (see AGENTS.md's header on this app).
 * The CHECK pill still reuses the numpad's own checkmark glyph unchanged -
 * the pill's colour disambiguates it from the CHECK key now, the same job
 * the box used to do. Still exactly TWO counters, not three: no drawing of
 * his has ever shown a time-spent readout, only a cross-count and a
 * check-count, so `s->enterMs`/`enterMsSet` stay in the state struct (a
 * timestamp costs nothing to keep) but nothing computes a displayed value
 * from them.
 *
 * GEOMETRY. Both pills sit in their own band at the very bottom of the
 * portrait canvas, spanning the FULL usable width (OX to OX+USABLE_W,
 * PILL_GAP between them) - deliberately NOT tied to NUMPAD_W: the pad's
 * own width is chosen for a legible key aspect ratio (see THE NUMPAD's own
 * comment on why a cell is not just "however much width is left over"),
 * and a pill is a readout, not a key, so it is free to be as wide as the
 * panel affords rather than inheriting a narrower key-driven width. This
 * is the fourth and last of the stacked bands THE NUMPAD's own comment
 * sums to USABLE_H. PILL_H is centred inside that band with a few pixels
 * of margin above and below, which is also what gives the anti-aliased
 * ink drawn on top of a pill room to fall off before it would ever reach
 * the pushed rectangle's own edge (see this app's own report on the
 * hardware streak bug named in the brief: a pill's push rectangle is
 * checked to cover its rounded ends and every anti-aliased icon/digit
 * pixel drawn on top, not just the flat fill).
 */
#define PILL_GAP 20
#define PILL_W  ((USABLE_W - PILL_GAP) / 2)          // 164 - half the full usable width, minus the gap
#define PILL_H  44
#define COUNTERS_Y0 (NUMPAD_Y0 + NUMPAD_H)           // 382 - the pad's own bottom edge
#define COUNTERS_H  (USABLE_H - QROW_H - LOUPE_ZONE_H - NUMPAD_H) // 56 - see THE NUMPAD's arithmetic
#define PILL_Y0 (COUNTERS_Y0 + (COUNTERS_H - PILL_H) / 2) // 388
#define PILL_WRONG_X0 OX                              // 10 - flush with the panel's own left margin
#define PILL_RIGHT_X0 (OX + PILL_W + PILL_GAP)        // 194 - flush with the panel's own right margin

// Icon and digit placement, shared by both pills (mirrored, since both are
// PILL_W wide) so the glyphs line up in one column and the numbers in
// another. Kept clear of the pill's own rounded ends (radius PILL_H/2 =
// 22): PILL_ICON_CX_OFF (42) puts even the wider cross glyph's own left
// edge a few pixels past that radius, and PILL_NUM_CX_OFF (100) keeps a
// full two-digit number's own right edge inside the straight midsection
// (PILL_W - PILL_H/2 = 142) rather than drifting into the curve. Digit box
// kept well above the width-to-thickness ratio that bit an earlier,
// smaller version of this row: too small relative to stroke thickness t,
// and digits_draw_soft's "0" renders as a solid blob instead of a ring
// (SOFT_INSET plus the stroke radius leaves less than a stroke-width of
// daylight between the two verticals) - not caught by eye until a
// rendered preview was looked at. W/T at or above 3 here keeps clear of it.
#define PILL_ICON_CX_OFF 42
#define PILL_NUM_CX_OFF  100
#define COUNTER_ICON_R 15.0f
// The cross needs its OWN reach, not COUNTER_ICON_R: draw_icon_multiply and
// draw_icon_check both take a "reach" parameter, but it means a different
// fraction of each glyph's own bounding box, so the SAME reach does not
// produce the SAME apparent size. At reach=15, the check's own stroke
// bounding box (including its t=0.26*reach thickness) is about 32x27px;
// the cross's is a symmetric X whose bounding box (2*reach + 2*t, t =
// 0.32*reach) is about 40x40px at the same reach - visibly bigger, exactly
// the "the X and check aren't the same size" bug report. COUNTER_CROSS_R
// is solved from the cross's own bounding-box formula for the width the
// check already has at COUNTER_ICON_R (2*reach*1.32 = 31.8 => reach ~
// 12.0), not copied from the check's reach.
#define COUNTER_CROSS_R 12.0f
#define COUNTER_DIGIT_W 30
#define COUNTER_DIGIT_H 38
#define COUNTER_DIGIT_T 9

/* ---- THE PILLS' COLOUR -----------------------------------------------------
 *
 * Both values are lifted from sketch.c's own colour palette
 * (palette_color(), firmware/apps/sketch.c) rather than invented here, per
 * the owner's own instruction to reuse the orange already used elsewhere
 * on this device rather than a new one: index 1 is sketch.c's "orange"
 * (px_swap(0xFC60), roughly rgb(255,142,0)), index 3 is its "green"
 * (px_swap(0x07E0), pure green). Both are the raw px_swap()'d RGB565
 * constant, copied rather than calling sketch.c's function directly
 * (nothing here wants a dependency on the sketchpad's own palette code for
 * two constants), with the source named so the two never drift apart by
 * accident.
 */
static uint16_t pill_color_wrong(void) { return px_swap(0xFC60); } // sketch.c palette_color(1), orange
static uint16_t pill_color_right(void) { return px_swap(0x07E0); } // sketch.c palette_color(3), green

/* =========================================================================
 * ICONS - shapes.h's float brush only, per decision 0009, reached through
 * this file's own portrait wrappers (PORTRAIT DRAWING HELPERS above).
 * Every one of these is either round (no straight edge to worry about) or
 * a short straight capsule stroke, the same treatment digits_draw_soft's
 * own seven-segment rails already use (a capsule shaft is straight, its
 * ends are round caps - accepted throughout this codebase for exactly this
 * shape, see digits.h). Nothing here is a filled rectangle standing in for
 * an icon.
 * ========================================================================= */

// Backspace: a left-pointing ARROW, "<-" - a shaft plus an arrowhead. The
// owner's third drawing (see LAYOUT above) redrew this key as a proper
// arrow rather than the bare chevron ("<") the earlier two drawings used,
// so this is now a shaft capsule reaching most of the way to the right
// PLUS the same two-capsule chevron wings at the left end, rather than the
// wings alone - the wings on their own were what read as "<".
static void draw_icon_back(int cx, int cy, float reach, uint16_t color) {
    float t = reach * 0.26f;
    float tipX = (float)cx - reach, tipY = (float)cy;
    float wingTopX = (float)cx - reach * 0.15f, wingTopY = (float)cy - reach * 0.55f;
    float wingBotX = (float)cx - reach * 0.15f, wingBotY = (float)cy + reach * 0.55f;
    float shaftX1 = (float)cx + reach * 0.9f;
    shapes_fill_capsule_aa(tipX, tipY, t, wingTopX, wingTopY, t, color);
    shapes_fill_capsule_aa(tipX, tipY, t, wingBotX, wingBotY, t, color);
    shapes_fill_capsule_aa(wingTopX, tipY, t, shaftX1, tipY, t, color);
}

// Submit: a checkmark - two capsules meeting at the low point.
static void draw_icon_check(int cx, int cy, float reach, uint16_t color) {
    float t = reach * 0.26f;
    float p0x = (float)cx - reach * 0.75f, p0y = (float)cy;
    float p1x = (float)cx - reach * 0.15f, p1y = (float)cy + reach * 0.6f;
    float p2x = (float)cx + reach * 0.85f, p2y = (float)cy - reach * 0.7f;
    shapes_fill_capsule_aa(p0x, p0y, t, p1x, p1y, t, color);
    shapes_fill_capsule_aa(p1x, p1y, t, p2x, p2y, t, color);
}

// Multiply: an X of two crossing capsule strokes. Kept dead straight
// rather than bowed (decision 0009's own exception for a shape this small
// and this conventional - a hand-drawn multiply sign is two straight
// strokes even on paper; the digits right next to it are built from
// straight capsule rails too, see this block's own header note).
static void draw_icon_multiply(int cx, int cy, float reach, uint16_t color) {
    float t = reach * 0.32f;
    shapes_fill_capsule_aa((float)cx - reach, (float)cy - reach, t,
                            (float)cx + reach, (float)cy + reach, t, color);
    shapes_fill_capsule_aa((float)cx - reach, (float)cy + reach, t,
                            (float)cx + reach, (float)cy - reach, t, color);
}

// Equals: two straight horizontal strokes, stacked - as canonically
// straight on paper as the multiply mark above, and dead straight for the
// same reason (decision 0009's own exception for a glyph this small and
// this conventional).
static void draw_icon_equals(int cx, int cy, float reach, uint16_t color) {
    float t = reach * 0.32f;
    float half = reach * 0.5f;
    shapes_fill_capsule_aa((float)cx - reach, (float)cy - half, t,
                            (float)cx + reach, (float)cy - half, t, color);
    shapes_fill_capsule_aa((float)cx - reach, (float)cy + half, t,
                            (float)cx + reach, (float)cy + half, t, color);
}

/* ---- COUNTER ICONS ---------------------------------------------------------
 *
 * The two counters are told apart by an EXPLICIT glyph, per the owner's
 * own drawings - a cross for wrong, a checkmark for right (see this file's
 * header for why that replaces an earlier ring/disc pairing). "Correct"
 * reuses draw_icon_check verbatim: the SAME mark the numpad's own CHECK
 * key draws - every one of the owner's drawings keeps that collision on
 * purpose. An earlier layout resolved it with a drawn box around the
 * panel; this one resolves it with the pill's own colour instead (see THE
 * COUNTER PILLS above) - either way, not by giving the counter a different
 * glyph. "Wrong" reuses draw_icon_multiply's
 * own X under its own name, for the reason decision 0009 already accepted
 * a dead-straight X at this size for the multiply sign: a cross is two
 * straight strokes even on paper.
 */
static void draw_icon_cross(int cx, int cy, float reach, uint16_t color) {
    draw_icon_multiply(cx, cy, reach, color);
}

/* =========================================================================
 * NUMBERS. One helper draws 1 or 2 digits (0..99), left-to-right, centred
 * on cx - the shared renderer for the numpad's own digit keys, the
 * question's two factors (the second factor runs 1..10, so it alone needs
 * the two-digit case), the answer she is building, and the two counters.
 * ========================================================================= */
#define DIGIT_GAP 4

static void draw_number_lr(int cx, int cy, int digitW, int digitH, int t,
                            int value, bool padTo2, uint16_t color) {
    if (value < 0) value = 0;
    if (value > 99) value = 99;
    bool twoDigits = padTo2 || value >= 10;
    // DIGITS_ONE_CENTER everywhere here: this numpad is a self-contained
    // widget nowhere near the panel's own edge (unlike clock.c's outer digit
    // cells - see digits_one_style_t, digits.h), so there is no outer margin
    // for a "1" to hug toward, and centring - this function's own behaviour
    // before oneStyle existed - is still the right call.
    if (!twoDigits) {
        digits_draw_soft(DIGITS_PORTRAIT, cx - digitW / 2, cy - digitH / 2, digitW, digitH, t, value, color, DIGITS_ONE_CENTER);
        return;
    }
    int tens = value / 10, ones = value % 10;
    int totalW = digitW * 2 + DIGIT_GAP;
    int x0 = cx - totalW / 2;
    digits_draw_soft(DIGITS_PORTRAIT, x0, cy - digitH / 2, digitW, digitH, t, tens, color, DIGITS_ONE_CENTER);
    digits_draw_soft(DIGITS_PORTRAIT, x0 + digitW + DIGIT_GAP, cy - digitH / 2, digitW, digitH, t, ones, color, DIGITS_ONE_CENTER);
}

/* =========================================================================
 * FACTS: the 6, 7 and 8 tables, second factor 1..10 - 30 facts.
 *
 * WHICH QUESTIONS COME UP, and why this changed on 2026-08-17. The first
 * version picked the next question by a WEIGHTED random draw, a per-fact
 * weight bumped on a wrong answer and eased back down on a right one, so a
 * fact she had just missed would resurface sooner. Pedagogically motivated,
 * but the owner's own instruction after seeing it in practice was blunter
 * than that trade-off: "randomize the numbers" - questions have to be
 * genuinely unpredictable to a child who has seen the previous few, and a
 * weight that keeps climbing on a fact she is struggling with does the
 * opposite, clustering the SAME few questions in a burst that reads as
 * anything but random. pick_next_fact() now draws UNIFORMLY over every
 * fact except the one just asked: no fact is ever more likely than another,
 * and the one guaranteed exclusion is exactly what makes an immediate
 * back-to-back repeat impossible BY CONSTRUCTION (verified by
 * feature-tables.ts's own "never repeats immediately" check, driven for
 * many consecutive questions, not just asserted from reading the code).
 *
 * WHICH TABLE, AND WHY THERE IS NO PICKER SCREEN. All three tables are
 * always mixed; there is no per-table selection. Two reasons, not one:
 * interleaving different tables is the better way to practice in the
 * first place (it is what stops "the sixes" from being memorised as a
 * chant rather than as thirty independent facts - the same argument
 * against straight rotation, one level up), and a picker is exactly the
 * kind of screen this device's numpad-sized layout has no spare room for
 * without shrinking either the pad or the counters. If a wider range of
 * tables is ever wanted, TABLE_BASES below is the one array that grows;
 * nothing else in this file assumes there are three.
 */
// NINES ADDED, AND THE ONES DROPPED, and the second half is the load-bearing
// change. The owner watched his niece use this and reported that she typed
// 42, then waited: the check key is not in her model of what answering is.
// The fix is to judge the moment the answer is complete, which needs the app
// to know how many digits "complete" means - and dropping x1 makes that one
// number for every question in the deck. 6x2 through 9x10 is 12 to 90:
// EVERY product is exactly two digits. Nothing else in this file has to ask.
//
// The ones were the cheapest thing to give up for that: x1 is the fact a
// child already has before she needs a drill, and it was the only source of
// a one-digit answer.
#define TABLE_COUNT 4
static const int TABLE_BASES[TABLE_COUNT] = { 6, 7, 8, 9 };
#define FACTOR_MIN 2
#define FACTOR_MAX 10
#define FACTOR_COUNT (FACTOR_MAX - FACTOR_MIN + 1)  // 9
#define FACT_COUNT (TABLE_COUNT * FACTOR_COUNT)     // 36
#define ANSWER_DIGITS 2                             // true of all 36, see above

static int fact_base(int f)    { return TABLE_BASES[f / FACTOR_COUNT]; }
static int fact_factor(int f)  { return (f % FACTOR_COUNT) + FACTOR_MIN; }
static int fact_product(int f) { return fact_base(f) * fact_factor(f); }

static uint32_t rng_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* =========================================================================
 * STATE
 * ========================================================================= */
typedef enum {
    PHASE_ASK = 0,        // waiting for her to build and submit an answer
    PHASE_RIGHT,          // brief positive pause, then a new question
    PHASE_WRONG_RETRY,     // brief pause, same question, answer cleared
    PHASE_WRONG_REVEAL,   // shows the right answer, then a new question
} phase_t;

// Feedback timings. Right is quick (keep momentum); retry is quicker still
// (get back to trying); reveal is the longest of the three on purpose - see
// THE QUESTION STATE below for why it is the one place this app slows down
// rather than moving on immediately.
#define RIGHT_MS        650
#define RETRY_MS        550
#define REVEAL_MS       1600

// THE GESTURE. Arm and release-grace are copied verbatim from menu.c/
// four.c - the same reasoning, the same measured controller, no reason to
// re-derive numbers that already have a citation (AGENTS.md names both
// explicitly as the ones to reuse). What IS re-derived is the commit
// confirm window: menu.c's 150ms exists to protect against a WRONG LAUNCH
// nobody is watching for in the instant it happens - an irreversible act
// with no undo. Here, every cell change is visible in the loupe AS it
// happens (nothing is filtered from what she sees), the release is what
// commits and nothing else, and a wrong commit is one backspace away from
// fixed. Lag is real cost here (it is the whole reason the loupe has to
// feel connected to her finger) where a 150ms-style excursion is not,
// because nothing launches on an excursion - only a release does, and she
// is watching the bubble when she decides to lift.
//
// MEASURED, not guessed - tools/sweep-tables-commit.ts, rebuilding
// emu.wasm at each candidate (EMU_EXTRA_DEFINES=-DCOMMIT_CONFIRM_MS=<n>)
// and driving 600 press-hold-release trials per value under the same
// calibrated jitter profile menu.c's own table used (34 dropouts/sec,
// 80-250px jitter for up to 3 reports), landing on each of the 9 digit
// cells in turn, measuring how often the digit that actually commits is
// NOT the one the thumb was on:
//
//     confirm window   wrong commits (of 600)   cancelled (of 600)
//            0ms              3 (0.50%)                 14
//           72ms              1 (0.17%)                 10
//          150ms              0 (0.00%)                 10   <- menu.c's own number
//
// Two things this table says together. First, even 0ms already beats
// menu.c's own worst case by a wide margin (0.5% wrong commits here vs
// menu's 14 excursions per 100 gestures) - expected, since nothing here
// commits until a genuine, RELEASE_GRACE-confirmed lift, where menu's own
// worst case was measuring a HALO that could move mid-drag with no lift
// involved at all. Second, the marginal gain from 72ms to 150ms (0.17% to
// 0.00%) is real but small, while the cost is not symmetric: this same
// window gates what the loupe SHOWS as well as what commits (set_hover()
// is called from both), so at 150ms every digit's first appearance in the
// loupe is delayed a full 150ms from the moment she lands a finger. 72ms
// is chosen with margin over the observed knee (wrong commits are already
// at 0 by 68-72ms in a smaller sweep) rather than at menu's own number,
// because the two failure costs are not the same: menu's 150ms protects an
// irreversible launch nobody is watching for; a wrong digit here is one
// backspace away from fixed and she is looking at the loupe the whole
// time. 72ms is also close to 3 jitter reports' own worst-case duration at
// the controller's 60Hz report rate (3 * 16.7ms = 50ms) plus margin for a
// dropout extending that episode's effective lifetime - the same
// arithmetic menu.c's own comment names as a lower bound before it
// overshoots it for its own, different reasons.
//
// THE FIRST HOVER OF A GESTURE IS SHOWN WITH NO CONFIRM DELAY AT ALL. The
// table above already argues 72ms is a CHANGE-filtering window, chosen
// against jitter between two cells the loupe has already shown her at least
// one of. On the very first armed sample of a gesture hoverCell is still -1:
// nothing is on screen yet to filter against, and nothing commits on a
// hover in the first place (a release does, re-reading hoverCell then - see
// this file's header, "WHAT COMMITS IS WHAT THE LOUPE SHOWED"). So the
// tick's own armed branch below shows the first cell the instant it arms
// (~40-67ms in, per ARM_SAMPLES/ARM_MS/ARM_RATE_HZ) rather than waiting
// another COMMIT_CONFIRM_MS on top - the owner's own complaint after
// testing in the emulator ("i have to press for a fairly long time for a
// touch to register") was exactly this stacked ~112ms floor before anything
// lit up. Found and fixed 2026-08-17.
//
// RELEASE_GRACE_MS was copied verbatim from menu.c/four.c, per the note
// above, but MEASURED here too rather than left on that citation alone
// (tools/sweep-tables-grace.ts: rebuilds emu.wasm at each candidate with
// EMU_EXTRA_DEFINES=-DRELEASE_GRACE_MS=<n>, drives 600 held/dragged/tapped
// trials per value under TOUCHSIM_HARDWARE_MEASURED's 34 dropouts/sec -
// this project's own measured worst case, not an assumption; see that
// file's header for the "does the emulator even model dropouts" check this
// was verified against before trusting any of it). What it found was NOT
// what shrinking a copied 300ms number was expected to find:
//
//     grace(ms)  premature commits (of 200-400 trials, held-still / drag)
//            80        94%  /  83%
//           120      34.5%  /  18%
//           160        17%  /  9.5%
//           200       3.5%  /  0.5%
//           250         1%  /  1%
//           260      0.75%  /  0%
//           270         0%  /  0.25%
//           280      0.25%  /  0.25%
//           290         0%  /  0%
//           300         0%  /  0%    <- shipped value
//           320         0%  /  0%
//           350         0%  /  0%
//
// A REAL FLOOR, not a conservative guess with room to spare. Premature
// commits (a dropout run outlasting the grace window while a real finger is
// still down, read as a genuine lift - the exact failure this window
// exists to bridge) fall off a cliff between 80ms and 200ms, then sit in a
// noisy near-zero tail from 250-280ms (0-1%, consistent with a true rate
// too small for a few hundred trials to pin down exactly) before going
// cleanly and repeatably to zero at 290ms and up. 300ms is not "menu.c's
// number, probably fine here too" - it is inside the first band this
// sweep found with a clean zero AND margin either side of it (290 and 320
// both clean too), which is exactly the "smallest value with real margin"
// bar COMMIT_CONFIRM_MS's own table above was chosen against. Shrinking it
// would trade a real, measured safety margin for a delay that Task 1's own
// fix (immediate first hover) already removed from what the owner actually
// felt - RELEASE_GRACE_MS only ever gates the instant AFTER a genuine
// lift, not how fast a key lights up. Left at 300.
#define ARM_SAMPLES        4
#define ARM_MS            40
#define ARM_RATE_HZ       15u
#ifndef RELEASE_GRACE_MS
#define RELEASE_GRACE_MS 300
#endif
#ifndef COMMIT_CONFIRM_MS
#define COMMIT_CONFIRM_MS 72
#endif

typedef struct {
    // Facts.
    int      factIndex;    // -1 until the first question is picked
    uint32_t rng;
    bool     rngSeeded;

    // The question in progress.
    int      answerDigits[2];
    int      answerLen;
    int      attemptsOnQuestion;

    phase_t  phase;
    uint32_t phaseDeadlineMs;

    // Counters.
    uint32_t wrongCount;
    uint32_t correctCount;
    uint32_t enterMs;   // kept (cheap) even though nothing displays it any
    bool     enterMsSet; // more - see THE COUNTER PILLS' own comment

    // Touch gesture (see THE GESTURE above; shared with calibration's own
    // NUMPAD TOUCH CALIBRATION section via tables_gesture_tick() below).
    bool     contactSeen;
    bool     armed;
    uint32_t gestureStartMs;
    uint32_t lastContactMs;
    int      contactCount;
    int      hoverCell;       // -1 = none/cancel
    int      pendingCell;
    uint32_t pendingSinceMs;
    int      rawX, rawY;      // last raw (unbiased) touch sample this gesture
                               // reported - what calibration measures against;
                               // ordinary gameplay never reads these two, only hoverCell.

    // The loupe's last drawn box AND cell, so a tick that hides, moves or
    // recontents it knows exactly what to erase and whether anything
    // actually changed. loupeCell is tracked separately from position -
    // see loupe_update()'s own comment for why position alone is not
    // enough (a finger can hold its horizontal position still while
    // drifting across a row boundary, changing the hovered cell with the
    // loupe's own box never moving).
    bool     loupeVisible;
    int      loupeBx, loupeBy, loupeBw, loupeBh;
    int      loupeCell;

    // The answer blank's blinking cursor (see THE BLINKING CURSOR /
    // update_cursor() below). Bookkeeping only - what it draws is a pure
    // function of nowMs, s->answerLen and phase.
    bool     cursorDrawn;
    int      cursorX;

    // NUMPAD TOUCH CALIBRATION (see that section above numpad_hit()). PWR
    // double-press opens/aborts it, independent of `phase` - it can
    // interrupt a question in progress; nothing about the question's own
    // state below is touched, calibration just borrows the screen and the
    // touch input for a few taps. Unlike the crosshair version this
    // replaced, calibration now shares tables_gesture_tick()'s own
    // contactSeen/armed/hoverCell/loupe* fields above rather than keeping a
    // private copy of them - see THE COMMIT PATH in that section's header
    // for why sharing the exact same gesture state is the whole point.
    bool     calibActive;
    int      calibSeqIdx;                 // 0..CALIB_SEQ_LEN; next prompt / samples landed so far
    int      calibDy[CALIB_SEQ_LEN];      // raw (touchY - prompted key's drawn centre Y), pre-bias
    int      calibDx[CALIB_SEQ_LEN];      // raw (touchX - prompted key's drawn centre X), pre-bias - printed only, never fitted

    // A private double-press detector for the PWR gesture - clock.c's
    // pwr_double_press() idiom, its own copy of the bookkeeping (see that
    // function's own comment for why a shared one is not worth it here).
    bool     calibHavePendingShort;
    uint32_t calibPendingShortMs;

    // The fitted/stored calibration THIS RUN is using (loaded once, on
    // enter() - see tables_effective_bias()). Independent of calibActive:
    // this is what numpad_hit() reads on every ordinary touch, not just
    // while a calibration pass is in progress.
    bool     calibHave;
    float    calibAlpha, calibBeta;
} tables_state_t;

static tables_state_t *s_tables;

/* =========================================================================
 * DRAWING - the numpad
 * ========================================================================= */
static void render_cell(int cell, bool hovered) {
    int bx, by, bw, bh;
    cell_rect(cell, &bx, &by, &bw, &bh);
    gfx_fill_rect(bx, by, bw, bh, PX_WHITE);
    if (hovered) {
        float rw = (float)bw / 2.0f - 4.0f;
        float rh = (float)bh / 2.0f - 4.0f;
        float r = rw < rh ? rw : rh;
        shapes_fill_disc_aa((float)(bx + bw / 2), (float)(by + bh / 2), r, loupe_bubble_color());
    }
    int cx = bx + bw / 2, cy = by + bh / 2;
    if (cell_is_digit(cell)) {
        draw_number_lr(cx, cy, 24, 32, 7, cell_digit_value(cell), false, PX_BLACK);
    }
    // No other kind of cell exists any more: the pad is ten digits and
    // nothing else.
}

static void push_cell(int cell) {
    int bx, by, bw, bh;
    cell_rect(cell, &bx, &by, &bw, &bh);
    gfx_push_wh(bx, by, bw, bh);
}

static void draw_numpad_all(void) {
    for (int c = 0; c < 12; c++) render_cell(c, false);
}

/* ---- the loupe: content, then erase/redraw for one tick ------------------ */
static void draw_loupe_at(int bx, int by, int bw, int bh, int cell) {
    gfx_fill_rect(bx, by, bw, bh, PX_WHITE);
    int cx = bx + bw / 2, cy = by + bh / 2;
    float r = LOUPE_R;
    shapes_fill_disc_aa((float)cx, (float)cy, r, loupe_bubble_color());
    if (cell_is_digit(cell)) {
        draw_number_lr(cx, cy, 30, 48, 10, cell_digit_value(cell), false, PX_BLACK);
    }
}

// Recomputes the loupe for this tick from the live (raw) touch x and the
// confirmed hoverCell - but only when something the bubble shows would
// actually change. Called only while a gesture is armed; see
// tables_tick().
//
// THE FLICKER BUG, AND HOW IT WAS FOUND. The owner's own report: "quand je
// reste enfoncé sur une touche pour la voir en surbrillance la pastille de
// surbrillance clignote et flicker, elle devrait être visible en entier" -
// holding a finger still on a key, the magnifier flickers instead of
// standing solidly visible. Reproduced before anything was changed, per
// this project's own standing rule (red before green applies to a
// mechanism, not only to an assertion): driving a held-still contact
// through the emulator and reading emu_push_count()/emu_push_x/y/w/h
// directly for each tick (the settled framebuffer alone cannot show a
// transient - decision 0003's own limit) showed the SAME 80x80 rectangle
// pushed TWICE, back to back, on every one of 15 consecutive ticks with an
// unchanged touch coordinate. The old body of this function erased the
// whole bubble (a white fill) and redrew it from scratch as two SEPARATE
// gfx_push() calls, unconditionally, on every tick loupe_update() ran -
// there was no check for whether the box had moved or the hovered cell
// had changed at all. Two real pushes a tick, the second painting over the
// first's blank erase, is exactly a flicker at the tick rate. Both pushed
// rectangles were a clean multiple of 8 wide, not truncated, so this is
// NOT the gfx.c row-width push defect another agent is chasing elsewhere
// in this tree (that one corrupts a single push's own pixels; this one
// was two clean pushes happening when zero were needed) - a redraw-storm
// bug local to this function, fixed here.
//
// THE FIX, in two parts. First, skip entirely when nothing the bubble
// shows would change: same visibility, same box position (nbx - nby never
// moves, only the horizontal centre tracks the finger) AND same cell
// (s->hoverCell against s->loupeCell). Position alone is not enough: a
// finger can hold its x steady while y drifts across a ROW boundary,
// changing which cell numpad_hit() names without moving the loupe's own
// box at all, and skipping the redraw in that case would leave a stale
// digit on screen - so s->loupeCell is now tracked to catch exactly that.
// Second, when the box DOES move while staying visible, erase and redraw
// as ONE combined region pushed ONCE, not two separate pushes: old and
// new always share the same y-band (nby is constant), so their union is a
// single rectangle - fill it white, draw the new bubble on top, push the
// union. Appearing and disappearing still need only their own single
// rectangle each.
static void loupe_update(tables_state_t *s, int rawX) {
    bool show = s->hoverCell >= 0;
    int nby = LOUPE_CY - LOUPE_BOX_H / 2, nbw = LOUPE_BOX_W, nbh = LOUPE_BOX_H;
    int nbx = 0;
    if (show) {
        int cx = rawX;
        if (cx < LOUPE_CX_MIN) cx = LOUPE_CX_MIN;
        if (cx > LOUPE_CX_MAX) cx = LOUPE_CX_MAX;
        nbx = cx - LOUPE_BOX_W / 2;
    }

    bool unchanged = (show == s->loupeVisible) &&
                      (!show || (nbx == s->loupeBx && nby == s->loupeBy && s->hoverCell == s->loupeCell));
    if (unchanged) return;

    if (s->loupeVisible && show) {
        int ux0 = s->loupeBx < nbx ? s->loupeBx : nbx;
        int uxEnd = (s->loupeBx + s->loupeBw) > (nbx + nbw) ? (s->loupeBx + s->loupeBw) : (nbx + nbw);
        gfx_fill_rect(ux0, nby, uxEnd - ux0, nbh, PX_WHITE);
        draw_loupe_at(nbx, nby, nbw, nbh, s->hoverCell);
        gfx_push_wh(ux0, nby, uxEnd - ux0, nbh);
    } else if (s->loupeVisible) { // hiding
        gfx_fill_rect(s->loupeBx, s->loupeBy, s->loupeBw, s->loupeBh, PX_WHITE);
        gfx_push_wh(s->loupeBx, s->loupeBy, s->loupeBw, s->loupeBh);
    } else { // appearing (show is true here, loupeVisible was false)
        draw_loupe_at(nbx, nby, nbw, nbh, s->hoverCell);
        gfx_push_wh(nbx, nby, nbw, nbh);
    }
    s->loupeVisible = show;
    s->loupeCell = s->hoverCell;
    s->loupeBx = nbx; s->loupeBy = nby; s->loupeBw = nbw; s->loupeBh = nbh;
}

static void loupe_hide(tables_state_t *s) {
    if (!s->loupeVisible) return;
    gfx_fill_rect(s->loupeBx, s->loupeBy, s->loupeBw, s->loupeBh, PX_WHITE);
    gfx_push_wh(s->loupeBx, s->loupeBy, s->loupeBw, s->loupeBh);
    s->loupeVisible = false;
}

static void set_hover(tables_state_t *s, int cell) {
    if (cell == s->hoverCell) return;
    int old = s->hoverCell;
    s->hoverCell = cell;
    if (old >= 0)  { render_cell(old, false); push_cell(old); }
    if (cell >= 0) { render_cell(cell, true); push_cell(cell); }
}

/* =========================================================================
 * DRAWING - the question band (full width, across the top)
 * ========================================================================= */

// Everything past the factor's own START depends on whether THIS
// question's factor (1..10) is one digit or two - see THE QUESTION BAND'S
// OWN LAYOUT above for why.
static int question_factor_w(tables_state_t *s) {
    return fact_factor(s->factIndex) >= 10 ? Q2W : QDIGIT_W;
}
static int question_factor_cx(tables_state_t *s) {
    return Q_FACTOR_X0 + question_factor_w(s) / 2;
}
static int question_eq_x0(tables_state_t *s) {
    return Q_FACTOR_X0 + question_factor_w(s) + Q_GAP;
}
static int question_eq_cx(tables_state_t *s) {
    return question_eq_x0(s) + QICON_BOX / 2;
}
static int question_slot_x0(tables_state_t *s) { // the blank starts here
    return question_eq_x0(s) + QICON_BOX + Q_GAP;
}
static int question_slot_cx(tables_state_t *s) {
    return question_slot_x0(s) + Q2W / 2;
}

// The fixed part: base, multiply mark, factor, equals mark. Only redrawn
// when a NEW question starts (start_new_question()), never per keystroke -
// the blank at the right (redraw_answer(), below) owns its own separate
// rect for that.
//
// Erases the FULL band width (not just up to this question's own content),
// because question_slot_x0() moves depending on whether the factor is one
// digit or two (see this block's header comment above): the PREVIOUS
// question could have ended further right or left than this one, and a
// partial erase sized to only the new content would leave the old
// question's ink stranded outside it. A full-width erase is a bounded,
// once-per-question cost (never per keystroke, never per tick) so there is
// no reason to chase the tighter bound.
static void redraw_question(tables_state_t *s) {
    gfx_fill_rect(OX, QROW_Y0, USABLE_W, QROW_H, PX_WHITE);
    draw_number_lr(Q_BASE_CX, QROW_CY, QDIGIT_W, QDIGIT_H, QDIGIT_T, fact_base(s->factIndex), false, PX_BLACK);
    draw_icon_multiply(Q_MUL_CX, QROW_CY, 12.0f, PX_BLACK);
    draw_number_lr(question_factor_cx(s), QROW_CY, QDIGIT_W, QDIGIT_H, QDIGIT_T, fact_factor(s->factIndex), false, PX_BLACK);
    draw_icon_equals(question_eq_cx(s), QROW_CY, 12.0f, PX_BLACK);
    gfx_push_wh(OX, QROW_Y0, USABLE_W, QROW_H);
}

// tint: PX_WHITE at rest, a pale wash while a wrong attempt is showing -
// see redraw_answer's own callers. Plain gfx_fill_rect, never the AA
// primitives: those convert every colour to grey (aa_composite_land),
// which is correct for ink but would silently discard an actual tint.
static void redraw_answer(tables_state_t *s, uint16_t tint, bool showCorrectValue) {
    int x0 = question_slot_x0(s), cx = question_slot_cx(s);
    // Tint only the visible slot (Q_SLOT_W, the same reference every digit
    // centres on below) - the extra Q_CURSOR_CLEARANCE strip past it is
    // erase margin for the caret's own rightmost rest position, never part
    // of the "answer box" a child reads, so it always stays plain white
    // regardless of tint (see this file's own history for the bias bug
    // painting the WHOLE ANSWER_BOX_W with the tint used to cause).
    gfx_fill_rect(x0, ANSWER_BOX_Y0, Q_SLOT_W, ANSWER_BOX_H, tint);
    gfx_fill_rect(x0 + Q_SLOT_W, ANSWER_BOX_Y0, ANSWER_BOX_W - Q_SLOT_W, ANSWER_BOX_H, PX_WHITE);
    if (showCorrectValue) {
        draw_number_lr(cx, QROW_CY, QDIGIT_W, QDIGIT_H, QDIGIT_T, fact_product(s->factIndex), false, PX_BLACK);
    } else if (s->answerLen > 0) {
        int value = s->answerDigits[0];
        if (s->answerLen == 2) value = s->answerDigits[0] * 10 + s->answerDigits[1];
        draw_number_lr(cx, QROW_CY, QDIGIT_W, QDIGIT_H, QDIGIT_T, value, false, PX_BLACK);
    }
    // The blank itself: the owner's own underline, always drawn under the
    // slot whether it currently holds a digit, two digits, or nothing yet.
    shapes_fill_capsule_aa((float)x0, (float)Q_UNDERLINE_Y, Q_UNDERLINE_R,
                            (float)(x0 + Q_SLOT_W), (float)Q_UNDERLINE_Y, Q_UNDERLINE_R, PX_BLACK);
    gfx_push_wh(x0, ANSWER_BOX_Y0, ANSWER_BOX_W, ANSWER_BOX_H);
    // This just repainted the whole box, cursor included - update_cursor()
    // redraws it fresh next tick against whatever s->answerLen is now.
    s->cursorDrawn = false;
}

/* ---- there is no cursor any more -------------------------------------------
 *
 * The owner's mockup labelled the answer blank "blink" and it carried a
 * caret for a while. Two things then removed the need for it, in order.
 *
 * First the check key went (see action_digit): the answer is judged the
 * moment its second digit lands, so the caret's whole job - "this is where
 * the next thing you type will go, keep going" - now lasts exactly one
 * keystroke. Second, on the real panel the owner read it as a defect
 * rather than as a hint: "le curseur de saisie de texte est pixellise, je
 * pense qu'on devrait l'enlever". A 4px capsule is anti-aliased and still
 * reads as a stair on a 322ppi panel held at reading distance.
 *
 * What is left says the same thing without a moving part: the underline
 * shows where the answer goes, and the digits appear on it as she taps.
 * BLINK_PERIOD_MS survives because the wrong-answer reveal still uses it.
 */
#define BLINK_PERIOD_MS 500u

/* ---- the counter pills --------------------------------------------------
 *
 * gfx_fill_pill: a flat-coloured, rounded-end "stadium" shape - a
 * rectangle body with a semicircular cap at each end - filled the same
 * generation-1, non-anti-aliased way timer.c's own progress ring is (see
 * THE COUNTER PILLS above for why the AA float brush cannot carry a real
 * colour). shapes_fill_half_width_table() gives the half-width of a circle
 * of radius `h/2` at every row of an `h`-tall grid; row `i`'s span is the
 * rectangle's own full width (w - h) plus twice that row's half-width,
 * centred - at the middle row the half-width equals the radius and the
 * span is the full pill width `w`; at the top/bottom rows it collapses
 * toward the rectangle body alone, which is exactly the rounded-end
 * silhouette. `h` is capped at 64 (this app's pills are 44px tall) so a
 * fixed-size stack array covers it with no VLA.
 */
static void gfx_fill_pill(int x, int y, int w, int h, uint16_t color) {
    if (h <= 0 || w <= 0) return;
    int16_t halfW[64];
    int hh = h > 64 ? 64 : h;
    int r = h / 2;
    shapes_fill_half_width_table(halfW, hh, (float)r);
    for (int row = 0; row < hh; row++) {
        int half = halfW[row];
        int rowW = (w - 2 * r) + 2 * half;
        if (rowW < 1) rowW = 1;
        int rowX = x + (r - half);
        gfx_fill_rect(rowX, y + row, rowW, 1, color);
    }
}

// One pill: the flat coloured fill, then the icon and the digit in black
// ink on top (see THE COUNTER PILLS above for why that split is necessary
// rather than a style choice). Pushes exactly the pill's own bounding
// rectangle, which contains every pixel either draw step can touch - the
// fill by construction (it never paints outside [x,x+w)x[y,y+h)), the icon
// and digit because their own centres and reaches were chosen with margin
// from the pill's rounded ends (see PILL_ICON_CX_OFF/PILL_NUM_CX_OFF's own
// comment).
static void redraw_pill(int x0, uint16_t color, int iconKind, int value) {
    gfx_fill_pill(x0, PILL_Y0, PILL_W, PILL_H, color);
    int iconCx = x0 + PILL_ICON_CX_OFF, iconCy = PILL_Y0 + PILL_H / 2;
    if (iconKind == 0) draw_icon_cross(iconCx, iconCy, COUNTER_CROSS_R, PX_BLACK);
    else draw_icon_check(iconCx, iconCy, COUNTER_ICON_R, PX_BLACK);
    draw_number_lr(x0 + PILL_NUM_CX_OFF, iconCy, COUNTER_DIGIT_W, COUNTER_DIGIT_H,
                    COUNTER_DIGIT_T, value, false, PX_BLACK);
    gfx_push_wh(x0, PILL_Y0, PILL_W, PILL_H);
}

static void redraw_wrong(tables_state_t *s) {
    uint32_t v = s->wrongCount > 99 ? 99 : s->wrongCount;
    redraw_pill(PILL_WRONG_X0, pill_color_wrong(), 0, (int)v);
}
static void redraw_correct(tables_state_t *s) {
    uint32_t v = s->correctCount > 99 ? 99 : s->correctCount;
    redraw_pill(PILL_RIGHT_X0, pill_color_right(), 1, (int)v);
}

/* =========================================================================
 * QUESTION SELECTION AND RESOLUTION
 * ========================================================================= */
// Uniform over every fact (0..FACT_COUNT-1) on the very first question
// (factIndex is -1, nothing to exclude yet); uniform over the other
// FACT_COUNT-1 facts on every question after that, which is what makes an
// immediate repeat structurally impossible rather than merely unlikely.
static int pick_next_fact(tables_state_t *s) {
    if (s->factIndex < 0) return (int)(rng_next(&s->rng) % FACT_COUNT);
    int idx = (int)(rng_next(&s->rng) % (FACT_COUNT - 1));
    if (idx >= s->factIndex) idx++; // shift past the excluded slot
    return idx;
}

static void start_new_question(tables_state_t *s) {
    s->factIndex = pick_next_fact(s);
    s->answerLen = 0;
    s->attemptsOnQuestion = 0;
    s->phase = PHASE_ASK;
    redraw_question(s);
    redraw_answer(s, PX_WHITE, false);
}

// The SAME question, after a first wrong attempt: clears the answer she
// typed and goes back to PHASE_ASK, but does NOT touch factIndex or
// attemptsOnQuestion (already 1, so a second wrong answer reaches the
// reveal rather than a third retry) or redraw the question itself (it has
// not changed). Kept as its own function rather than a branch inside
// start_new_question(): that function's whole job is picking a NEW fact,
// and folding "stay on this one" into it as a special case is exactly how
// this bug happened the first time here - a retry silently called
// start_new_question(), which picked a fresh fact AND reset
// attemptsOnQuestion to 0, so two wrong answers in a row could never reach
// the reveal at all (found by feature-tables.ts's own "a second wrong
// attempt reveals the answer" check, not by inspection).
static void resume_same_question(tables_state_t *s) {
    s->answerLen = 0;
    s->phase = PHASE_ASK;
    redraw_answer(s, PX_WHITE, false);
}

// PEDAGOGY: a wrong answer does not move on immediately and does not show
// the right answer immediately either. First wrong attempt -> the answer
// she typed is shown briefly against the amber wash (see tint_wrong()'s
// own comment for why amber and not red), then cleared, same question - a
// retrieval attempt before anything is given away, because being told the
// answer without trying again teaches "wait for the answer" rather than
// "recall the fact". A SECOND wrong attempt on the same question reveals
// the correct product, still on amber (REVEAL_MS, the longest pause of the
// three), and moves on - so she is never stuck retrying a fact she does
// not yet know, and always leaves having SEEN the right answer once before
// a new question replaces it on screen. Getting it right on the retry
// counts as correct: the point is recall, and a corrected recall is still
// recall.
//
// THE WRONG COUNTER counts every wrong SUBMISSION now, not just the final
// give-up - a real logic bug, not a display one, and an asymmetric one:
// correctCount already incremented on every successful CHECK press,
// whichever attempt it landed on, but wrongCount only incremented on the
// SECOND wrong attempt, silently dropping the first. Worked example: wrong,
// then wrong again (gives up) used to read wrongCount=1 for two actual
// wrong submissions; wrong, then right on the retry used to read
// correctCount=1, wrongCount=0 for one wrong submission and one right one -
// two different counting units on the two counters is exactly what "the
// count of wrong doesn't add up" was seeing. Both submissions increment
// wrongCount now (redraw_wrong() called immediately on the retry too, not
// only on give-up), so wrongCount+correctCount always equals the number of
// CHECK presses that carried a digit, the same unit on both sides.
static void resolve_answer(tables_state_t *s, uint32_t nowMs, bool correct) {
    s->attemptsOnQuestion++;
    if (correct) {
        s->correctCount++;
        redraw_answer(s, tint_right(), false); // she already sees her own correct digits
        s->phase = PHASE_RIGHT;
        s->phaseDeadlineMs = nowMs + RIGHT_MS;
        redraw_correct(s);
        printf("tables: %d x %d = %d correct\r\n", fact_base(s->factIndex), fact_factor(s->factIndex), fact_product(s->factIndex));
        return;
    }
    s->wrongCount++;
    redraw_wrong(s);
    if (s->attemptsOnQuestion < 2) {
        redraw_answer(s, tint_wrong(), false); // her own wrong digits, on amber
        s->phase = PHASE_WRONG_RETRY;
        s->phaseDeadlineMs = nowMs + RETRY_MS;
        printf("tables: wrong, retry\r\n");
        return;
    }
    redraw_answer(s, tint_wrong(), true); // reveal the correct product, still amber
    s->phase = PHASE_WRONG_REVEAL;
    s->phaseDeadlineMs = nowMs + REVEAL_MS;
    printf("tables: %d x %d = %d, gave up after 2 tries\r\n", fact_base(s->factIndex), fact_factor(s->factIndex), fact_product(s->factIndex));
}

/* =========================================================================
 * INPUT ACTIONS
 * ========================================================================= */
// THE ANSWER JUDGES ITSELF, there is no submit. The owner, after watching
// his niece: "6x7 elle a marqué 42 et elle attendait. Après elle se
// rappelait qu'il fallait qu'elle appuie sur rentrer." Pressing a key to say
// "I have finished answering" is a convention she has no reason to hold; the
// answer being finished is a fact the app can see for itself, because every
// product in this deck is ANSWER_DIGITS long (see TABLE_BASES).
//
// A slip is therefore marked wrong rather than corrected, since backspace
// went with the check key. That is deliberate and it is the one real cost
// here: PHASE_WRONG_RETRY already puts the SAME question straight back, so a
// fat finger costs one red mark and an immediate second go, and there is
// nothing left on the pad a child has to be taught. If the false-wrong rate
// turns out to bother her in use, backspace comes back - that is a smaller
// change than teaching a key nobody presses.
static void action_digit(tables_state_t *s, int digit, uint32_t nowMs) {
    if (s->answerLen >= ANSWER_DIGITS) return;
    s->answerDigits[s->answerLen++] = digit;
    redraw_answer(s, PX_WHITE, false);
    printf("tables: digit %d\r\n", digit); // observable by the regression tests, same
                                            // convention as morpion.c's "mark cell=" line
    if (s->answerLen < ANSWER_DIGITS) return;

    int value = 0;
    for (int i = 0; i < s->answerLen; i++) value = value * 10 + s->answerDigits[i];
    resolve_answer(s, nowMs, value == fact_product(s->factIndex));
}

/* =========================================================================
 * CALIBRATION MODE - LOGIC. See "NUMPAD TOUCH CALIBRATION" above (just
 * after numpad_hit()) for the model, the measurement discipline and the
 * storage rule; this is where all of it is actually driven and drawn.
 * ========================================================================= */

// The double-press detector - clock.c's pwr_double_press(), copied rather
// than shared (this file has no dependency on clock.c, and the shape is
// four lines - a shared header for four lines is not worth the coupling).
static bool tables_calib_pwr_double_press(tables_state_t *s, uint32_t nowMs, uint8_t key) {
    if (!(key & KEY_SHORT)) return false;
    if (s->calibHavePendingShort && (nowMs - s->calibPendingShortMs) <= CALIB_DOUBLE_PRESS_WINDOW_MS) {
        s->calibHavePendingShort = false;
        return true;
    }
    s->calibHavePendingShort = true;
    s->calibPendingShortMs = nowMs;
    return false;
}

// THE TUNABLE VS. A STORED CALIBRATION. TABLES_LIVE_TUNE being COMPILED IN
// is not the same question as the slider being MOVED - the devlink tuning
// registry is built into every emulator image regardless (sketch.c/
// clock.c/tables.c share one binary), so "the gate is on" is true on every
// automated run too, calibration's own test included; if that alone made
// the tunable win, no test could ever observe a calibration actually being
// used, and this app's calibration mode would be unfalsifiable in the
// emulator by construction. So the precedence is checked against whether
// tables_tune_set() has actually been called for THIS tunable THIS session
// (g_tuneThumbBiasMoved, cleared on every tables_enter()) - a slider that
// only sometimes reflects what it says would be worse than one that always
// does, so ONCE moved it keeps winning for the rest of the session, but a
// session that never touched it defers to whatever is actually stored. In
// every normal build (TABLES_LIVE_TUNE off, what ships) there is no
// tunable to move at all, so this always falls through to the calibration/
// default check below.
static float tables_effective_bias(int rawY) {
#if TABLES_LIVE_TUNE
    if (g_tuneThumbBiasMoved) return TOUCH_THUMB_BIAS_Y;
#endif
    if (s_tables != NULL && s_tables->calibHave) {
        return s_tables->calibAlpha + s_tables->calibBeta * (float)rawY;
    }
    return TOUCH_THUMB_BIAS_Y_DEFAULT;
}

// Insertion-sort median of n (n<=CALIB_SEQ_LEN) - cheap, no qsort needed
// (this build has no <stdlib.h> qsort to call anyway). The MEDIAN, not the
// mean - see this file's CALIBRATION header for why a mean would be
// dragged by one bad tap. Used both for the six key-5 samples (n=6, an
// even count - the average of the middle two, rounded) and for the plain
// constant over all nine samples (n=9, odd - the middle one outright); one
// generic function rather than two near-duplicates.
static int tables_calib_median_n(const int *v, int n) {
    int a[CALIB_SEQ_LEN];
    for (int i = 0; i < n; i++) a[i] = v[i];
    for (int i = 1; i < n; i++) {
        int key = a[i], j = i - 1;
        while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; j--; }
        a[j + 1] = key;
    }
    if (n % 2 == 1) return a[n / 2];
    return tables_round_i32(((float)a[n / 2 - 1] + (float)a[n / 2]) / 2.0f);
}

// Ordinary least squares, offset_y = alpha + beta*y, over the three
// row-representative deltas (keys 3/5/9's own medians - key 5's is the
// median of six, keys 3/9's are a single sample each), not the nine raw
// samples: the per-key median already picked one honest number per row,
// and the fit runs on those three - see this file's CALIBRATION header for
// why key 5's own median (not key 4's single tap) stands in for the middle
// row.
static void tables_calib_fit(const int targetY[], const int delta[], int n,
                              float *outAlpha, float *outBeta) {
    float nf = (float)n;
    float sumX = 0, sumY = 0, sumXY = 0, sumXX = 0;
    for (int i = 0; i < n; i++) {
        float x = (float)targetY[i], y = (float)delta[i];
        sumX += x; sumY += y; sumXY += x * y; sumXX += x * x;
    }
    float denom = nf * sumXX - sumX * sumX;
    float beta = (denom != 0.0f) ? (nf * sumXY - sumX * sumY) / denom : 0.0f;
    float alpha = (sumY - beta * sumX) / nf;
    *outAlpha = alpha;
    *outBeta = beta;
}

// Renders a value already scaled by 1000 (e.g. tables_round_i32(x*1000.0f))
// as a three-decimal string ("-12.345", "0.000") into `out` (>= 12 bytes) -
// this build's printf has no %f (emu_shim.c's own header: grepped, not
// guessed, and no firmware source needs one), so a fitted float is turned
// into text here and handed to printf as a plain %s.
static void tables_fmt_milli(int32_t milli, char *out) {
    bool neg = milli < 0;
    int32_t m = neg ? -milli : milli;
    int32_t whole = m / 1000, frac = m % 1000;
    char rev[12]; int rn = 0;
    if (whole == 0) rev[rn++] = '0';
    else while (whole > 0) { rev[rn++] = (char)('0' + whole % 10); whole /= 10; }
    int oi = 0;
    if (neg) out[oi++] = '-';
    while (rn > 0) out[oi++] = rev[--rn];
    out[oi++] = '.';
    out[oi++] = (char)('0' + (frac / 100) % 10);
    out[oi++] = (char)('0' + (frac / 10) % 10);
    out[oi++] = (char)('0' + frac % 10);
    out[oi] = '\0';
}

#define CALIB_DOT_R    6.0f
#define CALIB_DOT_GAP  20

// The nine samples-so-far, as a row of dots centred under the prompted
// digit - drawn in the loupe's own reserved zone, the same "otherwise
// blank paper" band THE LOUPE above describes. Filled for a sample already
// landed, a hollow ring for one still to come. Temporarily painted over by
// the REAL loupe bubble while a finger is actually down (loupe_update()/
// loupe_hide() own that band exactly as they do during ordinary gameplay -
// see NUMPAD TOUCH CALIBRATION above numpad_hit() for why calibration
// shares that machinery rather than avoiding it) and repainted fresh the
// moment tables_calib_draw_screen() runs again after the commit, which is
// harmless: nothing here is load-bearing while a gesture is in flight.
static void tables_calib_draw_progress(tables_state_t *s) {
    int totalW = (CALIB_SEQ_LEN - 1) * CALIB_DOT_GAP;
    int x0 = PANEL_W / 2 - totalW / 2;
    for (int i = 0; i < CALIB_SEQ_LEN; i++) {
        int cx = x0 + i * CALIB_DOT_GAP;
        if (i < s->calibSeqIdx) shapes_fill_disc_aa((float)cx, (float)LOUPE_CY, CALIB_DOT_R, PX_BLACK);
        else shapes_fill_annulus_aa((float)cx, (float)LOUPE_CY, CALIB_DOT_R, CALIB_DOT_R - 2.0f, PX_BLACK);
    }
}

// A full repaint: the prompted digit (top band, where the question
// normally sits), the sample-progress dots (loupe band) and the numpad
// itself, drawn exactly as draw_numpad_all() draws it for ordinary
// gameplay - see this section's own header for why the pad has to be the
// real one rather than a stand-in target. Called only on entry and after
// each sample lands - a handful of times over a whole pass, not per tick,
// so a whole-panel clear-and-push (gfx_push_all(), the same "rare, big
// transition" cost clock.c's own paint_all() accepts for set mode) is the
// honest cost here.
static void tables_calib_draw_screen(tables_state_t *s) {
    gfx_fill_rect(0, 0, PANEL_W, PANEL_H, PX_WHITE);
    draw_number_lr(PANEL_W / 2, QROW_CY, QDIGIT_W, QDIGIT_H, QDIGIT_T, CALIB_SEQ_DIGITS[s->calibSeqIdx], false, PX_BLACK);
    tables_calib_draw_progress(s);
    draw_numpad_all();
    gfx_push_all();
}

// Repaints the ORDINARY practice screen - what calibration drew over.
// Shared by abort (nothing about the session changed) and finish (a
// calibration just landed); neither touches factIndex/answerLen/the
// counters, so this is exactly what tables_enter() itself draws, minus the
// parts that reset a fresh session.
static void tables_calib_redraw_practice_screen(tables_state_t *s) {
    draw_numpad_all();
    redraw_question(s);
    redraw_answer(s, PX_WHITE, false);
    redraw_wrong(s);
    redraw_correct(s);
    gfx_push_all();
}

static void tables_calib_enter(tables_state_t *s) {
    s->calibActive = true;
    s->calibSeqIdx = 0;
    // Nothing half-seen from the ordinary numpad gesture may survive into
    // calibration - same reasoning clock.c's own set-mode-open comment
    // gives for its own double-press. This IS the ordinary gesture's own
    // state now (see tables_gesture_tick() below numpad_hit() for why), so
    // resetting it here is the same reset tables_tick()'s own release path
    // already performs on every ordinary cancel.
    s->contactSeen = false;
    s->armed = false;
    s->pendingCell = -1;
    loupe_hide(s);
    set_hover(s, -1);
    printf("tables: calibration opened\r\n");
    tables_calib_draw_screen(s);
}

static void tables_calib_abort(tables_state_t *s) {
    s->calibActive = false;
    printf("tables: calibration aborted, nothing saved\r\n");
    tables_calib_redraw_practice_screen(s);
}

// All nine samples done. Prints the six-5s spread (the noise floor - see
// this section's own header), THEN the four per-key medians, THEN the fit
// alongside the plain constant (both together, so the owner can compare
// them without scrolling back), THEN the trend-vs-spread verdict, THEN
// saves - one storage_save_u32() call, here, once, per storage.h's own
// rule.
static void tables_calib_finish(tables_state_t *s) {
    int d3 = s->calibDy[0], d4 = s->calibDy[1], d9 = s->calibDy[2];
    const int *fives = &s->calibDy[3]; // the six key-5 samples, in sequence order

    int fMin = fives[0], fMax = fives[0];
    for (int i = 1; i < 6; i++) {
        if (fives[i] < fMin) fMin = fives[i];
        if (fives[i] > fMax) fMax = fives[i];
    }
    int fMedian = tables_calib_median_n(fives, 6);
    int fSpread = fMax - fMin;
    printf("tables: calib SPREAD OF THE SIX 5s (the noise floor of his own gesture): min=%d max=%d median=%d spread=%d px\r\n",
           fMin, fMax, fMedian, fSpread);

    printf("tables: calib per-key medians (raw dy, pre-bias): 3=%d 4=%d 9=%d 5=%d px\r\n", d3, d4, d9, fMedian);

    // The fit runs on keys 3/5/9 - the three ROWS the sequence actually
    // spans - using 5's own median (six samples) rather than 4's single tap
    // for the middle row: see this section's own header for why key 4's
    // job is the horizontal (x) check, not the vertical fit.
    int bx, by, bw, bh;
    cell_rect(digit_to_cell(3), &bx, &by, &bw, &bh); int y3 = by + bh / 2;
    cell_rect(digit_to_cell(5), &bx, &by, &bw, &bh); int y5 = by + bh / 2;
    cell_rect(digit_to_cell(9), &bx, &by, &bw, &bh); int y9 = by + bh / 2;
    int fitY[3]  = { y3, y5, y9 };
    int fitDy[3] = { d3, fMedian, d9 };
    float alpha, beta;
    tables_calib_fit(fitY, fitDy, 3, &alpha, &beta);

    int constant = tables_calib_median_n(s->calibDy, CALIB_SEQ_LEN);

    char alphaStr[16], betaStr[16];
    tables_fmt_milli(tables_round_i32(alpha * 1000.0f), alphaStr);
    tables_fmt_milli(tables_round_i32(beta * 1000.0f), betaStr);
    printf("tables: calib FIT (least squares over 3/5/9): offset_y = alpha + beta*y   alpha=%s px   beta=%s px/px\r\n",
           alphaStr, betaStr);
    printf("tables: calib CONSTANT (median of all nine dy) = %d px\r\n", constant);

    int trendMax = d3 > fMedian ? d3 : fMedian; trendMax = trendMax > d9 ? trendMax : d9;
    int trendMin = d3 < fMedian ? d3 : fMedian; trendMin = trendMin < d9 ? trendMin : d9;
    int trend = trendMax - trendMin;
    const char *verdict = trend > fSpread ? "larger than" : (trend < fSpread ? "smaller than" : "about the same as");
    printf("tables: calib the vertical trend across 3/5/9 (%d px) is %s the six-5s spread (%d px)\r\n",
           trend, verdict, fSpread);

    uint32_t packed = tables_calib_pack(alpha, beta);
    storage_save_u32(STORAGE_KIND_TABLES_CALIB, packed);
    s->calibHave = true;
    s->calibAlpha = alpha;
    s->calibBeta = beta;
    printf("tables: calibration saved\r\n");

    s->calibActive = false;
    tables_calib_redraw_practice_screen(s);
}

// One commit landed during calibration - see this section's own header
// (THE COMMIT PATH) for why this is provably the same commit ordinary
// gameplay would have recorded, and (THE TARGET IS THE PROMPTED KEY'S
// DRAWN CENTRE / THE SAMPLE IS THE RAW TOUCH) for why the delta below is
// computed from `rawX`/`rawY` - tables_gesture_tick()'s own pre-bias
// reading - against cell_rect() rather than against `hitCell`'s touch
// zone. `hitCell` is only ever logged here (NO REJECTION, EVER above): it
// never decides whether this sample counts.
static void tables_calib_on_commit(tables_state_t *s, int rawX, int rawY, int hitCell) {
    int idx = s->calibSeqIdx;
    int digit = CALIB_SEQ_DIGITS[idx];
    int bx, by, bw, bh;
    cell_rect(digit_to_cell(digit), &bx, &by, &bw, &bh);
    int targetX = bx + bw / 2, targetY = by + bh / 2;
    int dx = rawX - targetX, dy = rawY - targetY;
    s->calibDx[idx] = dx;
    s->calibDy[idx] = dy;
    printf("tables: calib %d/%d prompted=%d target=(%d,%d) raw=(%d,%d) dx=%d dy=%d px landed=%d\r\n",
           idx + 1, CALIB_SEQ_LEN, digit, targetX, targetY, rawX, rawY, dx, dy,
           cell_is_digit(hitCell) ? cell_digit_value(hitCell) : -1);

    s->calibSeqIdx++;
    if (s->calibSeqIdx < CALIB_SEQ_LEN) {
        tables_calib_draw_screen(s);
        return;
    }
    tables_calib_finish(s);
}

/* =========================================================================
 * app_t callbacks
 * ========================================================================= */
static void tables_enter(void) {
    tables_state_t *s = s_tables = APP_STATE(tables_state_t);
    s->factIndex = -1;
    s->hoverCell = -1;
    s->pendingCell = -1;

    tables_tune_reset_moved(); // a fresh session trusts calibration/default again - see its own comment
    uint32_t calibRaw = 0;
    s->calibHave = storage_get_u32(STORAGE_KIND_TABLES_CALIB, &calibRaw);
    if (s->calibHave) {
        tables_calib_unpack(calibRaw, &s->calibAlpha, &s->calibBeta);
        printf("tables: loaded stored calibration\r\n");
    } else {
        printf("tables: no stored calibration, using the default bias\r\n");
    }

    draw_numpad_all();
    start_new_question(s);
    redraw_wrong(s);
    redraw_correct(s);
    printf("tables: entered\r\n");
}

// THE SHARED GESTURE ENGINE. Ordinary gameplay and NUMPAD TOUCH
// CALIBRATION (above numpad_hit()) both turn a press into a cell through
// this exact function - the identical arm (ARM_SAMPLES/ARM_MS/ARM_RATE_HZ),
// hover with commit-confirm debounce (COMMIT_CONFIRM_MS, s->pendingCell),
// loupe (loupe_update()/loupe_hide()) and release-grace (RELEASE_GRACE_MS)
// tables_tick() used to run inline for gameplay alone. Pulled out here so
// calibration can call the SAME code rather than a second, hand-rolled
// gesture detector next to it - see THE COMMIT PATH in that section's
// header for why that provability matters.
//
// Returns true exactly once per gesture, on a genuine COMMIT: a real
// release, after a real arm, landing on a real cell (wasArmed && cell>=0) -
// the same verdict the old inline code computed before deciding whether to
// call action_digit(). On that true, *outCell is the resolved (BIASED)
// cell exactly as ordinary gameplay would name it; *outRawX/*outRawY is
// the LAST raw, UNBIASED touch sample (s->rawX/s->rawY, updated every tick
// a finger is down) this gesture reported - the pre-bias point
// calibration's own model fits against. On a cancel (release with no arm,
// or outside the pad) this returns false and the caller does nothing,
// exactly as before.
static bool tables_gesture_tick(tables_state_t *s, const app_frame_t *f, int *outCell, int *outRawX, int *outRawY) {
    if (f->touchDown) {
        if (!s->contactSeen) {
            s->contactSeen = true;
            s->gestureStartMs = f->nowMs;
            s->contactCount = 0;
        }
        s->lastContactMs = f->nowMs;
        s->contactCount++;
        s->rawX = f->touchX;
        s->rawY = f->touchY;

        uint32_t elapsed = f->nowMs - s->gestureStartMs;
        if (!s->armed && s->contactCount >= ARM_SAMPLES && elapsed >= ARM_MS &&
            (uint32_t)s->contactCount * 1000u >= ARM_RATE_HZ * elapsed) {
            s->armed = true;
        }
        if (s->armed) {
            // Panel coordinates directly - no rotation needed, this app is
            // portrait, native, unrotated (see LAYOUT above). Landscape
            // apps map through panel_to_land() first; this one does not
            // need or have such a function.
            int lx = f->touchX, ly = f->touchY;
            int cell = numpad_hit(lx, ly, tables_effective_bias(ly));
            if (s->hoverCell < 0) {
                // The FIRST cell of this gesture. COMMIT_CONFIRM_MS exists to
                // filter jitter against an already-shown cell (see THE
                // GESTURE above); there is nothing shown yet to filter
                // against, hoverCell is still -1, and nothing commits on a
                // hover in the first place (a release does, re-reading
                // hoverCell at that point - "WHAT COMMITS IS WHAT THE LOUPE
                // SHOWED", this file's header). So showing the first cell
                // the instant the gesture arms is free: it cannot let a
                // commit happen on a cell the loupe never displayed,
                // because the loupe is about to display exactly this one.
                set_hover(s, cell);
                s->pendingCell = cell;
                s->pendingSinceMs = f->nowMs;
            } else if (cell == s->pendingCell) {
                if (cell != s->hoverCell && (f->nowMs - s->pendingSinceMs) >= COMMIT_CONFIRM_MS) {
                    set_hover(s, cell);
                }
            } else {
                s->pendingCell = cell;
                s->pendingSinceMs = f->nowMs;
            }
            loupe_update(s, lx);
        }
        return false;
    }

    if (!s->contactSeen) return false;
    if ((f->nowMs - s->lastContactMs) < RELEASE_GRACE_MS) return false;

    bool wasArmed = s->armed;
    int cell = s->hoverCell;
    int rawX = s->rawX, rawY = s->rawY;
    s->contactSeen = false;
    s->armed = false;
    s->contactCount = 0;
    s->pendingCell = -1;
    set_hover(s, -1);
    loupe_hide(s);

    if (!wasArmed || cell < 0) return false; // cancelled - see this file's header

    *outCell = cell;
    *outRawX = rawX;
    *outRawY = rawY;
    return true;
}

static void tables_tick(const app_frame_t *f) {
    tables_state_t *s = s_tables;

    if (!s->rngSeeded) {
        s->rng = f->nowMs ^ 0xB5297A4Du;
        if (s->rng == 0) s->rng = 1;
        s->rngSeeded = true;
        s->enterMs = f->nowMs;
        s->enterMsSet = true;
    }

    // THE CALIBRATION GESTURE, checked before anything else so it can open
    // (or abort) calibration from any phase, feedback pauses included -
    // see NUMPAD TOUCH CALIBRATION above numpad_hit() for the design.
    if (tables_calib_pwr_double_press(s, f->nowMs, f->key)) {
        if (s->calibActive) tables_calib_abort(s);
        else tables_calib_enter(s);
        return; // the double-press itself is not also a touch/tick action
    }
    if (s->calibActive) {
        int cell, rawX, rawY;
        if (tables_gesture_tick(s, f, &cell, &rawX, &rawY)) {
            tables_calib_on_commit(s, rawX, rawY, cell);
        }
        return;
    }

    if (s->phase != PHASE_ASK) {
        if (f->nowMs >= s->phaseDeadlineMs) {
            if (s->phase == PHASE_WRONG_RETRY) resume_same_question(s);
            else start_new_question(s); // PHASE_RIGHT or PHASE_WRONG_REVEAL: this question is done
        }
        return; // feedback states ignore touch entirely - see this file's header
    }

    int cell, rawX, rawY;
    if (tables_gesture_tick(s, f, &cell, &rawX, &rawY)) {
        if (cell_is_digit(cell)) action_digit(s, cell_digit_value(cell), f->nowMs);
    }
}

const app_t g_tablesApp = {
    .name       = "TABLES",
    .enter      = tables_enter,
    .tick       = tables_tick,
    .leave      = NULL,
    .landscape  = false,
    .wantsShake = false,
};
