/*
 * devlink implementation. See devlink.h for the integration contract and
 * tools/README-devlink.md for the wire protocol this speaks.
 *
 * Kept dependency-light on purpose: stdio/stdint/stdlib/string/ctype (all
 * plain C, already linked into every pico-sdk app via pico_stdlib) plus
 * pico/stdlib.h for getchar_timeout_us(). No new pico-sdk libraries.
 */
#include "devlink.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"

#define DEVLINK_VERSION   1
#define DEVLINK_LINE_MAX  96   // "MOVE -32768 -32768" style commands are
                                 // well under this; anything longer is
                                 // dropped and resynced on the next line.
#define DEVLINK_B64_WRAP  76   // chars per base64 line, per the spec

static devlink_hooks_t g_hooks;
static char g_line[DEVLINK_LINE_MAX];
static int g_lineLen = 0;

// Set by the CHORD command, serviced at the top of the NEXT devlink_poll()
// call. See devlink.h's devlink_poll() comment for why this cannot just
// release BOOT before returning from the CHORD command itself: the tick
// that has to see BOOT still held has not run yet at that point, and
// devlink_poll() must not block waiting for it to.
static bool g_chordReleasePending = false;

void devlink_init(const devlink_hooks_t *hooks) {
    g_hooks = *hooks;
    g_lineLen = 0;
}

/* ---------------------------------------------------------------------
 * Screenshot: greyscale + run-length encoding + base64.
 *
 * The panel is neutral grey throughout (white paper, black ink), so one
 * byte per pixel loses nothing that matters. Getting that byte out of a
 * stored RGB565 pixel needs the byte-swap undone first: the framebuffer is
 * kept byte-swapped relative to a CPU uint16_t because the panel wants the
 * opposite order and the buffer is DMA'd out raw (see gfx.h's px_swap).
 * ------------------------------------------------------------------- */
static inline uint8_t devlink_gray_at(int idx) {
    uint16_t px = g_hooks.fb[idx];
    uint16_t v = (uint16_t)((px >> 8) | (px << 8));
    return (uint8_t)(((v >> 5) & 0x3F) << 2);
}

// Walks the framebuffer as a stream of RLE (value, count) pairs, count
// clamped to 1..255, calling emit() for each pair. Used twice: once just to
// total the byte count (needed up front for the "SHOT w h count" header,
// before any base64 is written), once to actually stream it. Two full
// passes over the framebuffer are cheap; buffering the whole RLE stream
// would not be, next to a 330KB framebuffer in 520KB of SRAM.
typedef void (*devlink_rle_emit_fn)(uint8_t value, uint8_t count, void *ctx);

static void devlink_rle_walk(devlink_rle_emit_fn emit, void *ctx) {
    int n = g_hooks.w * g_hooks.h;
    if (n <= 0) return;
    uint8_t cur = devlink_gray_at(0);
    int runLen = 1;
    for (int i = 1; i < n; i++) {
        uint8_t g = devlink_gray_at(i);
        if (g == cur && runLen < 255) {
            runLen++;
        } else {
            emit(cur, (uint8_t)runLen, ctx);
            cur = g;
            runLen = 1;
        }
    }
    emit(cur, (uint8_t)runLen, ctx);
}

static void devlink_rle_count_cb(uint8_t value, uint8_t count, void *ctx) {
    (void)value;
    (void)count;
    uint32_t *total = (uint32_t *)ctx;
    *total += 2;
}

// Standard base64, streamed 3 input bytes -> 4 output chars at a time, with
// the padded final group handled the same way whether it is 1, 2 or 3 bytes
// long. Wraps output at DEVLINK_B64_WRAP chars/line as the protocol requires.
static const char devlink_b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

typedef struct {
    uint8_t buf[3];
    int bufLen;
    int lineCol;
} devlink_b64_stream_t;

static void devlink_b64_flush_group(devlink_b64_stream_t *s) {
    if (s->bufLen == 0) return;
    uint32_t n = ((uint32_t)s->buf[0] << 16) |
                 ((uint32_t)(s->bufLen > 1 ? s->buf[1] : 0) << 8) |
                 (uint32_t)(s->bufLen > 2 ? s->buf[2] : 0);
    char out[4];
    out[0] = devlink_b64_table[(n >> 18) & 0x3F];
    out[1] = devlink_b64_table[(n >> 12) & 0x3F];
    out[2] = s->bufLen > 1 ? devlink_b64_table[(n >> 6) & 0x3F] : '=';
    out[3] = s->bufLen > 2 ? devlink_b64_table[n & 0x3F] : '=';
    for (int i = 0; i < 4; i++) {
        putchar(out[i]);
        s->lineCol++;
        if (s->lineCol >= DEVLINK_B64_WRAP) {
            printf("\r\n");
            s->lineCol = 0;
        }
    }
    s->bufLen = 0;
}

static void devlink_b64_push_byte(devlink_b64_stream_t *s, uint8_t b) {
    s->buf[s->bufLen++] = b;
    if (s->bufLen == 3) devlink_b64_flush_group(s);
}

static void devlink_rle_emit_b64_cb(uint8_t value, uint8_t count, void *ctx) {
    devlink_b64_stream_t *s = (devlink_b64_stream_t *)ctx;
    devlink_b64_push_byte(s, value);
    devlink_b64_push_byte(s, count);
}

static void devlink_send_shot(void) {
    if (g_hooks.fb == NULL || g_hooks.w <= 0 || g_hooks.h <= 0) {
        printf("ERR no framebuffer\r\n");
        return;
    }

    uint32_t rleBytes = 0;
    devlink_rle_walk(devlink_rle_count_cb, &rleBytes);

    printf("SHOT %d %d %lu\r\n", g_hooks.w, g_hooks.h,
           (unsigned long)rleBytes);

    devlink_b64_stream_t s;
    s.bufLen = 0;
    s.lineCol = 0;
    devlink_rle_walk(devlink_rle_emit_b64_cb, &s);
    devlink_b64_flush_group(&s); // final partial group, if any
    if (s.lineCol != 0) printf("\r\n");
    printf("END\r\n");
}

/* ---------------------------------------------------------------------
 * Line protocol.
 * ------------------------------------------------------------------- */

// Parses two whitespace-separated ints from s. Returns how many it found
// (0, 1 or 2) so the caller can tell "DOWN" with no args from "DOWN 5" from
// "DOWN 5 6".
static int devlink_parse_two_ints(const char *s, int *a, int *b) {
    while (*s == ' ') s++;
    char *end;
    long va = strtol(s, &end, 10);
    if (end == s) return 0;
    s = end;
    while (*s == ' ') s++;
    long vb = strtol(s, &end, 10);
    if (end == s) return 1;
    *a = (int)va;
    *b = (int)vb;
    return 2;
}

// Parses one whitespace-separated int from s. Returns false if none found;
// used by SWITCH, which takes just an index rather than DOWN/MOVE's x/y pair.
static bool devlink_parse_one_int(const char *s, int *a) {
    while (*s == ' ') s++;
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s) return false;
    *a = (int)v;
    return true;
}

// Case-insensitive match of a single leading word in s against word (which
// must already be uppercase). Used by KEY/BOOT's named subcommands instead
// of strcasecmp (not pulled in anywhere else in this file, and this needs
// the "must end at a word boundary" check regardless, so a real match cannot
// accept "SHORTISH" as "SHORT"). Leading spaces in s are skipped; trailing
// content after the matched word is not consumed or inspected here.
static bool devlink_word_is(const char *s, const char *word) {
    while (*s == ' ') s++;
    size_t i = 0;
    for (; word[i]; i++) {
        if (toupper((unsigned char)s[i]) != (unsigned char)word[i]) return false;
    }
    char after = s[i];
    return after == '\0' || after == ' ';
}

static void devlink_dispatch(char *line) {
    char *p = line;
    while (*p == ' ') p++;
    char *cmdStart = p;
    while (*p && *p != ' ') p++;
    size_t cmdLen = (size_t)(p - cmdStart);

    char args_sep = *p;
    if (args_sep) *p++ = '\0'; // terminate the command token in place
    char *args = p;
    while (*args == ' ') args++;

    if (cmdLen == 0) return; // blank line: ignore silently

    // Uppercase a bounded copy for comparison; the original casing (still in
    // cmdStart) is echoed back in the "unknown command" error.
    char cmd[16];
    if (cmdLen >= sizeof(cmd)) {
        printf("ERR unknown %s\r\n", cmdStart);
        return;
    }
    for (size_t i = 0; i < cmdLen; i++) {
        cmd[i] = (char)toupper((unsigned char)cmdStart[i]);
    }
    cmd[cmdLen] = '\0';

    if (strcmp(cmd, "PING") == 0) {
        printf("OK devlink %d %d %d\r\n", DEVLINK_VERSION, g_hooks.w, g_hooks.h);
    } else if (strcmp(cmd, "SHOT") == 0) {
        devlink_send_shot();
    } else if (strcmp(cmd, "DOWN") == 0) {
        int x, y;
        if (devlink_parse_two_ints(args, &x, &y) != 2) { printf("ERR args\r\n"); return; }
        if (g_hooks.inject_down) g_hooks.inject_down(x, y);
        printf("OK\r\n");
    } else if (strcmp(cmd, "MOVE") == 0) {
        int x, y;
        if (devlink_parse_two_ints(args, &x, &y) != 2) { printf("ERR args\r\n"); return; }
        if (g_hooks.inject_move) g_hooks.inject_move(x, y);
        printf("OK\r\n");
    } else if (strcmp(cmd, "UP") == 0) {
        if (g_hooks.inject_up) g_hooks.inject_up();
        printf("OK\r\n");
    } else if (strcmp(cmd, "TAP") == 0) {
        int x, y;
        if (devlink_parse_two_ints(args, &x, &y) != 2) { printf("ERR args\r\n"); return; }
        if (g_hooks.inject_down) g_hooks.inject_down(x, y);
        if (g_hooks.inject_up) g_hooks.inject_up();
        printf("OK\r\n");
    } else if (strcmp(cmd, "ERASE") == 0) {
        if (g_hooks.erase) g_hooks.erase();
        printf("OK\r\n");
    } else if (strcmp(cmd, "KEY") == 0) {
        // Named forms only, no raw hex mask: see tools/README-devlink.md's
        // KEY section for why (a mistyped mask at a prompt silently injects
        // the wrong gesture; a misspelled name just fails).
        devlink_key_t which;
        if (devlink_word_is(args, "PRESS")) which = DEVLINK_KEY_PRESS;
        else if (devlink_word_is(args, "LONG")) which = DEVLINK_KEY_LONG;
        else if (devlink_word_is(args, "SHORT")) which = DEVLINK_KEY_SHORT;
        else if (devlink_word_is(args, "RELEASE")) which = DEVLINK_KEY_RELEASE;
        else { printf("ERR args\r\n"); return; }
        if (g_hooks.inject_key) g_hooks.inject_key(which);
        printf("OK\r\n");
    } else if (strcmp(cmd, "BOOT") == 0) {
        if (devlink_word_is(args, "DOWN")) {
            if (g_hooks.inject_boot) g_hooks.inject_boot(true);
        } else if (devlink_word_is(args, "UP")) {
            if (g_hooks.inject_boot) g_hooks.inject_boot(false);
        } else if (devlink_word_is(args, "CLICK")) {
            if (g_hooks.inject_boot_click) g_hooks.inject_boot_click();
        } else {
            printf("ERR args\r\n");
            return;
        }
        printf("OK\r\n");
    } else if (strcmp(cmd, "CHORD") == 0) {
        // The BOOT+PWR menu chord, composed from the same primitives as
        // BOOT/KEY above and no more: hold BOOT, deliver PWR's long-press
        // verdict, release BOOT. See runtime_core.c's chord comment for what
        // this reproduces at the firmware's input layer, and sensors.h's
        // sensors_inject_key()/sensors_inject_boot() for what it does NOT
        // reproduce (the PMIC register read and the flash-chip-select
        // borrow that a real press goes through first).
        if (g_hooks.inject_boot) g_hooks.inject_boot(true);
        if (g_hooks.inject_key) g_hooks.inject_key(DEVLINK_KEY_LONG);
        g_chordReleasePending = true;
        printf("OK\r\n");
    } else if (strcmp(cmd, "APP") == 0) {
        int idx = g_hooks.app_current ? g_hooks.app_current() : 0;
        const char *name = g_hooks.app_name ? g_hooks.app_name(idx) : NULL;
        printf("APP %d %s\r\n", idx, name ? name : "?");
    } else if (strcmp(cmd, "SWITCH") == 0) {
        int idx;
        if (!devlink_parse_one_int(args, &idx)) { printf("ERR args\r\n"); return; }
        bool ok = g_hooks.app_switch ? g_hooks.app_switch(idx) : false;
        printf(ok ? "OK\r\n" : "ERR range\r\n");
    } else {
        printf("ERR unknown %s\r\n", cmd);
    }
}

void devlink_poll(void) {
    // Release BOOT from a CHORD one tick after it was asserted (see
    // devlink.h's devlink_poll() comment and the CHORD case in
    // devlink_dispatch() above). This runs before anything is drained off
    // the wire, so it lands as early as possible in this iteration, strictly
    // after the caller's rtcore_tick() for this iteration has already run
    // (see runtime.c's main loop: rtcore_tick() then devlink_poll(), every
    // iteration) and strictly after the iteration where CHORD set the flag.
    if (g_chordReleasePending) {
        g_chordReleasePending = false;
        if (g_hooks.inject_boot) g_hooks.inject_boot(false);
    }

    for (;;) {
        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) return; // nothing queued right now

        if (c == '\n' || c == '\r') {
            if (g_lineLen > 0) {
                g_line[g_lineLen] = '\0';
                devlink_dispatch(g_line);
                g_lineLen = 0;
            }
            continue;
        }

        if (g_lineLen < DEVLINK_LINE_MAX - 1) {
            g_line[g_lineLen++] = (char)c;
        } else {
            // Overlong line: drop what we have and resync on the next
            // terminator rather than acting on a truncated command.
            g_lineLen = 0;
        }
    }
}
