# The emulator landscape, before extracting this into a public repo

Written before making a decision the owner cannot easily reverse: whether to keep
this project's approach (compile the firmware's own C to `wasm32-freestanding`
and emulate the board's boundary, per
[decision 0003](../../docs/decisions/0003-emulator-runs-the-real-apps.md)), or
move toward something closer to what mature peers do. Four questions drove
this: does an RP2350 emulator already exist, what would emulating it properly
take for this firmware specifically, how do others prove an emulator follows
the real device, and what does the owner's one-language request cost in
either direction. Sources are linked throughout; anything not directly
confirmed is marked as such rather than smoothed over.

## What exists

| Project | Emulates | Does NOT emulate | Runs our `.uf2`/`.elf`? | Licence | Activity |
|---|---|---|---|---|---|
| [rp2040js](https://github.com/wokwi/rp2040js) (Wokwi's engine) | RP2040 only: dual Cortex-M0+, enough GPIO/UART/timers to blink an LED, run Arduino code, run MicroPython's REPL | RP2350, Cortex-M33, PIO fidelity beyond basics, flash writes ("the filesystem is not writeable, as the SSI peripheral required for flash writing is not implemented yet") | No. Different chip family (M0+, not M33), and this board is RP2350 | MIT, Copyright Uri Shaked | Active on RP2040; RP2350 tracked in [issue #142](https://github.com/wokwi/rp2040js/issues/142), open since Jan 2025, points only to a third-party fork with RISC-V-only support, not Cortex-M33 |
| [c1570/rp2040js fork](https://github.com/c1570/rp2040js) | Partial RP2350, **RISC-V core only** | Cortex-M33 (the core this device actually runs) | No, for our purposes: wrong core | Inherits MIT | Unverified activity level; found only as a link from issue #142, not independently confirmed |
| Wokwi (hosted product, docs.wokwi.com) | Whatever rp2040js/its other engines emulate, plus a UI, wiring diagrams, serial monitor | Same ceiling as rp2040js: no RP2350/Pico 2 board in the public simulator as of this research | No | Proprietary hosted service, engine components separately licensed | Active product |
| QEMU, RP2350/Pico 2 support | Nothing yet, merged. [Task #3125](https://gitlab.com/qemu-project/qemu/-/work_items/3125) states the goal (RP2350, Pico 2, Pico 2 W, SparkFun Thing Plus) and lists the target peripherals (12 PIO state machines, USB, GPIO, ADC, flash/RAM) | Everything, until this lands | No | GPL/LGPL (QEMU) | Open task, no linked merge request found, no completion evidence found |
| QEMU, RP2040 support | An `[RFC PATCH 0/6]` series from January 2022 by Alex Bennée added "basic skeleton," board memory wiring, and boot ROM boilerplate for a `raspi-pico` machine ([mail-archive thread](https://www.mail-archive.com/qemu-devel@nongnu.org/msg860944.html)) | Confirmed merged status not found; the thread is an RFC, not a merge confirmation | Unconfirmed, and irrelevant to this device (RP2040, not RP2350) | GPL/LGPL | Unverified since 2022 |
| [matgla/Renode_RP2040](https://github.com/matgla/Renode_RP2040) | RP2040 only: dual Cortex-M0+, PIO (via an external C++ simulator), DMA with control blocks, I2C master, UART+DMA, SPI master, GPIO+interrupts, timers, watchdog, partial ADC | IRQ and DMA are explicitly called out as **not yet supported** in places, clock configuration not supported, USB and multicore have limited or no implementation, and the project's own status is **"WIP and Frozen (lack of time)"** | No: RP2040 core, and stated incomplete even for that chip | MIT | Frozen by the maintainer's own label |
| Renode (Antmicro), official upstream | No RP2040 or RP2350 entry found in the [official supported-boards list](https://renode.readthedocs.io/en/latest/introduction/supported-boards.html) | RP2040/RP2350 entirely, at the official level; only the third-party repo above exists | No | MIT (Renode core) | Renode itself is active; RP2040/RP2350 support is not part of it |
| [picoem / rp2350-emu](https://github.com/0x4D44/picoem) (crate `rp2350-emu` on crates.io) | The most serious find here: dual Arm Cortex-M33 @ 150MHz **and** dual Cortex-M0+ (RP2040), FPU, coprocessors, PIO blocks, boots the **real Raspberry Pi bootroms**, differentially validated against QEMU's Cortex-M33 and real RP2354 silicon via SWD | **UART / SPI / I2C / DMA / timers are explicitly stubs.** Hazard3 RISC-V is out of scope | Loads real binaries (`cargo run -p rp2350-emu-tui -- path/to/firmware.bin`), but with I2C and DMA stubbed, our firmware (three I2C devices on one bus, DMA-driven display and audio) would not run meaningfully | Dual MIT / Apache-2.0 | Very new: five releases in a five-day window, May 3-8 2026 ([libraries.io](https://libraries.io/cargo/rp2350-emu)), single author, own words: "a personal research project... No promises" |
| [ice458's PIO simulator](https://ice458.github.io/tools/pio_sim/index.html) | PIO assembly only, in-browser, both RP2040 and RP2350 instruction sets, state machine registers, FIFOs, GPIO, timing chart, IRQ flags | Everything outside PIO: no CPU, no DMA, no I2C, no whole-chip context | N/A, not a chip emulator | Unconfirmed | Unconfirmed activity, appears to be a solo hobby tool |
| [NathanY3G/rp2040-pio-emulator](https://github.com/NathanY3G/rp2040-pio-emulator) | PIO only, Python package, RP2040 and RP2350, aimed at unit-testing PIO programs | Same ceiling as above: PIO in isolation | N/A | Unconfirmed | Has tagged releases on GitHub |

**The direct answer to "does an RP2350 emulator already exist": no, not one that
covers what this firmware needs.** The closest thing, picoem, is three months
old, single-author, explicitly a research project with "no promises," and
stubs exactly the peripherals (I2C, DMA) this firmware leans on hardest. The
standalone PIO simulators are real and could plausibly be reused for the
display/audio PIO programs in isolation, but none of them sit inside a chip
model with cores, DMA, or a bus to arbitrate.

## Speculos: the closest peer, and the architecture the owner asked to be checked against

Ledger's [Speculos](https://github.com/LedgerHQ/speculos) is a device emulator
for their hardware wallet apps, and it made a real architectural choice that
is worth understanding precisely rather than from memory.

### What it actually does, verified

- **It runs the real app ELF**, compiled for **ARMv7-M/Thumb-2** (the secure
  element's real target), unmodified: "It opens the target app (`app.elf`)
  from the filesystem and maps it as is in memory"
  ([internals.md](https://speculos.ledger.com/dev/internals.html)).
- **The CPU is QEMU, but specifically `qemu-arm-static` user-mode emulation**
  (Linux-user style), not QEMU's system-mode / full-machine emulation. This
  matters and is worth stating plainly: user-mode QEMU only has to emulate
  the instruction set and a syscall funnel, not a memory map, a board, or
  peripherals, because the "app" is a userland ELF that expects an OS
  underneath it. That is a categorically lighter lift than what an RP2350
  emulator needs, because our firmware **is** the OS/bare-metal image; there
  is no host Linux process model for it to run inside.
- **The syscall trap is a single mechanism, not per-peripheral device
  models.** The `svc` instruction is replaced with `udf`, which raises
  `SIGILL`, which Speculos catches and dispatches to a Python
  reimplementation of "a few syscalls made by common apps" (explicitly not
  install, firmware-update, or OS-info syscalls). One instruction, one trap
  point, covering every hardware-adjacent thing an app can ask for, because
  BOLOS (Ledger's OS) enforces that everything an app does crosses that one
  gate.
- **Display and input are Python, driven off that same syscall boundary**: a
  framebuffer served over a Flask REST API on `127.0.0.1:5000`, with
  button/touch injection through the same API.
- **What it explicitly does NOT emulate**, in Ledger's own words as
  paraphrased by their blog post: "There is no firmware running inside
  Speculos, just a reimplementation of a few functions available through the
  SDK" ([Medium, "Speculos: an emulator for
  developers"](https://medium.com/ledger-on-security-and-blockchain/speculos-1709aa5fbec6)).
  BOLOS itself, the real OS, never runs anywhere in Speculos. There is no
  dashboard, no screensaver, no USB transport (TCP-based APDU only), and the
  same post admits an app "might behave differently than an app running on a
  hardware device (indeed, the code of the project is different)."

**Correction to the owner's stated understanding, stated as asked rather than
smoothed over:** the syscall boundary is not "the device OS reimplemented,"
it is a **narrow, single-instruction trap into a partial reimplementation of
the SDK surface an app calls**. BOLOS the OS is not modeled at all, it simply
never appears in the picture. This is closer to our situation than it first
looks, see below.

### Why that boundary, not a lower or higher one

No explicit design-rationale document from Ledger was found, unlike this
project's own decision 0003. The internals doc is pragmatic and
implementation-focused rather than a manifesto: it explains that `seccomp` or
`ptrace` alternatives "seem not practicable because QEMU don't support" what
they'd need, and that a disassembler-based instruction rewrite "doesn't look
worth it" versus the byte-patch approach actually used. **State this
plainly: if the owner is picturing a considered "we chose this boundary for
these reasons" writeup on Ledger's side, it was not found. What was found
reads as the cheapest mechanism that gets a real binary running, not a
philosophically justified boundary choice.**

### How they test conformance: Ragger

[Ragger](https://github.com/LedgerHQ/ragger) is Ledger's pytest-based
framework that runs **the same test script against three backends**:
Speculos (emulator), and two real-hardware transports, LedgerComm and
LedgerWallet. Its own stated rationale is developer convenience ("it would be
very convenient to be able to develop code which would be compatible
wherever the application runs, either on an emulator or on a physical
device," [Ragger's rationale
page](https://ledgerhq.github.io/ragger/rationale.html)) rather than an
explicit claim that this is how they validate the emulator, but the
mechanism is exactly differential testing: `navigate_and_compare` walks the
same navigation script and diffs each step's screenshot against a checked-in
golden image, across whichever backend is selected. Running that same
suite against real hardware and against Speculos, and comparing, is the
practical conformance check, inferred from the mechanism rather than stated
outright by Ledger as its purpose.

### What it is written in, stated plainly per the owner's ask

**Python** (the CLI, the syscall reimplementations, the Flask API), **C**
(support code under `src/`), and **QEMU** itself as an external C/C++
dependency it does not author but fully depends on. This is genuinely
polyglot, by necessity, not by accident: three components doing three
different jobs (run real machine code, glue to a scriptable host, provide an
OS-shaped syscall surface) that nobody has found a single-language way to
cover. **If the best-in-class emulator in the owner's own domain could not
honour a one-language constraint, that is a real data point, not a detail to
bury.**

### Mapped onto our situation, explicitly

- **Our equivalent of the BOLOS syscall boundary already exists, and is
  already clean.** `firmware/runtime/app.h`'s rule ("an app never touches
  hardware") is exactly BOLOS's rule for a Ledger app. `gfx.h` and
  `sensors.h` are our syscall table. Decision 0003 found this boundary
  because the runtime split was already right for other reasons; Speculos's
  existence is independent confirmation that this is the correct place to
  cut, not a reason to move it.
- **Their real advantage, which the owner has correctly noticed, is that the
  literal bytes that ship are the bytes that run.** Speculos's app.elf is
  built by the same toolchain, for the same target, as what goes on the real
  secure element; only the syscall implementations differ. Our wasm module
  is not that: `firmware/apps/*.c` and `firmware/runtime/gfx.c` are the same
  **source**, compiled by a **different compiler, to a different
  instruction set** (`zig cc` to `wasm32-freestanding`) than what actually
  ships (the pico-sdk's own toolchain, targeting Cortex-M33 - apps run from
  flash by XIP, per firmware/CMakeLists.txt and tools/invariants rule 1;
  only core1's own reachable code, which apps are not part of, is
  RAM-resident - decisions 0004/0005/0016). **Decision 0003's claim, "not
  the same algorithm, the same
  object code," is not accurate held up against Speculos's bar. It is the
  same C, compiled twice, to two different object codes.** That distinction
  is small in practice for straight-line application logic and can matter
  for anything sensitive to integer width assumptions, struct layout, float
  precision, or undefined behaviour the two compilers resolve differently.
  This should be corrected in the decision record's language, not just
  noted here.
- **What moving to their boundary for real would take, concretely.** Nothing
  found in this research executes an RP2350 ELF the way QEMU executes a
  Ledger app.elf. The CPU core itself is not the hard part: QEMU already
  emulates Cortex-M33 generically for other chips (the same core family
  shows up on Nordic and other vendors' parts, and community bare-metal M33
  QEMU targets exist, e.g. `mps2-an505`/`musca`-class boards). The hard part
  is that Speculos gets to skip modeling a memory map, DMA, PIO, or a shared
  I2C bus, because BOLOS's syscall funnel means the app never touches any of
  that directly. **Our firmware has no such funnel below the app layer**:
  `runtime_core.c`, `sensors.c`, `AMOLED_1in8.c`, and `qspi_pio.c` all poke
  memory-mapped peripheral registers directly, at many call sites, on two
  cores. Reaching Speculos's fidelity for us would mean building actual
  RP2350 device models (PIO's own instruction set, a DMA descriptor engine,
  an I2C controller, the XIP/QSPI flash controller including its chip-select
  arbitration, SIO, the watchdog) inside a CPU emulator, which is a
  multi-month, dedicated undertaking on the evidence of picoem's own
  trajectory (a single committed author, three months in, still stubbing
  I2C and DMA). It is not a rewrite of `emu_shim.c`, it is a new project the
  size of picoem itself.

### Would Speculos's architecture have caught the RCA in decision 0005?

No, and this is worth stating precisely because it is the sharpest test of
how far any of this gets us. The bug
([0005](../../docs/decisions/0005-rca-core1-dies-on-first-button.md)) was
core1 fetching a corrupted instruction from flash because core0's BOOT-button
read borrows the QSPI chip select for 60-100us with interrupts disabled, and
nothing protected core1's XIP fetches during that window. That is a **timing
race at the flash controller, between two cores**, not a CPU-ISA bug and not
a syscall-boundary bug. Speculos's whole trick (trap one instruction, skip
modeling the hardware underneath) has nothing to say about it, because there
is no equivalent "one instruction" a two-core flash-arbitration race funnels
through. Catching this would need a CPU emulator with an actual QSPI/flash
controller model, arbitrating chip-select access between two cores' fetch
streams with real timing, at a fidelity **no project surveyed here claims**,
including picoem (whose bootrom-boot claim is impressive but does not extend
to modeling flash-controller arbitration hazards, and which stubs DMA
outright). This is the same conclusion `requirements.md` already reaches
generically ("an emulator and the real device 'are different failure modes,'
not degrees of the same one"), now confirmed against the single hardest bug
this project has actually hit. The bug was found by reading a hazard
(`flash_safe_execute`'s own documentation, and a comment in a deleted file
that had already hit it once) and reasoning about a race, not by running
anything in emulation. The durable fix removes the hazard by construction -
originally `copy_to_ram` for the whole image (decisions 0004/0005), narrowed
to RAM-placing only what core1 can reach once the whole-image cost became
unaffordable (decision 0016), mechanically enforced either way by
tools/invariants rather than left to hold by convention - plus the decision
records themselves, so the hazard survives the next refactor. That is a
process and documentation fix, not a tooling one, and no amount of emulator
investment currently on the table changes that.

## What emulating RP2350 properly would require, for this firmware specifically

Concrete, from what this firmware actually uses (`firmware/CMakeLists.txt`,
`firmware/runtime/sensors.c`, `firmware/lib/QSPI_PIO/`,
`firmware/runtime/sound.c`), not a generic checklist:

| Facility | Used how, here | A CPU-only emulator (ISA + generic memory only) | What a useful subset needs |
|---|---|---|---|
| **Two cores, `multicore_launch_core1`** | Core1 owns `i2c1` and every sensor; core0 renders. Decision 0002 section 3 | Fails outright unless the machine explicitly wires two cores plus SIO (picoem does; QEMU's unmerged RP2350 target and Renode's frozen RP2040 port do not) | Two real cores with independent fetch streams and *relative* timing, not just two threads of an interpreter |
| **PIO** | `qspi.pio` drives the AMOLED panel over QSPI (pio0); `sound_i2s.pio` drives the ES8311 codec over I2S (pio1), deliberately on a different PIO block to avoid colliding with the display's undeclared claim on pio0 | Fails: PIO is a second, independent instruction set with its own state machines, FIFOs, and side-set/wait/IRQ semantics; nothing about emulating the M33 core touches it | A PIO interpreter (the standalone ones found above are a plausible starting point) wired into the same memory/IRQ model as the rest of the chip, not standalone |
| **DMA** | Drives the panel push and the I2S stream | Fails: needs a descriptor engine and its own bus arbitration | A DMA model with real transfer timing, since the display push path's whole reason for existing (decision 0001) is about timing-sensitive window geometry |
| **I2C at 400kHz, three chips on one bus (`i2c1`)** | FT3168 touch, QMI8658 IMU, AXP2101 PMIC, all read from core1 only, single-threaded by design because the vendor demo's own bus-sharing bug (a non-atomic spinlock) corrupts transactions under concurrent access | Fails outright: this is exactly what picoem currently stubs | A real I2C controller model, including clock-stretching behaviour (decision 0005 flags this as an open question worth investigating even on hardware) |
| **XIP flash cache + QSPI chip-select arbitration** | The actual mechanism behind decision 0005's bug: `bootbtn.c`'s `read_cs_low()` floats the chip select with interrupts off on the calling core only, and any concurrent XIP fetch on the other core reads garbage | Fails, and so does everything else surveyed here (see above) | Nothing found provides this. It would need to be purpose-built, and is the single hardest peripheral-fidelity item on this list |
| **Watchdog** | Fed once per runtime loop; a hang reboots into the menu (decision 0002 section 6) | Fails on a CPU-only model (no watchdog peripheral) but is one of the easier omissions to add: matgla's frozen Renode port already lists a watchdog model | Straightforward relative to the rest of this table |
| **SIO** | Inter-core FIFO, spinlocks (`hardware_sync`'s `__dmb()` backs the lock-free touch queue between cores) | Fails without an explicit SIO model; this is part of why "two cores" above is not just "run the interpreter twice" | Needed as soon as two cores are modeled at all |

**The honest summary: every mechanism this firmware actually depends on,
past straight-line ISA execution, is either unimplemented or explicitly
stubbed in every project found in this research.** A CPU-only emulator
(which is what "add an RP2350 machine to QEMU" mostly means today, since the
M33 core itself is not the unclaimed part) would run single-threaded
straight-line C and fail the instant this firmware touches PIO, DMA, I2C, or
the two-core split, which is nearly immediately: `sensors_init()` and
`gfx_init()` both run before the first frame.

## The conformance question: how to test that an emulator follows the firmware

Surveyed practice, cross-referenced against what `requirements.md` already
found and what this pass added:

- **Differential testing against real hardware** is the strongest technique
  available and, for this project specifically, **already has both halves
  built**. `tools/README-devlink.md` and `firmware/devlink.c` describe a
  USB-serial protocol, verified working on real hardware, that can request a
  screenshot (`SHOT`, base64 RLE-encoded framebuffer), inject touch
  (`inject_down/move/up`), inject buttons, and switch apps. This is,
  functionally, our own LedgerWallet-equivalent backend: a way to drive real
  hardware programmatically and read back what it actually shows. The
  emulator side already has record/replay of input traces
  (`requirements.md`'s row 5) and a freeze bundle
  ([agent-loop.md](agent-loop.md)) carrying the same kind of trace. **The
  concrete, buildable recommendation: a small harness that plays one
  recorded input trace through both backends, the emulator via replay and
  real hardware via devlink, and diffs the resulting screenshots.** This is
  Ragger's exact pattern (`navigate_and_compare` across Speculos and
  LedgerWallet), built from pieces this project already has, not a new
  subsystem.
- **Golden-image / framebuffer hashing** is standard CI practice for
  emulators generally (GBA-emulator `--hash-out` pipelines, Flutter's golden
  tests), already recommended in `requirements.md` row 4, and is the
  natural next step once the differential harness above exists: hash the
  emulator's framebuffer per frame against a checked-in golden hash for a
  fixed replay trace, independent of hardware, as a fast CI gate.
- **CPU conformance suites** (the kind chip vendors run against an ISA
  implementation) are not a fit here and would be disproportionate for a
  two-person project: this project is not implementing a CPU, and the one
  bug that has actually bitten it (0005) is a peripheral-timing hazard, not
  an ISA-correctness bug. Not recommended.
- **How Wokwi, Renode, and retro emulators validate themselves**, as already
  surveyed in `requirements.md`: determinism as a prerequisite (already
  true here, `emu_tick(nowMs)`), record/replay as the mechanism that turns a
  bug into a file, and an explicit, repeated acknowledgment across every
  mature tool researched that **timing fidelity is the thing none of them
  actually guarantee**, even Wokwi's cycle-accurate AVR core
  ([wokwi-features#931](https://github.com/wokwi/wokwi-features/issues/931)).
  Nothing in this pass changes that conclusion; Speculos does not claim
  timing fidelity either, and could not have caught decision 0005's bug for
  the same underlying reason.

**What is practical for a two-person project, stated as a ranked plan:**
1. Build the devlink-vs-emulator differential harness above. Cheapest,
   highest-value, uses only pieces that already exist and already work.
2. Add framebuffer hashing against golden images for the emulator alone, as
   a CI gate independent of hardware access.
3. Do not attempt CPU-conformance-suite-style validation, and do not treat
   "the emulator agrees with the board today" as proof against the class of
   bug 0005 belongs to; that class needs hazard analysis and documentation
   discipline (decision records), not more emulation fidelity, at any
   budget this project can spend.

## The language question

The owner wants one language for the extracted repo and is open to Zig.
Assessed plainly rather than surveyed neutrally:

- **The browser side must run JS or wasm, unavoidably.** A DOM-driven page
  (canvas, sliders, the console pane, the freeze/replay UI, `markup`
  integration) cannot be authored in pure Zig without compiling to wasm and
  hand-writing DOM bindings, for which there is no mature ecosystem
  comparable to TypeScript's. Every piece of existing infrastructure this
  project already leans on, `markup`'s ink layer, its `puppeteer-core` +
  installed-Chrome headless-verification pattern (`scripts/verify.ts`), the
  fleet's shared design CSS, is web-native and would need to be reimplemented,
  not ported, to move off it.
- **A pure-Zig approach means a native windowed emulator instead of a
  browser one** (Zig bindings to something like SDL2 or raylib, in place of
  a DOM page). What that costs, plainly: no URL to open and hand someone,
  no `markup` annotation loop (the entire premise of
  [agent-loop.md](agent-loop.md), which the owner explicitly values, is
  built on a web page markup can already draw on), and a from-scratch
  screenshot/automation pipeline to replace `puppeteer-core`'s. This is not
  a small cost; it removes the specific loop `requirements.md` and
  `agent-loop.md` were written to serve.
- **A pure-TypeScript approach cannot exist either, strictly.** The firmware
  is real, shipping C, compiled by pico-sdk's own toolchain for the actual
  hardware; nothing about that changes regardless of what the emulator is
  written in. The current setup already reflects this: `zig cc` is a build
  tool invoked to cross-compile that C to wasm, not a language this
  project's own source is authored in. C on the firmware side and
  TypeScript on the host side are both already unavoidable; the actual
  choice on the table is only ever about the third thing, whatever glue
  compiles or executes the firmware's C for the emulator.
- **A third option, where the question genuinely dissolves rather than gets
  argued away**: treat this project as having exactly two authored
  languages by necessity, not by choice, C for the firmware (already the
  single source of truth, already compiled twice today: once by pico-sdk's
  arm toolchain for hardware, once by `zig cc` for wasm) and TypeScript for
  everything that runs in a browser or drives one, and keep any compiler
  (`zig cc` today, or a future one) firmly in the category of an invoked
  build tool rather than an authored language, the same way this project
  already does not count pico-sdk's own CMake/GCC toolchain as "a language
  this project is written in." **Speculos is direct evidence that a
  device-real-time emulator naturally wants at least this many roles filled
  (device-target code, a CPU/host bridge, host-side glue), and even the
  team that could pick any architecture they wanted ended up polyglot.**
  Fighting that with an artificial one-language rule costs real
  infrastructure (the markup loop, the puppeteer verification pattern) for
  a label, not a functional gain.

## Recommendation

**Keep the current architecture (decision 0003, wasm-compiled firmware C, a
browser host) as the extracted repo's foundation, correct decision 0003's
"same object code" claim to "same C, two different compiled targets" since
Speculos's bar shows that distinction is real, and add the devlink-vs-replay
differential harness as the concrete, buildable answer to the conformance
question, rather than chasing Speculos-grade fidelity that nothing surveyed
here, including a three-month-old dedicated research project, has actually
built for this class of hardware yet.**

The main risk: this leaves the wasm build and the real firmware build as two
different object codes from one source, forever, which is exactly the
divergence risk decision 0003 was written to eliminate for the
*app-versus-reimplementation* case and does not fully eliminate for the
*compiler-versus-compiler* case. The devlink-vs-replay harness recommended
above is the mitigation: it catches divergence behaviourally, at the
framebuffer, on whatever cadence the project actually runs it, rather than
requiring compiler-level object-code identity to trust the tool at all.
