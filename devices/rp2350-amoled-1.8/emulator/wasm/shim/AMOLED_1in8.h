/*
 * shim/AMOLED_1in8.h: a minimal stand-in for firmware/lib/AMOLED/AMOLED_1in8.h,
 * used ONLY by the wasm build (see build.ts and shim/DEV_Config.h's header
 * comment for why this directory exists). The real header pulls in
 * qspi_pio.h (a PIO/DMA driver, pure hardware); this shim defines exactly
 * what gfx.h and gfx.c actually use:
 *
 *   - AMOLED_1IN8_WIDTH / AMOLED_1IN8_HEIGHT: the panel's native dimensions,
 *     which PANEL_W/PANEL_H (gfx.h) are #defined from.
 *   - AMOLED_1IN8_Display() / AMOLED_1IN8_DisplayWindows(): the two panel
 *     push entry points gfx.c calls. emu_shim.c implements both - not by
 *     pushing pixels anywhere (the framebuffer already holds them, and
 *     emu_fb() hands the host a pointer straight into it), but by recording
 *     the pushed rectangle into the emu_push_* list, per emu_abi.h: "The
 *     firmware's push path records every window it sent to the panel...
 *     That overlay is the single most useful thing here."
 *
 * AMOLED_1IN8_Init, AMOLED_1IN8_SetBrightness, AMOLED_1IN8_SetWindows and
 * AMOLED_1IN8_Clear are the real header's too, but nothing compiled into the
 * wasm build calls them (they are runtime.c's, which stays board-only), so
 * they are not declared here.
 */
#ifndef _AMOLED_1IN8_H_
#define _AMOLED_1IN8_H_

#include "DEV_Config.h"

#define AMOLED_1IN8_WIDTH  368
#define AMOLED_1IN8_HEIGHT 448

void AMOLED_1IN8_Display(UWORD *Image);
void AMOLED_1IN8_DisplayWindows(uint32_t Xstart, uint32_t Ystart, uint32_t Xend, uint32_t Yend, UWORD *Image);

#endif // !_AMOLED_1IN8_H_
