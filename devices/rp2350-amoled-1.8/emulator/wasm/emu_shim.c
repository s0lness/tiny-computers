/*
 * emu_shim: everything the browser side of the emulator needs that is NOT
 * the real firmware. Three jobs, per docs/decisions/0003 and emu_abi.h:
 *
 * 1. Implements emu_abi.h - the lifecycle (emu_init/emu_tick), the panel
 *    (emu_fb, the emu_push_* window recorder), input (emu_touch,
 *    emu_button[_verdict], emu_sensor_event) and emu_device()'s JSON.
 *
 * 2. Implements sensors.h in full, so runtime_core.c and every app get a
 *    real touch queue, real key bits, real BOOT state and a real shake
 *    counter - fed by the functions in (1) - instead of talking to i2c1.
 *    This is the "browser supplies a touch controller, two buttons, and a
 *    clock" half of decision 0003's THE ONE IDEA.
 *
 * 3. Stands in for a few libc pieces the real firmware sources (gfx.c,
 *    apps/sketch.c and friends) call directly and that a freestanding,
 *    -nostdlib wasm build does not get for free: malloc() (a static bump
 *    allocator - see "malloc" below), printf() (a small local formatter -
 *    see "printf" below), and DEV_Delay_ms() (a no-op - see shim/DEV_Config.h).
 *    None of these are wasm imports; they are ordinary C compiled into this
 *    module, same as any other function here.
 *
 * Depends on emu_abi.h, runtime_core.h, app.h, gfx.h, sensors.h and
 * freestanding headers only - this file is the "host", so unlike
 * runtime_core.c it is allowed to know it is running in wasm.
 */
#include "emu_abi.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"
#include "gfx.h"
#include "runtime_core.h"
#include "sensors.h"

/* ===========================================================================
 * What the module imports from the host (env.*). Exactly emu_abi.h's list:
 * js_log, plus the eight math functions declared extern in shim/math.h and
 * left undefined here on purpose (see build.ts and shim/math.h's header
 * comment). js_log is the only one this file calls directly.
 * ======================================================================= */
extern void js_log(const char *ptr, int len);

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* ===========================================================================
 * malloc: a static bump allocator, not a real one.
 *
 * The task brief's own framing: "gfx_init() uses malloc. Freestanding wasm
 * has no malloc: provide one, or better, provide a trivial bump allocator
 * over a static array, since the only allocation in the whole program is one
 * 330KB framebuffer." Grepping firmware/runtime/gfx.c confirms that: exactly
 * one malloc() call site in the entire codebase, in gfx_init(), for
 * PANEL_W*PANEL_H*2 bytes, never freed. A bump allocator over a static array
 * is simpler than a real allocator, costs nothing this program does not
 * already pay (the framebuffer has to live somewhere in linear memory
 * either way), and there is no second caller for a free-list to ever matter
 * to. HEAP_BYTES has a little slack over the exact framebuffer size so a
 * future second allocation does not need this constant revisited under
 * pressure.
 * ======================================================================= */
#define HEAP_BYTES (368 * 448 * 2 + 4096)

static uint8_t g_heap[HEAP_BYTES] __attribute__((aligned(8)));
static size_t g_heapUsed = 0;

void *malloc(size_t size) {
    size_t aligned = (g_heapUsed + 7u) & ~(size_t)7u;
    if (size > HEAP_BYTES || aligned > HEAP_BYTES - size) {
        rt_log("FATAL: emu_shim malloc: bump heap exhausted");
        rt_halt();
        return NULL; // unreachable if rt_halt()'s contract is honoured
    }
    void *p = &g_heap[aligned];
    g_heapUsed = aligned + size;
    return p;
}

/* ===========================================================================
 * printf: a small, real (format-subset) implementation, not an import.
 *
 * See shim/stdio.h's header comment for why this exists at all (printf is
 * not in emu_abi.h's import list, and adding env.printf would silently
 * widen the ABI the JS side is written against). Supports exactly the
 * specifiers the firmware sources this build actually compiles use -
 * grepped, not guessed: %s, %d (with an optional zero-padded width, e.g.
 * timer.c's "%02d"), %lu (sketch.c's uint32_t timestamps, cast to unsigned
 * long at every call site), and %%. Anything else is copied through
 * literally (the leading %% and the following character), which is wrong
 * general-purpose printf behaviour but never fires here and is safer than
 * silently eating an unrecognised specifier's argument.
 * ======================================================================= */
static void out_char(char **p, char *end, char c) {
    if (*p < end) {
        **p = c;
        (*p)++;
    }
}

static void out_str(char **p, char *end, const char *s) {
    if (!s) s = "(null)";
    while (*s) out_char(p, end, *s++);
}

static void out_uint(char **p, char *end, unsigned long v, int width, bool zeroPad) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    for (int pad = width - n; pad > 0; pad--) out_char(p, end, zeroPad ? '0' : ' ');
    while (n > 0) out_char(p, end, tmp[--n]);
}

static void out_int(char **p, char *end, long v, int width, bool zeroPad) {
    if (v < 0) {
        out_char(p, end, '-');
        out_uint(p, end, (unsigned long)(-v), width > 0 ? width - 1 : 0, zeroPad);
    } else {
        out_uint(p, end, (unsigned long)v, width, zeroPad);
    }
}

int printf(const char *fmt, ...) {
    char buf[256];
    char *p = buf;
    char *end = buf + sizeof(buf) - 1;

    va_list ap;
    va_start(ap, fmt);
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') {
            out_char(&p, end, *f);
            continue;
        }
        f++;
        bool zeroPad = false;
        if (*f == '0') {
            zeroPad = true;
            f++;
        }
        int width = 0;
        while (*f >= '0' && *f <= '9') {
            width = width * 10 + (*f - '0');
            f++;
        }
        bool isLong = false;
        if (*f == 'l') {
            isLong = true;
            f++;
        }
        switch (*f) {
            case 's':
                out_str(&p, end, va_arg(ap, const char *));
                break;
            case 'd':
                out_int(&p, end, isLong ? va_arg(ap, long) : (long)va_arg(ap, int), width, zeroPad);
                break;
            case 'u':
                out_uint(&p, end, isLong ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int), width, zeroPad);
                break;
            case 'c':
                out_char(&p, end, (char)va_arg(ap, int));
                break;
            case '%':
                out_char(&p, end, '%');
                break;
            case '\0':
                f--; // do not step past the terminator below
                break;
            default:
                out_char(&p, end, '%');
                out_char(&p, end, *f);
                break;
        }
    }
    va_end(ap);
    *p = '\0';
    rt_log(buf);
    return 0;
}

/* ===========================================================================
 * runtime_core.h's three host hooks. See its header comment for why each
 * exists: none of the three pico-sdk calls they replace (printf's stdio
 * lock, time_us_32(), watchdog_update()+sleep_ms()) exist for this target.
 * ======================================================================= */

void rt_log(const char *msg) {
    js_log(msg, str_len(msg));
}

// nowMs is cached from the most recent emu_tick() call (see below); a
// switch's cost reads as 0us within that same tick, which is honest given
// wasm has no microsecond clock of its own - see runtime_core.h.
static uint32_t g_nowMs = 0;

uint32_t rt_time_us(void) {
    return g_nowMs * 1000u;
}

// Cannot literally hold forever the way the board does (a blocking loop
// inside one exported call would hang the browser tab - wasm has no
// preemption to yield through). Logs, then traps the module outright via an
// illegal instruction, which WebAssembly.instantiate's caller sees as a
// thrown RuntimeError. See runtime_core.h: this path is unreachable by any
// of today's shipped apps on either target, so the divergence from the
// board's "hold on a red screen" behaviour is never actually observed.
void rt_halt(void) {
    rt_log("FATAL: rt_halt() - see the preceding log line for why");
    __builtin_trap();
}

/* ===========================================================================
 * The panel: AMOLED_1IN8_Display / AMOLED_1IN8_DisplayWindows, per
 * shim/AMOLED_1in8.h. Record the pushed rectangle; the framebuffer itself
 * already holds the pixels (see emu_fb()), so there is nothing to draw here.
 * ======================================================================= */
#define MAX_PUSHES 128

static int g_pushX[MAX_PUSHES];
static int g_pushY[MAX_PUSHES];
static int g_pushW[MAX_PUSHES];
static int g_pushH[MAX_PUSHES];
static int g_pushCount = 0;

static void record_push(int x, int y, int w, int h) {
    if (g_pushCount >= MAX_PUSHES) return; // drop, same policy the real
                                            // touch/inject rings use when full
    g_pushX[g_pushCount] = x;
    g_pushY[g_pushCount] = y;
    g_pushW[g_pushCount] = w;
    g_pushH[g_pushCount] = h;
    g_pushCount++;
}

void AMOLED_1IN8_Display(UWORD *Image) {
    (void)Image; // already gfx_fb; see emu_fb()
    record_push(0, 0, AMOLED_1IN8_WIDTH, AMOLED_1IN8_HEIGHT);
}

void AMOLED_1IN8_DisplayWindows(uint32_t Xstart, uint32_t Ystart, uint32_t Xend, uint32_t Yend, UWORD *Image) {
    (void)Image;
    // Xend/Yend are exclusive (gfx.c's own comment on this call), so the
    // pushed window is [Xstart, Xend) x [Ystart, Yend).
    record_push((int)Xstart, (int)Ystart, (int)(Xend - Xstart), (int)(Yend - Ystart));
}

/* ===========================================================================
 * sensors.h, in full. See this file's header comment, job (2).
 * ======================================================================= */

/* ---- touch: a small ring, fed by emu_touch(), drained by
 * sensors_touch_next() - the same shape as the board's core1->core0 ring,
 * just single-threaded, so no barrier is needed. */
#define TOUCH_Q_CAP 16
static touch_sample_t g_touchQ[TOUCH_Q_CAP];
static uint32_t g_touchHead = 0, g_touchTail = 0;

bool sensors_touch_next(touch_sample_t *out) {
    if (g_touchHead == g_touchTail) return false;
    *out = g_touchQ[g_touchHead];
    g_touchHead = (g_touchHead + 1) % TOUCH_Q_CAP;
    return true;
}

static void touch_q_push(uint8_t fingers, uint16_t x, uint16_t y) {
    uint32_t next = (g_touchTail + 1) % TOUCH_Q_CAP;
    if (next == g_touchHead) return; // full: drop, agent/host can send it again
    g_touchQ[g_touchTail] = (touch_sample_t){ g_nowMs, fingers, x, y };
    g_touchTail = next;
}

// Declared by sensors.h for the board's devlink test link; nothing in the
// wasm-compiled sources calls it (devlink stays board-only, see
// docs/decisions/0003), but it is implemented anyway for API completeness -
// it is, after all, just another way to feed the same queue emu_touch() does.
void sensors_inject_touch(uint8_t fingers, uint16_t x, uint16_t y) {
    touch_q_push(fingers, x, y);
}

static bool g_fingerDown = false;

void sensors_set_finger_down(bool down) {
    g_fingerDown = down;
}

/* ---- PWR key: bits latched exactly like the real AXP2101 register 0x49
 * (see sensors.h), fed by emu_button()/emu_button_verdict() for button
 * index BTN_PWR. sensors_key_take() is read-and-clear, once per tick, same
 * contract as the board. */
#define BTN_BOOT 0
#define BTN_PWR  1

static uint8_t g_keyEvent = 0;

uint8_t sensors_key_take(void) {
    uint8_t ev = g_keyEvent;
    g_keyEvent = 0;
    return ev;
}

/* ---- BOOT: level tracked directly (no chip-select borrowing here - see
 * bootbtn.h on the board for what THAT costs; none of it applies to a
 * browser button), click derived on the release edge, same moment the
 * board calls a click. */
static bool g_bootLevel = false;
static bool g_bootClickedPending = false;
// Mirrors bootbtn.c's g_swallowNextClick: sensors_boot_consume_click() is
// called from runtime_core.c's chord handling WHILE BOOT is still held (the
// chord has just fired, BOOT has not been released yet), so a plain "clear
// whatever is pending now" would be a no-op here - the click this needs to
// swallow has not happened yet. Arming a flag that the NEXT release checks,
// same as the board, is what makes the order-of-calls work regardless of
// whether BOOT is currently up or down at the moment it is called.
static bool g_bootSwallowNextClick = false;

bool sensors_boot_clicked(void) {
    bool v = g_bootClickedPending;
    g_bootClickedPending = false;
    return v;
}

bool sensors_boot_down(void) {
    return g_bootLevel;
}

// No chip-select borrow to protect here (see sensors.h's comment on why this
// exists at all) - just arm the same one-shot swallow the board's
// bootbtn_consume_next_click() does.
void sensors_boot_consume_click(void) {
    g_bootSwallowNextClick = true;
}

/* ---- shake: one monotonic counter, bumped by emu_sensor_event() for the
 * declared "shake" sensor, suppressed while a finger is down - the same
 * rule sensors.h documents for the real IMU path, so a resting hand while
 * drawing cannot erase here either, even though nothing in wasm actually
 * measures acceleration; the host decides when a shake "happened" and this
 * file only enforces the same gate the board does. */
static uint32_t g_eraseSeq = 0;

uint32_t sensors_erase_seq(void) {
    return g_eraseSeq;
}

// Declared by sensors.h for devlink's ERASE command; not called from the
// wasm-compiled sources (see sensors_inject_touch()'s comment above), kept
// for the same completeness reason, wired to the identical counter
// emu_sensor_event() bumps.
void sensors_inject_erase(void) {
    if (!g_fingerDown) g_eraseSeq++;
}

/* ---- diagnostics: always zero. Nothing in wasm has i2c timeouts or queue
 * drops to count; see emu_abi.h's "What is not real" section. */
void sensors_stats(sensors_stats_t *out) {
    out->touchReads = 0;
    out->touchTimeouts = 0;
    out->touchQueueDrops = 0;
    out->touchRecoveries = 0;
    out->imuTimeouts = 0;
    out->pmicTimeouts = 0;
}

// sensors_init()/sensors_start() are deliberately NOT implemented here:
// nothing in the wasm build calls them (they are main()'s, board-only, and
// main() is not compiled for this target - see build.ts's source list), and
// there is no i2c1 to bring up in a browser regardless.

/* ===========================================================================
 * emu_abi.h
 * ======================================================================= */

int emu_init(void) {
    if (!gfx_init()) {
        rt_log("FATAL: emu_init: gfx_init() failed");
        return 0;
    }
    rtcore_init();
    return 1;
}

void emu_tick(uint32_t nowMs) {
    g_nowMs = nowMs;
    g_pushCount = 0;
    rtcore_tick(nowMs);
}

int emu_fb(void) {
    return (int)(intptr_t)gfx_fb;
}

int emu_push_count(void) { return g_pushCount; }
int emu_push_x(int i) { return g_pushX[i]; }
int emu_push_y(int i) { return g_pushY[i]; }
int emu_push_w(int i) { return g_pushW[i]; }
int emu_push_h(int i) { return g_pushH[i]; }

void emu_touch(int down, int x, int y) {
    touch_q_push(down ? 1 : 0, (uint16_t)x, (uint16_t)y);
}

void emu_button(int index, int down) {
    if (index == BTN_BOOT) {
        bool wasDown = g_bootLevel;
        g_bootLevel = (down != 0);
        if (wasDown && !g_bootLevel) { // click on release
            if (g_bootSwallowNextClick) {
                g_bootSwallowNextClick = false;
            } else {
                g_bootClickedPending = true;
            }
        }
    } else if (index == BTN_PWR) {
        g_keyEvent |= down ? KEY_PRESS : KEY_RELEASE;
    }
    // Any other index: emu_device() only ever declares two buttons, so this
    // would be a host bug; ignored rather than trapped, same policy
    // devlink_hook_app_switch() uses for a bad index on the board.
}

void emu_button_verdict(int index, int isLong) {
    // Only PWR declares longPressMs in emu_device(), so this is the only
    // index the host should ever call this for (see emu_abi.h).
    if (index == BTN_PWR) {
        g_keyEvent |= isLong ? KEY_LONG : KEY_SHORT;
    }
}

void emu_sensor_event(int index) {
    (void)index; // emu_device() declares exactly one sensor ("shake"), so
                 // there is nothing to switch on yet; see sensors_inject_erase().
    if (!g_fingerDown) g_eraseSeq++;
}

int emu_app_current(void) {
    return app_current();
}

void emu_app_switch(int index) {
    app_switch_to(index);
}

/* ---- emu_device(): built from what this board actually is, per
 * emu_abi.h's example JSON and AGENTS.md's "buttons, which the vendor
 * header describes wrongly" (PWR is the LOWER button). App names come from
 * g_apps[]/g_appCount (app.h), not hardcoded, so adding an app to the one
 * table in runtime_core.c makes it show up here too, same as decision
 * 0003's "adding an app means adding it to one table, and it appears in
 * both" promise. */
// 512 was tight even before "gestures" existed; the gesture's "how" prose
// alone is over 200 bytes. 1024 leaves real headroom.
static char g_deviceJson[1024];

static char *json_append(char *p, const char *s) {
    while (*s) *p++ = *s++;
    return p;
}

int emu_device(void) {
    char *p = g_deviceJson;
    p = json_append(p, "{\"name\":\"RP2350-Touch-AMOLED-1.8\",");
    p = json_append(p, "\"panel\":{\"w\":368,\"h\":448,\"format\":\"rgb565be\"},");
    p = json_append(p, "\"buttons\":[");
    p = json_append(p, "{\"id\":\"boot\",\"label\":\"BOOT\",\"edge\":\"right\",\"at\":0.38},");
    p = json_append(p, "{\"id\":\"pwr\",\"label\":\"PWR\",\"edge\":\"right\",\"at\":0.62,\"longPressMs\":1500}");
    p = json_append(p, "],");
    p = json_append(p, "\"touch\":{\"points\":1},");
    p = json_append(p, "\"sensors\":[{\"id\":\"shake\",\"kind\":\"event\"}],");
    p = json_append(p, "\"apps\":[");
    for (int i = 0; i < g_appCount; i++) {
        if (i > 0) p = json_append(p, ",");
        p = json_append(p, "\"");
        p = json_append(p, g_apps[i]->name);
        p = json_append(p, "\"");
    }
    p = json_append(p, "],");
    // The one compound gesture this runtime recognises (runtime_core.c,
    // "the BOOT+PWR long-press chord"): hold BOOT, then hold PWR until its
    // long-press verdict lands while BOOT is still down. Described in
    // device terms, not emulator terms, per emu_abi.h's note on
    // gestures[].how - the emulator supplies its own keyboard shortcuts for
    // "which key is BOOT" at render time.
    p = json_append(p, "\"gestures\":[{\"id\":\"menu\",\"label\":\"menu\",");
    p = json_append(p, "\"how\":\"Hold BOOT, then also hold PWR. Keep both held until PWR ");
    p = json_append(p, "registers a long press (about 1.5s): that opens the app menu. ");
    p = json_append(p, "Do the same chord again to close it and return to what was running.\"}]");
    p = json_append(p, "}");
    *p = '\0';
    return (int)(intptr_t)g_deviceJson;
}

/* ===========================================================================
 * DEV_Delay_ms: see shim/DEV_Config.h's header comment. apps/sketch.c's
 * wipe_erase() calls this to pace the shake-to-erase wipe band by band on
 * real hardware; in wasm it is a no-op, so the wipe completes within the
 * same emu_tick() instead of over ~240ms of real time. Consistent with
 * emu_abi.h's "Timing... is not real": pacing a redraw for visual effect is
 * exactly the kind of thing that section already rules out of this
 * emulator's job, and a real sleep here is not just unfaithful, it would
 * block the one JS thread the whole page runs on.
 * ======================================================================= */
void DEV_Delay_ms(uint32_t xms) {
    (void)xms;
}
