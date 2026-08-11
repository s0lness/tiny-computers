# tiny-computers

Multi-device repo for pocket-sized touchscreen boards. **The real instructions
live per device**, in `devices/<board>/AGENTS.md`. Read that one, not this one,
before touching anything: build environments, toolchains and gotchas differ
completely between an RP2350 and an ESP32-S3, and nothing here is portable
between them.

Currently: [`devices/rp2350-amoled-1.8`](devices/rp2350-amoled-1.8/AGENTS.md).

## Rules that hold across devices

- **Verify which board you physically have before reading any documentation.**
  Waveshare sells RP2350 and ESP32-S3 versions of the same product, in the same
  case, under nearly the same name. `picotool info` or the USB vendor ID
  settles it in seconds. Getting this wrong costs a toolchain install and
  several hours of reading the wrong datasheets, which is exactly how this
  repo started.
- **Never re-copy a vendor driver over a patched one.** The patches are listed
  by name in each device's `AGENTS.md`, and `devices/*/vendor-baseline/` holds
  the unmodified originals so a diff always shows what we changed. Re-copying
  silently reintroduces bugs that took a day to find.
- **Prefer fixing at app level.** A patch inside a vendor driver is a
  maintenance liability; rounding an argument before you pass it is not. Patch
  the driver only when there is genuinely no hook, and say so in a comment.
- **Do not trust a pin name.** Vendor headers name pins after what the pin was
  meant to do, not what the schematic wires it to. Check the schematic before
  writing a handler.
- **Instrument before theorising.** Every wrong diagnosis in this repo so far
  came from reasoning out from a plausible mechanism. The ones that worked came
  from finding a property of the symptom that the suspected subsystem could not
  possibly explain.

## Conventions

- TypeScript only for host tooling: `.ts`, run with `bun`. No `.js` or `.mjs`,
  including build scripts and throwaway scripts.
- Local host apps bind to `127.0.0.1` explicitly. `Bun.serve({port})` with no
  hostname listens on every interface.
- No em dashes in prose or comments.
- Secrets live in `.secrets.env`, never committed.

## Adding a device

Create `devices/<board>/` with, at minimum, an `AGENTS.md` (how to build and
flash, plus gotchas) and a `README.md` (the bring-up log, written for a
stranger). Copy the unmodified vendor drivers to `vendor-baseline/` before
patching anything. Add a row to the table in the root `README.md`.
