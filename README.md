# tiny-computers

Firmware and tooling for small screens-with-a-CPU: the pocket-sized
touchscreen puck class of device. One directory per board under `devices/`,
each self-contained, each with its own bring-up log.

The bring-up logs are the point of this repo as much as the code is. These
boards ship as a zip of example code rather than as a maintained library, and
that code has bugs, mislabels its own pins, and configures parts in ways the
datasheet says will not work. Every one of those cost real time to find and
almost nothing to write down afterwards. So they get written down.

## Devices

| Device | MCU | Screen | Status |
|---|---|---|---|
| [`rp2350-amoled-1.8`](devices/rp2350-amoled-1.8) | RP2350A | 368x448 AMOLED, SH8601 over QSPI | working: sketchpad, two app slots |

Planned: the ESP32-S3 sibling of the same 1.8" AMOLED puck. Waveshare sells
both under nearly identical names in the same case, which is its own lesson:
**verify what you actually have before reading any documentation.** This
project began by installing an entire ESP-IDF toolchain for the wrong chip.

## Layout

```
devices/<board>/
  README.md          the bring-up log: what the hardware really does
  AGENTS.md          how to build, flash and work on it
  docs/decisions/    why things are the way they are
  firmware/          the app, plus vendor drivers with our patches
  vendor-baseline/   unmodified vendor drivers, to diff patches against
  tools/             host-side tooling
```

`AGENTS.md` says how, `README.md` says what actually happens, and
`docs/decisions/` says why. Keeping those three separate is deliberate: the
first goes stale silently, the second is what a stranger needs, and the third
is what stops a decision being re-litigated by whoever arrives next.

## Working on this

Each device directory stands alone. Read its `AGENTS.md` first; it carries the
build environment, the flashing commands, and the gotchas that will otherwise
cost you an afternoon.

Two conventions worth stating once:

- **Patches to vendor drivers are called out by name** in each device's
  `AGENTS.md`, because they are invisible in the source and are silently lost
  if the driver is ever re-copied from the vendor zip. `vendor-baseline/` is
  there so you can always see exactly what changed.
- **Findings get filed upstream when there is a repository that actually
  contains the defect**, and recorded here when there is not. The reason to
  file is not that the vendor will fix it; it is that an issue is searchable
  and the next person with the same symptom will land on it. Where that is
  impossible, the bring-up log is the only record, which is why it is written
  for a stranger rather than as notes to self.
