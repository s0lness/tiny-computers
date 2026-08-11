# 0001: Pad every pushed window to 64 pixels wide

Date: 2026-08-11
Status: accepted, cause not yet bisected

## The symptom

Strokes on the sketchpad came out broken. The description changed as the app
improved, but it was one defect throughout: first "dead pixels, the strokes
break down into sort of multicolour things", later "smudging", finally
"glitches when drawing vertically".

The last description was the useful one, and it came with photographs.

## What made it diagnosable

A single stroke drawn as a U: the curved bottom was clean, both vertical sides
were shredded into a ladder of horizontal ticks. Same stroke, same code, same
second. Two vertical strokes elsewhere in the same session, both shredded.

The artifact tracked the **direction** of the stroke.

That is what ruled out everything upstream of the display. Touch sampling,
stroke smoothing, pressure simulation and the rasteriser have no notion of
direction; a line is a sequence of segments regardless of orientation. And the
firmware's own instrumentation agreed: over a 60 second session that included
the broken strokes it logged zero stroke splits, zero rejected samples and zero
stray contacts, with 33 stroke starts matched by exactly 33 ends. The firmware
believed it had drawn continuous strokes. So the loss was downstream, between
the framebuffer and the panel.

## The mechanism

`AMOLED_1IN8_DisplayWindows` issues **one DMA transfer per row** of the
window. Therefore the window's *width* is the size of each transfer and its
*height* is the number of transfers.

| Stroke | Dirty rect | Consequence |
|---|---|---|
| horizontal | wide, short | few large transfers |
| vertical | narrow, tall | hundreds of tiny transfers |
| full refresh | full width | fewest, largest transfers |

Every observation lines up with that table, including the one that had been
sitting unexplained for hours: full-screen refreshes always looked perfect,
which had been read as "the panel is fine, so the problem must be in the pixel
data", when it actually meant "the widest possible window is the safest one".

## The change

`push_dirty()` now aligns each window to 8 pixels and pads it to at least 64
wide (`PUSH_MIN_W`). The narrowest vertical stroke therefore becomes a 128
byte per row transfer instead of a few dozen.

Cost: a little more data per push. A vertical stroke that dirtied a 12 pixel
wide column now sends 64. Pushes were measured at 27 to 90 microseconds
against a frame budget of roughly 16 milliseconds, so the headroom is ample.

Confirmed on hardware: verticals draw clean.

## Bisect round one

`push_bisect_test()` drew the same vertical stroke four times, one per policy:

| Column | Policy | Result |
|---|---|---|
| 1 | align 2, no minimum | **shredded** |
| 2 | align 8, no minimum | clean |
| 3 | align 2, minimum 64 | clean |
| 4 | align 8, minimum 64 | clean |

Only the first shredded, so either change fixes it independently. That kills
the simplest reading of the mechanism. Columns 1 and 2 push windows of very
similar *width* (8 to 16 pixels), so narrowness alone cannot be the trigger.

What separates them is that column 1's widths land on 10, 12, 14 while every
clean column has a width that is a multiple of 8. Working hypothesis: **each
row's length must be a multiple of 8 pixels (16 bytes)**, and neither the
window's starting column nor its absolute width matters.

If that holds, the shipped fix was right for the wrong reason and more
expensive than it needed to be: the 64 pixel minimum was doing nothing that
rounding the width to 8 did not already do. `PUSH_MIN_W` is therefore 8 and
`PUSH_GRAN_W` is 8, which pushes far less data per stroke.

## Bisect round two, and the answer

| Column | Policy | Result |
|---|---|---|
| 1 | untouched (start aligned 2, length aligned 2) | **shredded** |
| 2 | start aligned 2, **length rounded to 8** | clean |
| 3 | start aligned 8, length rounded to 8 | clean |
| 4 | as 3, plus 64 minimum width | clean |

Column 2 settles it. An unaligned start with a rounded row length is clean, so:

> **Each row of a pushed window must be a multiple of 8 pixels (16 bytes)
> long. Where the window starts does not matter, and neither does how wide it
> is beyond that.**

`push_dirty()` is now exactly that and nothing more: start aligned to 2 as
before, row length rounded up to 8. The 8 pixel start alignment and the 64
pixel minimum have both been removed as unnecessary. When a rounded window
would run off the right edge it is slid left rather than clipped, because
shortening the row is the very thing that corrupts it, and the panel's 368
pixel width is itself a multiple of 8 so sliding stays valid.

## Mechanism, still not fully established

A width that is a multiple of 8 pixels is 16 bytes. The obvious theory is that
each row's transfer must be a whole number of 32-bit words, four of them, and
that rows of 5 or 6 words corrupt.

The configuration was then read rather than assumed, and it **weakens** that
theory. `DEV_Config.c` sets the DMA to `DMA_SIZE_8`, and the transfer count is
`(Xend - Xstart) * 2`, so the DMA moves one byte per transfer and has no notion
of words at all. The PIO side is `sm_config_set_out_shift(&c, false, true, 8)`,
autopull with an 8 bit threshold, so it consumes one byte per FIFO entry.
Neither side has an obvious reason to care whether the row is 20 bytes or 32.

Which leaves the panel, or the QSPI framing, as the more likely home of the
constraint. 8 pixels at 16 bits is 32 nibbles on a 4 bit bus, and a requirement
expressed in whole bursts of that size would fit the evidence. This has not
been confirmed against the SH8601 datasheet.

So the upstream report should carry the bisect table as the evidence, state the
rule as measured, and present the mechanism as open. It is worth filing either
way: a driver whose partial-refresh path silently corrupts output for most
window widths is a bug regardless of which layer imposes the limit, and this is
the second defect found in this same function after the off-by-one that dropped
the bottom row of every partial refresh.

## Why the earlier fix worked

The 64 pixel minimum happened to make every width a multiple of 8, so it fixed
the bug without touching its cause, at roughly eight times the data per push.

## What is still unknown, and why that matters

**Two things were changed at once**, the alignment and the minimum width, so
which one actually fixes it is not yet established. The plausible mechanisms
differ:

- a per-row DMA re-arm race that only bites when the transfer is short enough
  to complete inside the re-arm window, which would implicate width;
- a panel or PIO alignment requirement on the column address or the row byte
  count, which would implicate alignment.

`push_bisect_test()`, bound to a long press on PWR, draws the same vertical
stroke four times side by side under align-2/no-minimum, align-8/no-minimum,
align-2/min-64 and align-8/min-64. Whichever columns shred name the cause.

This matters because the bug is almost certainly in Waveshare's driver and
should be reported upstream, and a report that says "pad your windows and it
goes away" without a mechanism is not worth filing. It is also the second bug
found in this same function, after an off-by-one that dropped the bottom row of
every partial refresh.

## What this cost, and the lesson

Two earlier explanations were wrong, and both produced real fixes for real bugs
that were not this one: coordinate tearing between the separate X and Y register
reads, and a bridging rule that joined a lift-and-retouch with a line across the
screen. Keeping them is correct. Believing they were the answer was not.

The mistake was reasoning from a plausible mechanism toward the symptom. What
worked was the opposite: take a property of the artifact that the suspected
subsystem cannot possibly explain, and let it eliminate whole layers at once.
Photographs and a screen recording supplied that property; serial logs alone
never would have, because the firmware's logs were correct and reassuring
throughout.
