// Builds a jump-to-app strip from the device descriptor's own "apps" array
// (names only, per emu_abi.h; no separate emu_app_name export any more).
// Only rendered, and emu_app_current/emu_app_switch only ever called, when
// the firmware actually declared apps: "A firmware without a concept of
// apps leaves these unimplemented and the emulator will not call them."

import type { EmuExports } from "./wasm";

export interface AppStripControl {
  refresh(): void;
}

export function buildAppStrip(container: HTMLElement, apps: string[], emu: EmuExports): AppStripControl | null {
  container.innerHTML = "";
  if (apps.length === 0 || typeof emu.emu_app_switch !== "function" || typeof emu.emu_app_current !== "function") {
    container.parentElement?.classList.add("hidden");
    return null;
  }
  container.parentElement?.classList.remove("hidden");

  const buttons: HTMLButtonElement[] = apps.map((name, i) => {
    const btn = document.createElement("button");
    btn.className = "btn sec sm app-strip-btn";
    btn.textContent = name;
    btn.addEventListener("click", () => emu.emu_app_switch?.(i));
    container.appendChild(btn);
    return btn;
  });

  function refresh(): void {
    const current = emu.emu_app_current?.() ?? -1;
    buttons.forEach((b, i) => b.classList.toggle("active", i === current));
  }
  refresh();
  return { refresh };
}
