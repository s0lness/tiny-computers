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
// NUMPAD TOUCH CALIBRATION (below numpad_hit()) prompts only digits 1..9
// (CELL_ZERO is never one of the nine learned keys), so "which cell does
// this prompted digit draw as" is always cell = digit - 1 there - no
// separate digit-to-cell inverse is needed the way an earlier version of
// this file (when calibration could still land on 0) once carried one.

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
// after numpad_hit() below, replaces it with nine MEASURED per-key centres
// once the owner runs that mode; TOUCH_THUMB_BIAS_Y_DEFAULT remains exactly
// what a never-calibrated puck falls back to - see tables_uncalibrated_bias()/
// tables_tunable_moved() for the precedence between this constant, a
// stored calibration, and the TABLES_LIVE_TUNE bench slider just below.

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
// tables_tunable_moved() (past tables_state_t, below) for why this exists
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

// `biasY` is TOUCH_THUMB_BIAS_Y_DEFAULT, or the live-tune slider's value
// once it has actually been moved (tables_uncalibrated_bias(), NUMPAD
// TOUCH CALIBRATION below) - this function is the UNCALIBRATED rectangle
// path only, now. Once a calibration has actually been run, the gesture
// tick calls numpad_hit_calibrated() instead (nearest of nine learned
// centres, no bias at all) rather than passing this function a fitted
// value - see that function's own comment, right after this one, for why
// a shared rectangle grid cannot represent nine independently learned
// centres in the first place.
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
 * REWRITTEN 2026-08-19, FROM A FITTED LINE TO NINE LEARNED CENTRES. The
 * version this replaced prompted 3, 4, 9, then 5 six times (nine samples)
 * and fit offset_y = alpha + beta*y through them. It measured its own
 * design flaw once real runs existed to compare: three back-to-back runs on
 * the board, same person, same grip, minutes apart, put key 4 at 57, 50,
 * then 26 - a 31px swing on the ONE sample that key ever got - while the
 * centre (six samples) got its own honest median every time. The centre had
 * six chances to average out his gesture noise; the three keys that decide
 * the fitted SLOPE had one each. Worse, in the third run the centre (51)
 * read higher than the bottom key (48), breaking the monotonic top-to-
 * bottom trend the first run showed cleanly - the vertical trend across
 * 3/5/9 no longer even exceeded the six-5s spread that same run measured as
 * his own noise floor. A single sample cannot measure anything against a
 * 22-30px noise floor, and a nine-point pass spent six of its nine samples
 * on the one key whose position was never in question. The owner's own
 * fix, verbatim: "you should ask me to type in a number, then register
 * where i type and that should become the new target (over like 10 taps
 * for each number)" - no fitted line, no assumed functional form, each
 * key's own target replaced by where he actually put his thumb.
 *
 * STILL REPLACES A FIVE-CROSSHAIR VERSION BEFORE THAT ONE, AND THE REASON
 * STILL HOLDS. The five-crosshair design (four corners plus centre, five
 * taps each) measured a POINTING gesture - slow, deliberate, a finger laid
 * flat on an isolated target - where a numpad gets a TYPING gesture:
 * quicker, the thumb arriving at a different angle, attention on the
 * number rather than the pixel. The owner, after trying it: "the way I
 * type on a numpad is not the same as when I press buttons somewhere. I
 * think I'm being much more precise when you show me the arrows than when
 * I'm on a numpad." THE FIX carried forward unchanged into this rewrite:
 * the calibration screen IS the numpad, drawn exactly as tables_tick()'s
 * own ordinary gameplay draws it - draw_numpad_all(), the same twelve
 * cells, the same glyphs, the same loupe, the same arm/commit-confirm/
 * release-grace gesture (see THE COMMIT PATH below) - so the hand does
 * what it normally does. Only the QUESTION BAND changes: instead of a
 * multiplication fact it shows the single digit to press next.
 *
 * THE SEQUENCE: all NINE digit keys (1-9; CELL_ZERO is not one of them,
 * see NEAREST LEARNED CENTRE below), TEN taps each, 90 taps total
 * (CALIB_KEYS x CALIB_TAPS_PER_KEY, CALIB_SEQ_LEN below). Deliberately not
 * reduced from the owner's own "10 taps for each number" - the whole
 * lesson of the run-to-run swing above is that fewer samples measure
 * nothing against his own gesture noise.
 *
 * ROTATED, NOT BLOCKED. The prompt order is 1,2,...,9, 1,2,...,9, ten times
 * over (calib_seq_digit() below) rather than ten of "1" then ten of "2" and
 * so on. A blocked order would let each key's own ten samples fall inside
 * one short window of the pass - the first few seconds for key 1, the last
 * few for key 9 - so anything that drifts slowly over a 90-tap session (the
 * hand tiring, the grip settling, the device shifting slightly against
 * whatever it is resting on) would show up as a PER-KEY bias rather than
 * the session-wide noise it actually is: key 9's median would carry
 * whatever the hand was doing two minutes in, key 1's whatever it was
 * doing at second zero, and the two would disagree for a reason that has
 * nothing to do with where either key actually sits. Rotating spreads every
 * key's ten samples across the FULL span of the pass, so a slow drift
 * contributes equally to every key's own median instead of unevenly to
 * whichever key happened to be asked at that moment - the same reasoning
 * the old design already used for the six-5s-in-a-row noise measurement,
 * generalised from "one key, repeated, to catch gesture noise" to "every
 * key, interleaved, to keep session-wide drift from reading as a per-key
 * offset". The visible cost is that the SAME digit is not asked twice in a
 * row (a mild rhythm change from the old design's six-in-a-row block); the
 * benefit is a median immunised against exactly the kind of drift a real
 * 90-tap pass is long enough to develop.
 *
 * NO REJECTION, EVER - unchanged from both earlier designs. Any touch that
 * lands anywhere on the numpad - the digit prompted or a different one
 * entirely - is accepted as this prompt's own sample, and the sequence
 * advances regardless. There is no "wrong key, try again": correcting him
 * would make him aim carefully, and this mode would measure his caution
 * instead of his ordinary typing. tables_calib_on_commit() below never
 * inspects which cell the touch actually resolved to for this reason (it
 * is logged, never gated on).
 *
 * PER KEY, THE LEARNED CENTRE IS THE MEDIAN OF ITS TEN RAW TOUCH POINTS,
 * NOT THE MEAN - same reason as the old design's own six-5s median: one bad
 * tap (a slip, a double-report, a finger catching the wrong angle) drags a
 * mean toward it forever; a median shrugs it off as long as it is not more
 * than half the sample. Only Y is learned (see ONLY DY IS STORED below); X
 * is measured and printed but never fitted, because the measured dx medians
 * (this app's own report) sit at -2 to -7px, inside his own noise floor -
 * the same "is this trend bigger than the noise" question the old design's
 * own trend-vs-spread comparison asked, just answered for x once rather
 * than re-asked every pass, since there is no cheap way to print a
 * per-pass x noise floor without repeating a key back-to-back the way the
 * old six-5s block did, which the rotation above deliberately gives up.
 * The target measured against is still cell_rect()'s own DRAWN centre -
 * the thing his eye aims at - never the key's TOUCH zone, for the same
 * reason as before: numpad_hit()'s bias (or, once calibrated, the learned
 * centre itself) exists to correct FOR the gap between drawn and touched,
 * so it cannot also be the thing measuring that gap.
 *
 * NEAREST LEARNED CENTRE, NOT SHIFTED RECTANGLES. The old model kept a
 * single shared rectangle grid and moved every zone by the same fitted
 * amount; nine independently learned centres do not share a common shift,
 * so shifted rectangles would either overlap or leave gaps between
 * neighbours whose centres moved by different amounts. Assigning a touch to
 * whichever of the nine learned centres is nearest (Euclidean, on the RAW
 * touch, no bias applied - the centres already sit where he actually
 * touches) partitions the pad by his own taps with neither gap nor overlap
 * and assumes nothing about the shape of that partition - see
 * numpad_hit_calibrated() below numpad_hit(). CELL_ZERO is deliberately NOT
 * one of the nine learned keys (the owner's own instruction: "keep it as
 * the whole bottom row"), so numpad_hit_calibrated() keeps a rectangle-
 * style gate for it - a touch at or below a line drawn a half-key beneath
 * the LOWEST of the nine learned centres (whichever key that turns out to
 * be, not assumed to be row 3) is the zero key outright, before nearest-
 * centre is even evaluated for the digit keys. That gate is anchored to the
 * learned centres themselves rather than to the drawn grid, so it still
 * gives the zero key its full reach below the pad no matter how far down
 * his own taps pulled the ninth row - see that function's own comment for
 * why a fixed line (the drawn grid boundary) cannot make this promise once
 * the centres it would need to sit below are free to move independently.
 *
 * THE SAMPLE IS THE RAW TOUCH, BEFORE ANY BIAS - unchanged. tables_gesture_
 * tick() below stores s->rawX/s->rawY from f->touchX/f->touchY every tick a
 * finger is down, independent of numpad_hit()'s (or numpad_hit_calibrated()'s)
 * own cell resolution, which only ever decides what HIGHLIGHTS. Verify by
 * reading tables_calib_on_commit(): dx/dy come from `rawX`/`rawY` as handed
 * to it by tables_gesture_tick(), never from s->hoverCell.
 *
 * THE COMMIT PATH is unchanged - see that heading, further down this
 * section originally, now folded into this same comment for locality:
 * tables_gesture_tick() is the SAME function ordinary gameplay uses to turn
 * a press into a digit, so the point calibration records is provably what
 * the real game would have recorded for the same press.
 *
 * THE GESTURE THAT OPENS/ABORTS IT is unchanged: PWR double-press, checked
 * before the phase dispatch so it works from any screen calibration might
 * interrupt; a second double-press mid-pass aborts outright, saving
 * nothing (tables_calib_pwr_double_press()/tables_calib_abort() below).
 *
 * THE TUNABLE VS. A STORED CALIBRATION is unchanged in SHAPE
 * (tables_gesture_tick()'s own precedence check, below) though what "a
 * stored calibration" means changed from a line to nine centres - see that
 * function's own comment. Still worth repeating why g_tuneThumbBiasMoved
 * has to exist at all: TABLES_LIVE_TUNE is compiled into every emulator
 * build (the devlink tuning registry serves sketch.c/clock.c/tables.c from
 * one binary), so "the gate is on" is true on every automated run
 * including this mode's own test, and without that flag no test could ever
 * observe a calibration actually being used.
 * ========================================================================= */
#define CALIB_KEYS          9   // digits 1..9; CELL_ZERO is never one of them
#define CALIB_TAPS_PER_KEY 10   // the owner's own number, not reduced
#define CALIB_SEQ_LEN (CALIB_KEYS * CALIB_TAPS_PER_KEY) // 90

// The rotated prompt order (see ROTATED, NOT BLOCKED above): digit
// 1,2,...,9 repeating, so idx%CALIB_KEYS is the digit-minus-one and
// idx/CALIB_KEYS is which of its ten taps this is (0..9). No lookup table
// needed - the whole point is that the order is a simple, predictable
// rotation rather than a hand-picked sequence.
static int calib_seq_digit(int idx) { return (idx % CALIB_KEYS) + 1; }
static int calib_seq_tap(int idx)   { return idx / CALIB_KEYS; }

#define CALIB_DOUBLE_PRESS_WINDOW_MS 500u // clock.c's own DOUBLE_PRESS_WINDOW_MS, same reasoning

/* ---- STORAGE, AND THE TORN-WRITE ARGUMENT --------------------------------
 *
 * ONLY DY IS STORED - nine of them, one per key, each the learned centre's
 * offset from that key's own drawn cell_rect() centre (so loading needs
 * only re-add a stored dy to a target this file already knows how to
 * compute, never a stored x at all - see PER KEY, THE LEARNED CENTRE above
 * for why x is measured and printed but not kept).
 *
 * NINE VALUES DO NOT FIT ONE RECORD. storage.h's record carries one
 * uint32_t `value`; the old model's two floats fit in one (PACKING, this
 * section's own previous version). Nine dy's do not: measured dy sits
 * roughly 20..70px (this app's own report), so each is packed as a signed
 * 7-bit value of (dy - CALIB_DY_OFFSET) - 45 chosen as the offset because it
 * centres that 20..70 range on zero, covering -25..+25 with the 7-bit
 * field's own -64..+63 range to spare for a run that lands outside the
 * range this file has actually seen. 9 x 7 = 63 bits: two uint32 words (LO
 * = bits 0..31, HI = bits 32..62 - see tables_calib_pack9()/unpack9()),
 * i.e. TWO storage records, STORAGE_KIND_TABLES_CALIB_LO and _HI
 * (storage.h).
 *
 * A TORN WRITE - power lost between the LO save and the HI save - MUST be
 * detected and MUST fall back to the uncalibrated default, never apply
 * half a pass. storage.c appends records one at a time; there is no atomic
 * "write two records together" primitive, and there should not be one just
 * for this caller (storage.h is deliberately not a transactional store).
 * THE SCHEME: both records of one calibration pass are saved with the SAME
 * small tag, stored in the record's own `reserved` byte (offset 3 - already
 * covered by the CRC, storage.c's own record layout comment; using it
 * changes no on-flash shape, only what a byte that used to always be 0
 * holds) via storage_save_u32_tagged()/storage_get_u32_tagged()
 * (storage.h). A read (tables_enter() below) accepts the pair ONLY if BOTH
 * records are present AND their tags match:
 *
 *   - if LO is missing, HI is missing, or the read simply never ran a
 *     calibration pass before - no pair, calibHave stays false, the
 *     uncalibrated default is used. This is also the ordinary "never
 *     calibrated" case, so it needs no separate message.
 *   - if LO is present and HI is not (or the reverse) - power was lost
 *     between the two saves, or storage was disabled for part of a boot
 *     that spanned two calibration passes (see storage.c's own new
 *     diagnostic for that path). calibHave stays false: a HALF-applied
 *     calibration - eight keys with fresh data, one still carrying
 *     whatever a stale HI or LO happens to hold - is worse than none, since
 *     it silently mixes two different passes' geometry.
 *   - if BOTH are present but their tags DIFFER - the more dangerous case,
 *     because it is not simply "one record missing": a torn write between
 *     two DIFFERENT calibration passes can leave a fresh LO sitting next to
 *     a STALE HI (or vice versa) that is individually well-formed (valid
 *     magic, valid CRC, a plausible-looking value) and would silently pass
 *     any check that only asks "does a record exist for this kind". Tags
 *     catch exactly this: tables_calib_finish() below reads whichever of
 *     LO/HI is currently stored (if either) and saves the NEW pass at
 *     old_tag + 1 (wrapping in the tag byte's own 8 bits), so a genuinely
 *     torn write - new LO, old HI - carries two DIFFERENT tags and is
 *     rejected, where a fixed tag (e.g. always 0) would have silently
 *     accepted it. calibHave stays false; the uncalibrated default is used
 *     until a complete pass lands.
 *
 * Worth naming why incrementing (rather than a fixed constant) matters:
 * with a FIXED tag, pass 2 writing LO(tag0, dataB) and then losing power
 * before HI(tag0, dataB) would leave the sector holding LO(tag0, dataB) next
 * to HI(tag0, dataA) from pass 1 - same tag, different passes, and a
 * same-tag check would wrongly accept the mismatched pair. Incrementing
 * makes pass 2's LO carry a tag pass 1's HI cannot also carry, so the same
 * scenario is caught. A single stray collision (the tag byte wrapping back
 * to a value an old stale record happens to share) is not ruled out in
 * principle, but at 256 possible tags and a mode a child never touches and
 * an owner runs on the order of once per session, it is not a real risk
 * worth spending more of the record's own bytes on.
 */
#define CALIB_DY_OFFSET     45   // centres the measured 20..70px range on zero
#define CALIB_DY_FIELD_MIN (-64) // signed 7-bit range: -64..63
#define CALIB_DY_FIELD_MAX   63
#define CALIB_DY_FIELD_BITS   7
#define CALIB_DY_FIELD_MASK 0x7Fu

static void tables_calib_pack9(const int dy[CALIB_KEYS], uint32_t *outLo, uint32_t *outHi) {
    uint64_t packed = 0;
    for (int i = 0; i < CALIB_KEYS; i++) {
        int32_t v = dy[i] - CALIB_DY_OFFSET;
        bool clipped = v < CALIB_DY_FIELD_MIN || v > CALIB_DY_FIELD_MAX;
        if (v < CALIB_DY_FIELD_MIN) v = CALIB_DY_FIELD_MIN;
        if (v > CALIB_DY_FIELD_MAX) v = CALIB_DY_FIELD_MAX;
        if (clipped) {
            printf("tables: calib WARNING - key %d's learned dy exceeded the packed range and was clamped\r\n", i + 1);
        }
        uint64_t field = (uint64_t)((uint32_t)v & CALIB_DY_FIELD_MASK);
        packed |= field << (CALIB_DY_FIELD_BITS * i);
    }
    *outLo = (uint32_t)(packed & 0xFFFFFFFFu);
    *outHi = (uint32_t)((packed >> 32) & 0xFFFFFFFFu);
}

static void tables_calib_unpack9(uint32_t lo, uint32_t hi, int outDy[CALIB_KEYS]) {
    uint64_t packed = ((uint64_t)hi << 32) | (uint64_t)lo;
    for (int i = 0; i < CALIB_KEYS; i++) {
        uint32_t field = (uint32_t)((packed >> (CALIB_DY_FIELD_BITS * i)) & CALIB_DY_FIELD_MASK);
        int32_t v = (int32_t)field;
        if (field & 0x40u) v -= 0x80; // sign-extend the 7-bit two's-complement field
        outDy[i] = v + CALIB_DY_OFFSET;
    }
}

// NEAREST LEARNED CENTRE (see this section's own header). `dy` is the nine
// learned per-key offsets (tables_state_t's own calibCentreDy, indexed the
// same way cell_rect()'s cell 0..8 already is - digit i+1 lives at index
// i). Only ever called once calibHave is true (tables_gesture_tick() below
// gates on that), so this never has to fall back to anything itself.
//
// THE TWO GATES, both unbiased (raw lx/ly, no TOUCH_THUMB_BIAS_Y anywhere
// in this function - the learned centres already sit where he touches, so
// adding a bias on top would double-correct):
//
//   - TOP: ly < NUMPAD_Y0 cancels, same as numpad_hit()'s own top gate.
//     Nearest-centre alone would still assign a touch well above the pad
//     (the loupe zone, the question band) to whichever digit key happens
//     to be closest, which is wrong - that space is not the numpad.
//   - BOTTOM (the zero key): CELL_ZERO is not one of the nine learned
//     keys, so it cannot win a nearest-centre comparison on its own. A
//     touch at or below `zeroGateY` - a half key beneath the LOWEST of the
//     nine learned centres, whichever key that is - is the zero key
//     outright, checked BEFORE nearest-centre runs. Anchoring to the
//     learned centres (not to NUMPAD_Y0 + 3*CELL_H, the drawn grid's own
//     row-3 boundary) is what keeps this promise even when his taps pull
//     the bottom digit row's centres well past that fixed line: measured
//     dy runs up to 70px against a CELL_H of 57, so a learned centre can
//     already sit past the drawn grid's own next row boundary, and a fixed
//     gate would then hand some of the zero key's own territory to
//     whichever digit centre moved into it. zeroOuterY reuses
//     TOUCH_THUMB_BIAS_Y_DEFAULT's own 40px as how far below the pad's
//     drawn bottom edge a touch may still reach and count - not a
//     calibrated number (the zero key is not calibrated at all), just the
//     same "how far below is still a legitimate reach" this app already
//     shipped with, kept as-is rather than invented fresh for one gate.
static int numpad_hit_calibrated(int lx, int ly, const int dy[CALIB_KEYS]) {
    if (lx < NUMPAD_X0 || lx >= NUMPAD_X0 + NUMPAD_W) return -1;
    if (ly < NUMPAD_Y0) return -1;

    int centreX[CALIB_KEYS], centreY[CALIB_KEYS];
    int maxCentreY = 0;
    for (int i = 0; i < CALIB_KEYS; i++) {
        int bx, by, bw, bh;
        cell_rect(i, &bx, &by, &bw, &bh); // cell i (0..8) IS digit i+1's cell
        centreX[i] = bx + bw / 2;
        centreY[i] = by + bh / 2 + dy[i];
        if (i == 0 || centreY[i] > maxCentreY) maxCentreY = centreY[i];
    }

    int zeroGateY = maxCentreY + CELL_H / 2;
    int zeroOuterY = NUMPAD_Y0 + NUMPAD_H + (int)TOUCH_THUMB_BIAS_Y_DEFAULT;
    if (ly >= zeroGateY) {
        return ly < zeroOuterY ? CELL_ZERO : -1;
    }

    int best = 0, bestDistSq = -1;
    for (int i = 0; i < CALIB_KEYS; i++) {
        int ddx = lx - centreX[i], ddy = ly - centreY[i];
        int distSq = ddx * ddx + ddy * ddy;
        if (bestDistSq < 0 || distSq < bestDistSq) { bestDistSq = distSq; best = i; }
    }
    return best; // cell i IS digit i+1's cell - see cell_digit_value()
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
    int      calibSeqIdx;                             // 0..CALIB_SEQ_LEN (90); next prompt / samples landed so far
    int      calibDy[CALIB_KEYS][CALIB_TAPS_PER_KEY];  // raw (touchY - prompted key's drawn centre Y), pre-bias, per key per tap
    int      calibDx[CALIB_KEYS][CALIB_TAPS_PER_KEY];  // raw dx, per key per tap - printed only, never used for classification

    // A private double-press detector for the PWR gesture - clock.c's
    // pwr_double_press() idiom, its own copy of the bookkeeping (see that
    // function's own comment for why a shared one is not worth it here).
    bool     calibHavePendingShort;
    uint32_t calibPendingShortMs;

    // The stored calibration THIS RUN is using (loaded once, on enter() -
    // see tables_gesture_tick()'s own precedence check). Independent of
    // calibActive: these are the nine learned centres numpad_hit_calibrated()
    // reads on every ordinary touch, not just while a pass is in progress.
    bool     calibHave;
    int      calibCentreDy[CALIB_KEYS]; // one learned dy per digit 1..9 (index = digit-1)
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
// default check below. This now decides between the UNCALIBRATED rectangle
// path (numpad_hit(), with either the tunable's live value or
// TOUCH_THUMB_BIAS_Y_DEFAULT) and the CALIBRATED nearest-centre path
// (numpad_hit_calibrated()) - tables_gesture_tick() below is the one call
// site, since that is the only place a bias value or a calibration is
// actually consumed.
static bool tables_tunable_moved(void) {
#if TABLES_LIVE_TUNE
    return g_tuneThumbBiasMoved;
#else
    return false;
#endif
}

static float tables_uncalibrated_bias(void) {
#if TABLES_LIVE_TUNE
    if (g_tuneThumbBiasMoved) return TOUCH_THUMB_BIAS_Y;
#endif
    return TOUCH_THUMB_BIAS_Y_DEFAULT;
}

// Insertion-sort median of n (n<=CALIB_TAPS_PER_KEY) - cheap, no qsort
// needed (this build has no <stdlib.h> qsort to call anyway). The MEDIAN,
// not the mean - see this file's CALIBRATION header (PER KEY, THE LEARNED
// CENTRE) for why a mean would be dragged by one bad tap. Called once per
// key, on that key's own ten taps.
static int tables_calib_median_n(const int *v, int n) {
    int a[CALIB_TAPS_PER_KEY];
    for (int i = 0; i < n; i++) a[i] = v[i];
    for (int i = 1; i < n; i++) {
        int key = a[i], j = i - 1;
        while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; j--; }
        a[j + 1] = key;
    }
    if (n % 2 == 1) return a[n / 2];
    return tables_round_i32(((float)a[n / 2 - 1] + (float)a[n / 2]) / 2.0f);
}

/* ---- PROGRESS DISPLAY - nine meters, not nine dots -----------------------
 *
 * A NINE-DOT ROW WAS ENOUGH FOR NINE SAMPLES; IT IS NOT ENOUGH FOR NINETY.
 * The old design's own dots (one per SAMPLE, filled once that sample
 * landed) worked because there were exactly as many dots as samples in the
 * whole pass. At 90 samples that stops being legible - ninety dots either
 * shrink below anything a finger-sized panel can resolve or spill off the
 * 348px of usable width - and it stops answering the question that
 * actually matters mid-pass, which is not "how many samples total" but
 * "which key am I on, and how far through ITS ten am I".
 *
 * ONE METER PER KEY, LEFT TO RIGHT IN DIGIT ORDER (1..9), each a vertical
 * bar that fills from empty to full as that key's own ten taps land -
 * CALIB_METER_W wide, CALIB_METER_H tall, spaced CALIB_METER_GAP apart, the
 * same 240px span across 348 the old nine dots used, just turned from a
 * row of instants into a row of fill levels. A key with zero taps so far
 * reads as an empty outline; a key with all ten reads as a solid bar;
 * anything between reads as a fraction, at a glance, without reading a
 * number - the same "answer legible without a number" property the rest of
 * this app's UI already leans on (the loupe, the right/wrong washes).
 *
 * THE CURRENT KEY gets a small marker dot above its own meter - not a
 * colour change (this device's colour is reserved for right/wrong, see THE
 * COUNTER PILLS elsewhere in this file) and not required to find it (the
 * big top-band digit already names it, unchanged from every earlier
 * version of this mode), but a marker ties "the digit I am being asked
 * for" directly to "the bar that is about to move" without a saccade
 * between the top of the screen and this row - useful precisely because
 * the top digit and this row are far apart on a 448px-tall portrait panel.
 *
 * STILL UNDER THE PAD, NOT IN THE LOUPE'S BAND - unchanged reasoning from
 * the old design: the counters' band below the pad is free for the whole
 * pass (calibration draws no pills there), the loupe never reaches it, and
 * the real loupe bubble would paint straight over anything left in its own
 * zone the instant a finger is down, which is exactly what made the old
 * nine-dot version read as frozen before it moved down here.
 */
#define CALIB_METER_W       14
#define CALIB_METER_H       40
#define CALIB_METER_GAP     30
#define CALIB_METER_MARK_R  4.0f
#define CALIB_METER_MARK_GAP 2

// How many of key `key`'s (0..8) ten taps have already landed, given the
// rotated sequence position `seqIdx` (0..CALIB_SEQ_LEN, calib_seq_digit()/
// calib_seq_tap() above) - i.e. how many FULL rotations have completed,
// plus one more for a key whose own slot in the CURRENT partial rotation
// has already been visited. Pure arithmetic on the rotation, not a scan
// over calibDy, because the rotation is regular enough that a scan would
// just be recomputing this.
static int calib_taps_done(int key, int seqIdx) {
    int fullRounds = seqIdx / CALIB_KEYS;
    int posInRound = seqIdx % CALIB_KEYS;
    return key < posInRound ? fullRounds + 1 : fullRounds;
}

static void tables_calib_draw_progress(tables_state_t *s) {
    int totalW = (CALIB_KEYS - 1) * CALIB_METER_GAP;
    int x0 = PANEL_W / 2 - totalW / 2;

    int contentH = (int)(CALIB_METER_MARK_R * 2.0f) + CALIB_METER_MARK_GAP + CALIB_METER_H;
    int topMargin = (COUNTERS_H - contentH) / 2;
    int markCy = COUNTERS_Y0 + topMargin + (int)CALIB_METER_MARK_R;
    int meterY0 = COUNTERS_Y0 + topMargin + (int)(CALIB_METER_MARK_R * 2.0f) + CALIB_METER_MARK_GAP;

    int currentKey = calib_seq_digit(s->calibSeqIdx) - 1;
    for (int key = 0; key < CALIB_KEYS; key++) {
        int cx = x0 + key * CALIB_METER_GAP;
        int mx = cx - CALIB_METER_W / 2;

        // Empty outline (light track) for the whole meter, generation-1
        // flat fill - same technique THE COUNTER PILLS elsewhere in this
        // file uses and for the same reason (this is chrome, not ink; the
        // AA float brush converts colour to grey, which is fine for black
        // ink but not for telling "track" from "fill" apart at a glance).
        gfx_fill_rect(mx, meterY0, CALIB_METER_W, CALIB_METER_H, loupe_bubble_color());

        int done = calib_taps_done(key, s->calibSeqIdx);
        int fillH = (CALIB_METER_H * done) / CALIB_TAPS_PER_KEY;
        if (fillH > 0) {
            gfx_fill_rect(mx, meterY0 + (CALIB_METER_H - fillH), CALIB_METER_W, fillH, PX_BLACK);
        }

        if (key == currentKey) {
            shapes_fill_disc_aa((float)cx, (float)markCy, CALIB_METER_MARK_R, PX_BLACK);
        }
    }
}

// A full repaint: the prompted digit (top band, where the question
// normally sits), the nine per-key progress meters (counters' band) and
// the numpad itself, drawn exactly as draw_numpad_all() draws it for
// ordinary gameplay - see this section's own header for why the pad has to
// be the real one rather than a stand-in target. Called only on entry and
// after each sample lands - up to 90 times over a whole pass, not per
// tick, so a whole-panel clear-and-push (gfx_push_all(), the same "rare,
// big transition" cost clock.c's own paint_all() accepts for set mode) is
// the honest cost here.
static void tables_calib_draw_screen(tables_state_t *s) {
    gfx_fill_rect(0, 0, PANEL_W, PANEL_H, PX_WHITE);
    draw_number_lr(PANEL_W / 2, QROW_CY, QDIGIT_W, QDIGIT_H, QDIGIT_T, calib_seq_digit(s->calibSeqIdx), false, PX_BLACK);
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

// All 90 samples done. Prints, per key 1..9, the ten raw dy's min/max/
// spread (the noise floor of THAT key's own repeats - the six-5s idea
// generalised to every key now that every key gets repeats) and the
// median dy AND median dx (dx printed only - see this section's own header,
// ONLY DY IS STORED), THEN packs and saves the nine learned dy's - the
// tagged two-record write STORAGE, AND THE TORN-WRITE ARGUMENT above
// describes - THEN loads them straight into s->calibCentreDy so the pass
// that just finished takes effect immediately, no reboot needed.
static void tables_calib_finish(tables_state_t *s) {
    int learnedDy[CALIB_KEYS];
    for (int key = 0; key < CALIB_KEYS; key++) {
        const int *dy = s->calibDy[key];
        const int *dx = s->calibDx[key];
        int dyMin = dy[0], dyMax = dy[0];
        for (int t = 1; t < CALIB_TAPS_PER_KEY; t++) {
            if (dy[t] < dyMin) dyMin = dy[t];
            if (dy[t] > dyMax) dyMax = dy[t];
        }
        int dyMedian = tables_calib_median_n(dy, CALIB_TAPS_PER_KEY);
        int dxMedian = tables_calib_median_n(dx, CALIB_TAPS_PER_KEY);
        learnedDy[key] = dyMedian;
        printf("tables: calib key %d: dy min=%d max=%d spread=%d median=%d px | dx median=%d px (printed only, not stored)\r\n",
               key + 1, dyMin, dyMax, dyMax - dyMin, dyMedian, dxMedian);
    }

    uint32_t lo, hi;
    tables_calib_pack9(learnedDy, &lo, &hi);

    // The generation tag: whatever tag is currently stored (LO if present,
    // else HI, else this is the first pass ever) plus one - see STORAGE,
    // AND THE TORN-WRITE ARGUMENT above for why incrementing (not a fixed
    // tag) is what makes a torn write between two DIFFERENT passes
    // detectable rather than just a torn write within one.
    uint32_t prevValue; uint8_t prevTag = 0;
    if (!storage_get_u32_tagged(STORAGE_KIND_TABLES_CALIB_LO, &prevValue, &prevTag)) {
        storage_get_u32_tagged(STORAGE_KIND_TABLES_CALIB_HI, &prevValue, &prevTag);
    }
    uint8_t tag = (uint8_t)(prevTag + 1);

    storage_save_u32_tagged(STORAGE_KIND_TABLES_CALIB_LO, lo, tag);
    storage_save_u32_tagged(STORAGE_KIND_TABLES_CALIB_HI, hi, tag);
    printf("tables: calibration saved (tag=%d)\r\n", (int)tag);

    for (int key = 0; key < CALIB_KEYS; key++) s->calibCentreDy[key] = learnedDy[key];
    s->calibHave = true;

    s->calibActive = false;
    tables_calib_redraw_practice_screen(s);
}

// One commit landed during calibration - see this section's own header
// (THE COMMIT PATH) for why this is provably the same commit ordinary
// gameplay would have recorded, and (PER KEY, THE LEARNED CENTRE / THE
// SAMPLE IS THE RAW TOUCH) for why the delta below is computed from
// `rawX`/`rawY` - tables_gesture_tick()'s own pre-bias reading - against
// cell_rect() rather than against `hitCell`'s touch zone. `hitCell` is only
// ever logged here (NO REJECTION, EVER above): it never decides whether
// this sample counts.
static void tables_calib_on_commit(tables_state_t *s, int rawX, int rawY, int hitCell) {
    int idx = s->calibSeqIdx;
    int digit = calib_seq_digit(idx);
    int key = digit - 1;
    int tap = calib_seq_tap(idx);
    int bx, by, bw, bh;
    cell_rect(key, &bx, &by, &bw, &bh); // cell `key` IS digit `digit`'s cell
    int targetX = bx + bw / 2, targetY = by + bh / 2;
    int dx = rawX - targetX, dy = rawY - targetY;
    s->calibDx[key][tap] = dx;
    s->calibDy[key][tap] = dy;
    printf("tables: calib %d/%d prompted=%d tap=%d/%d target=(%d,%d) raw=(%d,%d) dx=%d dy=%d px landed=%d\r\n",
           idx + 1, CALIB_SEQ_LEN, digit, tap + 1, CALIB_TAPS_PER_KEY, targetX, targetY, rawX, rawY, dx, dy,
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

    // Accept the pair ONLY if both LO and HI are present and carry the SAME
    // tag - see STORAGE, AND THE TORN-WRITE ARGUMENT (above numpad_hit_
    // calibrated()) for why a tag mismatch (or either record simply
    // missing) means "do not trust this, fall back to the uncalibrated
    // default" rather than applying whichever half did land.
    uint32_t lo = 0, hi = 0; uint8_t loTag = 0, hiTag = 0;
    bool haveLo = storage_get_u32_tagged(STORAGE_KIND_TABLES_CALIB_LO, &lo, &loTag);
    bool haveHi = storage_get_u32_tagged(STORAGE_KIND_TABLES_CALIB_HI, &hi, &hiTag);
    if (haveLo && haveHi && loTag == hiTag) {
        int learnedDy[CALIB_KEYS];
        tables_calib_unpack9(lo, hi, learnedDy);
        for (int key = 0; key < CALIB_KEYS; key++) s->calibCentreDy[key] = learnedDy[key];
        s->calibHave = true;
        printf("tables: loaded stored calibration (tag=%d)\r\n", (int)loTag);
    } else if (haveLo || haveHi) {
        // One record without its matching other - a torn write between two
        // saves, or between two DIFFERENT passes (tag mismatch). Either
        // way this is exactly the case that must NOT be half-applied - see
        // this file's own torn-write argument for why a mismatched or
        // incomplete pair is worse than none at all.
        s->calibHave = false;
        printf("tables: stored calibration was incomplete (a torn write?) - using the default bias\r\n");
    } else {
        s->calibHave = false;
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
            // CALIBRATED (nearest of nine learned centres) wins whenever a
            // calibration is loaded, UNLESS the live-tune slider has
            // actually been moved this session - see THE TUNABLE VS. A
            // STORED CALIBRATION above tables_tunable_moved()/
            // tables_uncalibrated_bias() for why a merely-compiled-in
            // tunable must not itself win.
            int cell = (!tables_tunable_moved() && s->calibHave)
                ? numpad_hit_calibrated(lx, ly, s->calibCentreDy)
                : numpad_hit(lx, ly, tables_uncalibrated_bias());
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
