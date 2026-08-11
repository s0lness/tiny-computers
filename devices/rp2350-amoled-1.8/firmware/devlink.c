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
 * opposite order and the buffer is DMA'd out raw (see main.c's px_swap).
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
    } else {
        printf("ERR unknown %s\r\n", cmd);
    }
}

void devlink_poll(void) {
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
