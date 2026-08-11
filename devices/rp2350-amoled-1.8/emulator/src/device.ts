// The device shell: dragging the puck around the page, rotating it, and the
// two side buttons. None of this is a port of firmware code (the puck body
// does not exist in main.c); it is the physical mock the panel sits inside.
//
// Dragging and rotating are scoped to explicit elements the user grabs
// directly (the bezel background, a rotate handle), never the whole
// document, so this does not fight markup's own pointer handling when the
// page is served through markup/serve.ts (see README, "Serving through
// markup").

export function makeDraggable(bezel: HTMLElement, wrapper: HTMLElement): void {
  let dragging = false;
  let startX = 0, startY = 0, origLeft = 0, origTop = 0;

  bezel.addEventListener("pointerdown", (e) => {
    // Only start a drag when the plastic itself was grabbed, not the
    // screen or a button (those are child elements with their own
    // handlers; e.target is the bezel only when the click landed on bare
    // plastic, since children stop being the target for their own hits).
    if (e.target !== bezel) return;
    dragging = true;
    startX = e.clientX;
    startY = e.clientY;
    const rect = wrapper.getBoundingClientRect();
    origLeft = rect.left;
    origTop = rect.top;
    bezel.setPointerCapture(e.pointerId);
    e.preventDefault();
  });
  bezel.addEventListener("pointermove", (e) => {
    if (!dragging) return;
    const dx = e.clientX - startX;
    const dy = e.clientY - startY;
    wrapper.style.left = `${origLeft + dx}px`;
    wrapper.style.top = `${origTop + dy}px`;
  });
  const stop = () => { dragging = false; };
  bezel.addEventListener("pointerup", stop);
  bezel.addEventListener("pointercancel", stop);
}

export interface PwrEvents {
  onShort: () => void;
  onLong: () => void; // fires once, at 1.5s held (AXP2101 long-press IRQ threshold)
  onHardOff: () => void; // fires once, at 6s held (AXP2101 hard power-off threshold)
}

// PWR button press-and-hold: short (<1.5s), long (>=1.5s, fires at the
// threshold, still held), hard-off (>=6s). Mirrors the AXP2101 PMIC
// thresholds noted in main.c's buttons_poll() comment (REG 27H: long-press
// IRQ at 1.5s, hard cutoff at 6s).
export function wirePwrButton(btn: HTMLElement, events: PwrEvents): void {
  const LONG_MS = 1500;
  const HARDOFF_MS = 6000;
  let downAt = 0;
  let longTimer: ReturnType<typeof setTimeout> | null = null;
  let hardTimer: ReturnType<typeof setTimeout> | null = null;

  function clearTimers() {
    if (longTimer) { clearTimeout(longTimer); longTimer = null; }
    if (hardTimer) { clearTimeout(hardTimer); hardTimer = null; }
  }

  btn.addEventListener("pointerdown", (e) => {
    downAt = performance.now();
    btn.classList.add("pressed");
    btn.setPointerCapture(e.pointerId);
    longTimer = setTimeout(() => { btn.classList.add("long"); events.onLong(); }, LONG_MS);
    hardTimer = setTimeout(() => { events.onHardOff(); }, HARDOFF_MS);
  });
  btn.addEventListener("pointerup", () => {
    const held = performance.now() - downAt;
    btn.classList.remove("pressed", "long");
    clearTimers();
    if (held < LONG_MS) events.onShort();
  });
  btn.addEventListener("pointercancel", () => {
    btn.classList.remove("pressed", "long");
    clearTimers();
  });
}

// BOOT is wired only to the RP2350's bootloader-entry strap, sampled once
// at power-on; the firmware itself never reads it at runtime (confirmed on
// hardware, main.c's button-hunt comment). So this is inert by design, a
// press-feedback flash and nothing else.
export function wireBootButton(btn: HTMLElement): void {
  btn.addEventListener("pointerdown", (e) => {
    btn.classList.add("pressed");
    btn.setPointerCapture(e.pointerId);
  });
  const stop = () => btn.classList.remove("pressed");
  btn.addEventListener("pointerup", stop);
  btn.addEventListener("pointercancel", stop);
}
