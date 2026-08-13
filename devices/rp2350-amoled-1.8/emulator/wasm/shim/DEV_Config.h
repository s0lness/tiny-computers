/*
 * shim/DEV_Config.h: a minimal stand-in for firmware/lib/Config/DEV_Config.h,
 * used ONLY by the wasm build (see build.ts: this directory goes first on
 * the include path). The real header pulls in six pico-sdk headers (under
 * hardware/) to declare GPIO/SPI/I2C/PWM wrappers no wasm-compiled source ever
 * calls; this shim defines exactly what actually gets used by the sources
 * that DO compile for wasm (gfx.h, gfx.c, apps/sketch.c):
 *
 *   - UWORD/UBYTE: the vendor's type aliases, used throughout gfx.h/
 *     AMOLED_1in8.h's declarations.
 *   - DEV_Delay_ms(): apps/sketch.c's wipe_erase() calls it to pace the
 *     shake-to-erase wipe band by band. See emu_shim.c for why this is a
 *     no-op in wasm rather than a real delay (the short version: a
 *     synchronous sleep would freeze the browser tab, and pacing a redraw
 *     is exactly the kind of thing docs/decisions/0003 already rules out of
 *     the emulator's job - "Timing... is not real").
 *
 * Nothing else DEV_Config.h declares (DEV_Module_Init, DEV_I2C_*, DEV_SPI_*,
 * the pin numbers) is referenced by anything this build compiles, so none of
 * it is declared here.
 */
#ifndef _DEV_CONFIG_H_
#define _DEV_CONFIG_H_

#include <stdint.h>

#define UBYTE   uint8_t
#define UWORD   uint16_t
#define UDOUBLE uint32_t

void DEV_Delay_ms(UDOUBLE xms);

#endif
