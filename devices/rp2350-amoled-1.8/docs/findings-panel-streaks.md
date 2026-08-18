# Streaks on the panel: three software fixes that could never have worked

Date: 2026-08-18
Method: photographs of the glass, taken by the owner's hand, with the expected
outcome written down BEFORE each one. Nothing in this repository can observe
this defect, so nothing in this repository could have found it.

## The symptom

With the sketchpad's colour palette open, every colour cell trailed thin
streaks of its own colour into the white gutter beside it, running along the
panel's rows. In the first photograph one cell was also cut off partway
across. Photographed twice, hours apart, on different firmware.

## What it was

**Signal integrity on the QSPI wire at 75MHz.** `qspi.pio` ran both PIO
programs at `clkdiv 1.0`, the value the vendor driver shipped with. Halving it
to 2.0, changing nothing else, and photographing again: no streaks.

Cost, measured on the board with the same app either side: **158k main-loop
iterations/second at 1.0 against 122k at 2.0, about 22% fewer**. The loop is
not dominated by the panel push, so a clean picture is cheap.

## The three software explanations that died, and how

This is the useful part. Each was reasonable, each was pursued properly, and
each was killed by a measurement rather than by an argument.

**1. The palette's push geometry.** Decision 0001 established that a pushed
window's row length must be a multiple of 8 or the panel corrupts the
transfer, so a palette cell violating it was the obvious first suspect. Traced
by hand for every cell including the column nearest the panel edge, then
driven through the real compiled firmware in the emulator: the whole pop-in
frame by frame, drags across every row, and the project's own calibrated
dropout+jitter profile. **Zero violations, and full coverage of every pixel
drawn.** A genuine negative result: the geometry was provably sound.

**2. Push frequency.** With the geometry cleared, the next candidate was
decision 0001's own unconfirmed note about "a per-row DMA re-arm race that
only bites when the transfer is short enough". A throttle was added so two
different-geometry pushes could not fire back to back. **The photograph after
it was unchanged.**

**3. The framebuffer itself.** The decisive one. The device's framebuffer was
captured over devlink WHILE the owner held the palette open on the glass, so
both instruments looked at the same instant. **The gutters were uniformly
white.** Correct in memory, striped on the panel. That is what finally
pointed past the software entirely, and it should have been the first
measurement rather than the fourth.

A fourth change was made in passing - a shift-out fence between rows inside
`AMOLED_1IN8_DisplayWindows` - which also did not move the symptom. It was
kept, because the gap it closes is real on its own terms, but its comment now
says plainly that nothing observed on this board has ever been traced to it.

## The method, which is worth more than the answer

**Write down what each outcome will look like before you look.** Every
photograph in this investigation was preceded by two sentences: what we would
see if the hypothesis held, and what we would see if it did not. Without that,
"is it better?" against a fuzzy phone photo of thin coloured lines is a
question that can be answered yes three times in a row while nothing improves.

**Ask which instrument can see the defect at all, first.** The emulator has no
panel and models no timing (decision 0003). It can prove a framebuffer
correct, and it did, repeatedly and expensively, on a bug that was never in
the framebuffer. Three careful software fixes and several hours went into a
layer that had already been shown to be innocent.

**Compare the two instruments at the same instant.** Framebuffer-versus-glass,
captured while the owner's finger held the state open, converted an open
question into a settled one in a single measurement.

## If you touch that clock again

`clkdiv` in `firmware/lib/QSPI_PIO/qspi.pio` (and its generated `qspi.pio.h`;
both are in the tree) is the one number to move if a future board revision
changes the electrical picture. **The test is a photograph of the palette, not
a passing test suite.** The gate, the invariants and all 34 test files were
green throughout the entire period this defect was on the glass.
