# Lucide (vendored)

Source: https://github.com/lucide-icons/lucide, commit `a7c781bd43dbf295a4c2ab07d25d544dd7879bf9`
(`main`, 2026-08-10). ISC licensed - see `LICENSE` in this directory, kept
verbatim as the licence requires.

Only the icons actually used or seriously considered as a candidate for one
of this device's three menu icons are vendored here, not the whole Lucide
set - `icons/*.svg`, untouched from upstream (`width`/`height`/`viewBox="0 0
24 24"`, `stroke-width="2"`, round caps and joins).

| File | Candidate for | Picked? |
|---|---|---|
| `timer.svg` | chrono (the stopwatch) | yes, sole candidate |
| `pencil.svg` | sketch (the drawing pad) | rendered, owner to choose |
| `pencil-line.svg` | sketch | rendered, owner to choose |
| `hourglass.svg` | timer (the countdown) | rendered, owner to choose |
| `loader-circle.svg` | timer, as the coil's stand-in | rendered, owner to choose |

`tools/lucide-convert.ts` reads these files directly (fetched at design time,
not at build time) and flattens each `<line>`/`<circle>`/`<path>` into the
point data `tools/gen-lucide-menu-icons.ts` bakes into
`firmware/apps/menu.c`. See that file's own header comment for why a
stopwatch-shaped icon is literally named `timer.svg` upstream while this
device's own `timer` app (the countdown) ended up drawn from `hourglass.svg`
or `loader-circle.svg` instead - the match is by silhouette, not by name.
