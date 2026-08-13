// The puck chrome: dragging it around the page, rotating it, and whatever
// buttons the firmware declared. Nothing in here is a port of firmware
// code or knows about a specific device; button shape, position and
// press/hold behaviour all come from emu_device()'s declaration (see
// wasm.ts's DeviceButton and main.ts's chrome-building).
//
// Dragging is scoped to an explicit element the user grabs directly (the
// bezel background), never the whole document, so this does not fight
// markup's own pointer handling when the page is served through
// markup/serve.ts (see README, "Serving through markup").

// onDrag (optional): every pointermove while actively dragging, with the
// raw client coordinates, in ADDITION to repositioning the wrapper. This
// is what lets main.ts feed the same jolt detector the window-shake path
// uses (see puckDragShake there) from an in-page drag instead: dragging
// the puck is ordinary DOM pointer input, which always gets delivered and
// always gets animation frames, unlike a real OS titlebar drag (see
// windowshake.ts's header comment on why that path cannot be trusted).
export function makeDraggable(bezel: HTMLElement, wrapper: HTMLElement, onDrag?: (clientX: number, clientY: number) => void): void {
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
    onDrag?.(e.clientX, e.clientY);
  });
  const stop = () => { dragging = false; };
  bezel.addEventListener("pointerup", stop);
  bezel.addEventListener("pointercancel", stop);
}

export interface ButtonEvents {
  onDown?: () => void;
  onUp?: () => void;
  // Fired on release, after onUp: isLong tells the module which verdict
  // this press earned. Only meaningful (and only called by main.ts) for a
  // button that declared longPressMs; a button without one is a plain
  // level switch and the firmware decides click/hold itself, exactly as
  // real GPIO would.
  onVerdict?: (isLong: boolean) => void;
}

export interface WiredButton {
  down(): void;
  up(): void;
}

// Generic press-and-hold wiring: down fires immediately, a long verdict
// fires once at longPressMs while still held (if longPressMs is given),
// and a verdict fires on release either way (long if the threshold was
// already reached, short otherwise). A button with no longPressMs always
// gets a "short" verdict on release, immediately, exactly matching a plain
// button that has no concept of a hold at all.
//
// "holding" (only added when longPressMs is declared) is a second class
// alongside "pressed", read by app.css's ::after fill: its transition
// duration is set, once, to that exact longPressMs (via the --hold-ms
// custom property below), so the fill's own animation IS the countdown,
// not a separate approximation of it - the owner should never have to
// wonder whether a five-second hold registered, and a CSS transition timed
// to the real threshold cannot drift from it the way a hand-rolled JS
// progress loop could.
export function wireButton(el: HTMLElement, events: ButtonEvents, longPressMs?: number): WiredButton {
  let longFired = false;
  let longTimer: ReturnType<typeof setTimeout> | null = null;

  if (longPressMs !== undefined) el.style.setProperty("--hold-ms", `${longPressMs}ms`);

  function clearTimer() {
    if (longTimer) { clearTimeout(longTimer); longTimer = null; }
  }

  function down() {
    if (el.classList.contains("pressed")) return; // already down (e.g. key auto-repeat)
    longFired = false;
    el.classList.add("pressed");
    if (longPressMs !== undefined) el.classList.add("holding");
    events.onDown?.();
    if (longPressMs !== undefined) {
      longTimer = setTimeout(() => {
        longFired = true;
        el.classList.add("long");
        events.onVerdict?.(true);
      }, longPressMs);
    }
  }
  function up() {
    if (!el.classList.contains("pressed")) return;
    el.classList.remove("pressed", "long", "holding");
    clearTimer();
    events.onUp?.();
    if (!longFired) events.onVerdict?.(false);
  }

  el.addEventListener("pointerdown", (e) => { el.setPointerCapture(e.pointerId); down(); e.preventDefault(); });
  el.addEventListener("pointerup", up);
  el.addEventListener("pointercancel", up);

  return { down, up };
}

// Owner feedback: hard to hit with a mouse and hard to read at a glance.
// Roughly 40% bigger in both dimensions than the first pass (40x8 -> 56x14).
const BTN_LENGTH_PX = 56;
const BTN_THICKNESS_PX = 14;
const BTN_OFFSET_PX = -8; // how far it protrudes past the bezel edge

// Creates the DOM element for one declared button and positions it along
// its declared edge, at its declared fraction (0..1). Position is real
// geometry, not decoration: emu_abi.h calls out button position as "a real
// source of confusion when a device is held rotated, and a diagram beats a
// paragraph", so this IS the diagram.
export function createButtonElement(
  edge: "left" | "right" | "top" | "bottom",
  at: number,
  bezelWidthPx: number,
  bezelHeightPx: number
): HTMLDivElement {
  const el = document.createElement("div");
  el.className = `dev-btn edge-${edge}`;
  const clampedAt = Math.max(0, Math.min(1, at));
  if (edge === "left" || edge === "right") {
    const top = clampedAt * Math.max(0, bezelHeightPx - BTN_LENGTH_PX);
    el.style.top = `${top}px`;
    el.style.height = `${BTN_LENGTH_PX}px`;
    el.style.width = `${BTN_THICKNESS_PX}px`;
    el.style[edge] = `${BTN_OFFSET_PX}px`;
  } else {
    const left = clampedAt * Math.max(0, bezelWidthPx - BTN_LENGTH_PX);
    el.style.left = `${left}px`;
    el.style.width = `${BTN_LENGTH_PX}px`;
    el.style.height = `${BTN_THICKNESS_PX}px`;
    el.style[edge] = `${BTN_OFFSET_PX}px`;
  }
  return el;
}

// dx/dy (default 0) are the puck-motion shake offset (see puckmotion.ts),
// in screen pixels. translate() is listed before rotate() on purpose: CSS
// applies the transform list right to left, so the element rotates about
// its own centre FIRST and is then shifted by (dx, dy) in the parent's
// (screen) space, meaning the shake always reads as moving in real screen
// directions regardless of the puck's current rotation setting.
export function applyRotation(bezel: HTMLElement, totalDeg: number, dx = 0, dy = 0): void {
  bezel.style.transform = `translate(${dx}px, ${dy}px) rotate(${totalDeg}deg)`;
}
