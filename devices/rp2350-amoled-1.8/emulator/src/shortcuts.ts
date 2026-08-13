// One window-level keydown/keyup listener, shared by every control that
// wants a keyboard shortcut (buttons, sensors), rather than each control
// attaching its own. That matters here specifically because the chrome is
// rebuilt from scratch on every wasm reload (the device descriptor can
// change), so a per-control listener would leak one more copy of itself on
// every rebuild; clear() lets main.ts wipe all bindings before rebuilding.

function isTypingTarget(target: EventTarget | null): boolean {
  const el = target as HTMLElement | null;
  if (!el) return false;
  const tag = el.tagName;
  return tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT";
}

export class ShortcutRegistry {
  private clicks = new Map<string, () => void>();
  private held = new Map<string, { down: () => void; up: () => void }>();
  private activeKeys = new Set<string>();

  constructor() {
    window.addEventListener("keydown", (e) => this.onKeyDown(e));
    window.addEventListener("keyup", (e) => this.onKeyUp(e));
  }

  clear(): void {
    this.clicks.clear();
    this.held.clear();
    this.activeKeys.clear();
  }

  bindClick(key: string, fn: () => void): void {
    this.clicks.set(key.toLowerCase(), fn);
  }

  bindHeld(key: string, handlers: { down: () => void; up: () => void }): void {
    this.held.set(key.toLowerCase(), handlers);
  }

  private onKeyDown(e: KeyboardEvent): void {
    if (isTypingTarget(e.target)) return;
    const key = e.key.toLowerCase();
    if (e.repeat) return;
    const heldFn = this.held.get(key);
    if (heldFn) {
      this.activeKeys.add(key);
      heldFn.down();
      e.preventDefault();
      return;
    }
    const clickFn = this.clicks.get(key);
    if (clickFn) {
      clickFn();
      e.preventDefault();
    }
  }

  private onKeyUp(e: KeyboardEvent): void {
    const key = e.key.toLowerCase();
    const heldFn = this.held.get(key);
    if (heldFn && this.activeKeys.has(key)) {
      this.activeKeys.delete(key);
      heldFn.up();
    }
  }
}

// Picks a free single-key shortcut for a declared id, preferring a letter
// from the id itself (so "boot" naturally gets "b", "pwr" gets "p") so the
// assignment reads as intentional rather than arbitrary, falling back to
// digits and then the rest of the alphabet if every letter in the id is
// already taken.
export function assignShortcut(id: string, used: Set<string>): string | null {
  for (const ch of id.toLowerCase()) {
    if (/[a-z]/.test(ch) && !used.has(ch)) {
      used.add(ch);
      return ch;
    }
  }
  for (const ch of "123456789") {
    if (!used.has(ch)) {
      used.add(ch);
      return ch;
    }
  }
  for (let c = 97; c <= 122; c++) {
    const ch = String.fromCharCode(c);
    if (!used.has(ch)) {
      used.add(ch);
      return ch;
    }
  }
  return null;
}
