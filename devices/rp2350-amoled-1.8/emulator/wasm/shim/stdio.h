/*
 * shim/stdio.h: NOT one of the shims the task brief anticipated - see
 * shim/stdlib.h's header comment for why (zig 0.16's wasm32-freestanding
 * target has no <stdio.h>, confirmed empirically the same way).
 *
 * firmware/runtime/gfx.c and every app under firmware/apps/ that logs
 * (chrono.c, timer.c, sketch.c) `#include <stdio.h>` and call printf() as
 * real, unmodified firmware code. printf is not in emu_abi.h's import list
 * and adding it as a new env.printf import would be exactly the silent
 * ABI-widening the task brief warns against - the JS side is written
 * against env.js_log and eight math functions, nothing else.
 *
 * So printf is implemented, not imported: emu_shim.c defines a small,
 * real (if format-subset) printf that formats into a local buffer and hands
 * the result to rt_log(), which is the legitimate path to env.js_log. It
 * never crosses the wasm/JS boundary itself - see emu_shim.c's header
 * comment for the exact format specifiers it supports and why this is safe
 * in a way a hand-rolled sinf() would not be (pure text formatting, no
 * numerical-fidelity concern).
 */
#ifndef _SHIM_STDIO_H_
#define _SHIM_STDIO_H_

int printf(const char *fmt, ...);

#endif
