/*
 * shim/stdlib.h: NOT one of the four shims the task brief anticipated
 * (DEV_Config.h and AMOLED_1in8.h, for the vendor headers) - this one exists
 * because zig 0.16's wasm32-freestanding target ships no <stdlib.h> at all
 * (confirmed empirically: `zig cc -target wasm32-freestanding -c` on a file
 * that includes it fails with "'stdlib.h' file not found", not a link
 * error). That tracks the C standard's own split: a freestanding
 * implementation is only required to provide <stddef.h>, <stdint.h>,
 * <stdbool.h>, <stdarg.h> and a few others; <stdlib.h>, <math.h> and
 * <stdio.h> are hosted-only, and zig's freestanding target takes that
 * literally rather than bundling a minimal libc for them.
 *
 * gfx.c's `#include <stdlib.h>` (for malloc(), in gfx_init()) is real
 * firmware code compiled unmodified for this target, so it needs a
 * malloc() declaration to compile against. emu_shim.c provides the actual
 * implementation - see its header comment for why that is a static bump
 * allocator rather than a real allocator, which is fine given the firmware
 * only ever calls it once.
 */
#ifndef _SHIM_STDLIB_H_
#define _SHIM_STDLIB_H_

#include <stddef.h>

void *malloc(size_t size);

#endif
