/*
 * fwtool - firmware binary viewer / patcher / serial flasher (command line)
 *
 *   fwtool info  <file>
 *   fwtool dump  <file> [offset] [length|all]
 *   fwtool patch <file> <offset> <hex bytes...> [-o out.bin]
 *   fwtool ports
 *   fwtool flash <file> <port> [options]
 *   fwtool edit  <file>                       (interactive mode)
 *
 * All offsets and lengths are hexadecimal ("0x" prefix optional).
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L   /* fileno, isatty */
#endif

#include <ctype.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#define fw_isatty _isatty
#define fw_fileno _fileno
#else
#include <unistd.h>
#define fw_isatty isatty
#define fw_fileno fileno
#endif

#include "flash.h"
#include "serial.h"

#define FWTOOL_VERSION "0.1.0"
#define BYTES_PER_ROW 16
#define MAX_TOKENS 256

/* ================================================================ colors */
static int g_color = 0;
#define COL_RED    (g_color ? "\x1b[1;31m" : "")
#define COL_GRAY   (g_color ? "\x1b[90m" : "")
#define COL_CYAN   (g_color ? "\x1b[36m" : "")
#define COL_YELLOW (g_color ? "\x1b[33m" : "")
#define COL_RESET  (g_color ? "\x1b[0m" : "")

static void init_color(int disabled)
{
    if (disabled || getenv("NO_COLOR") || !fw_isatty(fw_fileno(stdout)))
        return;
#ifdef _WIN32
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode;
        if (!GetConsoleMode(h, &mode))
            return;
        if (!SetConsoleMode(h, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */))
            return;
    }
#endif
    g_color = 1;
}

static void on_sigint(int sig)
{
    (void)sig;
    g_cancel = 1;
    signal(SIGINT, on_sigint);   /* Windows CRT resets the handler after each signal */
}

/* ================================================================ buffer */
typedef struct {
    size_t off;
    uint8_t old;
    unsigned group;
} change_t;

typedef struct {
    char path[1024];
    uint8_t *data;
    uint8_t *orig;      /* content as last loaded/saved */
    size_t size;
    change_t *undo;
    size_t undo_n, undo_cap;
    unsigned group;
} fwbuf;

static void buf_free(fwbuf *b)
{
    free(b->data);
    free(b->orig);
    free(b->undo);
    memset(b, 0, sizeof *b);
}

static int buf_load(fwbuf *b, const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;
    memset(b, 0, sizeof *b);
    if (!f) {
        perror(path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "%s: cannot determine file size\n", path);
        fclose(f);
        return -1;
    }
    b->size = (size_t)sz;
    b->data = (uint8_t *)malloc(b->size ? b->size : 1);
    b->orig = (uint8_t *)malloc(b->size ? b->size : 1);
    if (!b->data || !b->orig) {
        fprintf(stderr, "Out of memory\n");
        fclose(f);
        buf_free(b);
        return -1;
    }
    if (fread(b->data, 1, b->size, f) != b->size) {
        fprintf(stderr, "%s: read error\n", path);
        fclose(f);
        buf_free(b);
        return -1;
    }
    fclose(f);
    memcpy(b->orig, b->data, b->size);
    snprintf(b->path, sizeof b->path, "%s", path);
    return 0;
}

static int buf_save(fwbuf *b, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return -1;
    }
    {
        int write_ok = fwrite(b->data, 1, b->size, f) == b->size;
        int close_ok = fclose(f) == 0;   /* always close, even after a failed write */
        if (!write_ok || !close_ok) {
            fprintf(stderr, "%s: write error\n", path);
            return -1;
        }
    }
    memcpy(b->orig, b->data, b->size);
    if (path != b->path)
        snprintf(b->path, sizeof b->path, "%s", path);
    printf("Saved %zu bytes to %s\n", b->size, path);
    return 0;
}

static size_t buf_modified(const fwbuf *b)
{
    size_t i, n = 0;
    for (i = 0; i < b->size; i++)
        n += b->data[i] != b->orig[i];
    return n;
}

static void buf_begin_group(fwbuf *b)
{
    b->group++;
}

static void buf_set(fwbuf *b, size_t off, uint8_t v)
{
    if (off >= b->size || b->data[off] == v)
        return;
    if (b->undo_n == b->undo_cap) {
        size_t cap = b->undo_cap ? b->undo_cap * 2 : 256;
        change_t *nu = (change_t *)realloc(b->undo, cap * sizeof *nu);
        if (!nu) {
            fprintf(stderr, "Out of memory (undo history)\n");
            return;
        }
        b->undo = nu;
        b->undo_cap = cap;
    }
    b->undo[b->undo_n].off = off;
    b->undo[b->undo_n].old = b->data[off];
    b->undo[b->undo_n].group = b->group;
    b->undo_n++;
    b->data[off] = v;
}

/* Reverts the last write operation. Returns the number of bytes restored and
 * the affected range [*first_off, *last_off] (unchanged bytes may lie in between). */
static size_t buf_undo(fwbuf *b, size_t *first_off, size_t *last_off)
{
    size_t n = 0;
    unsigned g;
    if (!b->undo_n)
        return 0;
    g = b->undo[b->undo_n - 1].group;
    *first_off = SIZE_MAX;
    *last_off = 0;
    while (b->undo_n && b->undo[b->undo_n - 1].group == g) {
        change_t *c = &b->undo[--b->undo_n];
        b->data[c->off] = c->old;
        if (c->off < *first_off)
            *first_off = c->off;
        if (c->off > *last_off)
            *last_off = c->off;
        n++;
    }
    return n;
}

/* ================================================================ helpers */
static uint32_t crc32_calc(const uint8_t *d, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        crc ^= d[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static int parse_hex_number(const char *s, size_t *out)
{
    size_t v = 0;
    int digits = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    for (; *s; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else if (*s == '_') continue;
        else return -1;
        if (v > (SIZE_MAX >> 4))
            return -1;
        v = (v << 4) | (size_t)d;
        digits++;
    }
    if (!digits)
        return -1;
    *out = v;
    return 0;
}

/* Parses tokens like "DE AD", "DEADBEEF", "0xDE,0xAD", "de:ad" into bytes. Caller frees *out. */
static int parse_hex_bytes(int argc, char **argv, uint8_t **out, size_t *outlen)
{
    size_t cap = 64, n = 0;
    int i, half = -1;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf)
        return -1;
    for (i = 0; i < argc; i++) {
        const char *s = argv[i];
        while (*s) {
            int d;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && half < 0) {
                s += 2;
                continue;
            }
            if (*s == ',' || *s == ':' || *s == ' ' || *s == '\t' || *s == '-' || *s == '_') {
                s++;
                continue;
            }
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else {
                fprintf(stderr, "Invalid hex character '%c' in \"%s\"\n", *s, argv[i]);
                free(buf);
                return -1;
            }
            if (half < 0) {
                half = d;
            } else {
                if (n == cap) {
                    uint8_t *nb = (uint8_t *)realloc(buf, cap * 2);
                    if (!nb) {
                        free(buf);
                        return -1;
                    }
                    buf = nb;
                    cap *= 2;
                }
                buf[n++] = (uint8_t)((half << 4) | d);
                half = -1;
            }
            s++;
        }
    }
    if (half >= 0) {
        fprintf(stderr, "Odd number of hex digits\n");
        free(buf);
        return -1;
    }
    if (n == 0) {
        fprintf(stderr, "No bytes given\n");
        free(buf);
        return -1;
    }
    *out = buf;
    *outlen = n;
    return 0;
}

static int ask_yes_no(const char *question)
{
    char line[64];
    printf("%s [y/N] ", question);
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin))
        return 0;
    return line[0] == 'y' || line[0] == 'Y' || line[0] == 'j' || line[0] == 'J';
}

/* ================================================================ output */
static void print_info(const fwbuf *b)
{
    size_t i;
    uint8_t sum = 0;
    for (i = 0; i < b->size; i++)
        sum = (uint8_t)(sum + b->data[i]);
    printf("File:      %s\n", b->path);
    printf("Size:      %zu bytes (0x%zX)\n", b->size, b->size);
    printf("CRC32:     0x%08X\n", (unsigned)crc32_calc(b->data, b->size));
    printf("Sum8:      0x%02X\n", sum);
    printf("Modified:  %zu byte(s)\n", buf_modified(b));
}

static void hexdump(const fwbuf *b, size_t off, size_t len)
{
    size_t row, end, i;
    if (off >= b->size) {
        printf("Offset 0x%zX is beyond end of file (size 0x%zX)\n", off, b->size);
        return;
    }
    end = (len > b->size - off) ? b->size : off + len;

    printf("%sOffset     ", COL_CYAN);
    for (i = 0; i < BYTES_PER_ROW; i++)
        printf("%02X %s", (unsigned)i, i == 7 ? " " : "");
    printf(" ASCII%s\n", COL_RESET);

    for (row = off - off % BYTES_PER_ROW; row < end; row += BYTES_PER_ROW) {
        int row_mod = 0;
        for (i = 0; i < BYTES_PER_ROW && row + i < b->size; i++)
            if (b->data[row + i] != b->orig[row + i])
                row_mod = 1;

        printf("%s%08zX%s%s%c%s ", COL_GRAY, row, COL_RESET, COL_RED, row_mod ? '*' : ' ', COL_RESET);
        for (i = 0; i < BYTES_PER_ROW; i++) {
            size_t p = row + i;
            if (p < off || p >= end) {
                printf("   ");
            } else if (b->data[p] != b->orig[p]) {
                printf("%s%02X%s ", COL_RED, b->data[p], COL_RESET);
            } else {
                printf("%02X ", b->data[p]);
            }
            if (i == 7)
                printf(" ");
        }
        printf(" ");
        for (i = 0; i < BYTES_PER_ROW; i++) {
            size_t p = row + i;
            if (p < off || p >= end) {
                printf(" ");
            } else {
                uint8_t c = b->data[p];
                char ch = (c >= 32 && c < 127) ? (char)c : '.';
                if (b->data[p] != b->orig[p])
                    printf("%s%c%s", COL_RED, ch, COL_RESET);
                else
                    printf("%s%c%s", COL_YELLOW, ch, COL_RESET);
            }
        }
        printf("\n");
    }
}

static void print_diff(const fwbuf *b)
{
    size_t i, shown = 0, total = buf_modified(b);
    if (!total) {
        printf("No changes.\n");
        return;
    }
    for (i = 0; i < b->size && shown < 200; i++) {
        if (b->data[i] != b->orig[i]) {
            printf("0x%08zX: %02X -> %s%02X%s\n", i, b->orig[i], COL_RED, b->data[i], COL_RESET);
            shown++;
        }
    }
    if (total > shown)
        printf("... and %zu more\n", total - shown);
}

static void print_find(const fwbuf *b, const uint8_t *pat, size_t n)
{
    size_t i, hits = 0;
    if (n > b->size) {
        printf("Not found.\n");
        return;
    }
    for (i = 0; i + n <= b->size; i++) {
        if (memcmp(b->data + i, pat, n) == 0) {
            if (hits < 50)
                printf("  found at 0x%08zX\n", i);
            hits++;
        }
    }
    if (!hits)
        printf("Not found.\n");
    else if (hits > 50)
        printf("  ... %zu matches total\n", hits);
    else
        printf("%zu match(es)\n", hits);
}

static void port_cb(const char *name, const char *desc, void *user)
{
    (void)user;
    printf("  %-20s %s\n", name, desc);
}

static void list_ports(void)
{
    int n;
    printf("Available serial ports:\n");
    n = serial_list_ports(port_cb, NULL);
    if (n == 0)
        printf("  (none found)\n");
}

/* Writes bytes at offset as one undo group. Returns 0 on success. */
static int write_bytes(fwbuf *b, size_t off, const uint8_t *v, size_t n)
{
    size_t i;
    if (off >= b->size || n > b->size - off) {
        fprintf(stderr, "Range 0x%zX..0x%zX is outside the file (size 0x%zX)\n", off, off + n - 1, b->size);
        return -1;
    }
    buf_begin_group(b);
    for (i = 0; i < n; i++)
        buf_set(b, off + i, v[i]);
    return 0;
}

/* Optionally asks for confirmation, then flashes the buffer. Returns 0 on success. */
static int do_flash(fwbuf *b, flash_opts *o, int confirm)
{
    char q[512];
    int rc;
    if (confirm) {
        size_t mod = buf_modified(b);
        if (mod)
            printf("Note: %zu modified byte(s) (unsaved) - the current buffer will be sent.\n", mod);
        snprintf(q, sizeof q, "Send %zu bytes to %s @ %ld baud (%s)?", b->size, o->port, o->baud,
                 flash_proto_name(o->proto));
        if (!ask_yes_no(q)) {
            printf("Aborted.\n");
            return -1;
        }
    }
    g_cancel = 0;
    rc = flash_run(o, b->data, b->size);
    g_cancel = 0;
    return rc;
}

/* ================================================================ usage */
static void usage(void)
{
    printf("fwtool " FWTOOL_VERSION " - firmware viewer, patcher and serial flasher\n\n"
           "Usage:\n"
           "  fwtool info  <file>                              show size, CRC32, checksum\n"
           "  fwtool dump  <file> [offset] [length|all]        hex dump (default length 0x100)\n"
           "  fwtool patch <file> <offset> <hex...> [-o out]   overwrite bytes (in place or to out)\n"
           "  fwtool ports                                     list serial ports\n"
           "  fwtool flash <file> <port> [options]             transfer firmware\n"
           "  fwtool edit  <file>                              interactive editor / flasher\n\n"
           "All offsets and lengths are hexadecimal (0x prefix optional).\n\n"
           "Flash options:\n");
    flash_print_option_help();
    printf("      --patch OFF=HEX   patch bytes in memory before flashing (repeatable)\n"
           "  -y, --yes             do not ask for confirmation\n\n"
           "Global options:\n"
           "      --no-color        disable colored output\n\n"
           "Examples:\n"
           "  fwtool dump firmware.bin 1F0 40\n"
           "  fwtool patch firmware.bin 0x1A0 DE AD BE EF -o patched.bin\n"
#ifdef _WIN32
           "  fwtool flash firmware.bin COM3 -b 115200 -p xmodem\n"
#else
           "  fwtool flash firmware.bin /dev/ttyUSB0 -b 115200 -p xmodem\n"
#endif
           );
}

static void interactive_help(void)
{
    printf("Commands (offsets/lengths in hex):\n"
           "  d [off] [len]          hex dump (without args: continue)\n"
           "  w <off> <hex...>       write bytes, e.g.  w 1A0 DE AD BE EF\n"
           "  fill <off> <len> <xx>  fill range with byte value\n"
           "  find <hex...>          search byte pattern\n"
           "  u                      undo last write\n"
           "  diff                   list modified bytes\n"
           "  revert                 discard all unsaved changes\n"
           "  i                      file info (size, CRC32, ...)\n"
           "  s [file]               save (optionally under new name)\n"
           "  ports                  list serial ports\n"
           "  flash <port> [opts]    flash current buffer, opts as on command line:\n");
    flash_print_option_help();
    printf("  h / ?                  this help\n"
           "  q                      quit\n");
}

/* ============================================================ interactive */
static int tokenize(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"')
                p++;
        } else {
            argv[argc++] = p;
            while (*p && !isspace((unsigned char)*p))
                p++;
        }
        if (*p)
            *p++ = '\0';
    }
    return argc;
}

static int is_cmd(const char *s, const char *a, const char *b)
{
    return !strcmp(s, a) || (b && !strcmp(s, b));
}

static int cmd_edit(const char *path)
{
    fwbuf b;
    char line[4096];
    char *argv[MAX_TOKENS];
    size_t next_off = 0;

    if (buf_load(&b, path) != 0)
        return 1;
    print_info(&b);
    printf("\nType 'h' for help.\n");

    for (;;) {
        int argc;
        const char *c;

        printf("%sfwtool%s%s> ", COL_CYAN, buf_modified(&b) ? "*" : "", COL_RESET);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) {
#ifdef _WIN32
            /* Ctrl+C makes fgets return NULL immediately, but the CRT runs the
             * SIGINT handler on a separate thread - give it time to set g_cancel,
             * otherwise Ctrl+C is mistaken for EOF and unsaved changes are lost. */
            if (!g_cancel)
                Sleep(100);
#endif
            if (g_cancel) {          /* Ctrl+C on Windows interrupts fgets */
                g_cancel = 0;
                clearerr(stdin);
                printf("\n(use 'q' to quit)\n");
                continue;
            }
            if (buf_modified(&b))
                printf("\nEOF - unsaved changes discarded.\n");
            break;
        }
        g_cancel = 0;
        argc = tokenize(line, argv, MAX_TOKENS);
        if (!argc)
            continue;
        c = argv[0];

        if (is_cmd(c, "h", "help") || !strcmp(c, "?")) {
            interactive_help();
        } else if (is_cmd(c, "q", "quit") || !strcmp(c, "exit")) {
            if (buf_modified(&b) && !ask_yes_no("Unsaved changes. Quit anyway?"))
                continue;
            break;
        } else if (is_cmd(c, "i", "info")) {
            print_info(&b);
        } else if (is_cmd(c, "d", "dump")) {
            size_t off = next_off, len = 0x100;
            if (argc > 1 && parse_hex_number(argv[1], &off) != 0) {
                printf("Invalid offset\n");
                continue;
            }
            if (argc > 2 && parse_hex_number(argv[2], &len) != 0) {
                printf("Invalid length\n");
                continue;
            }
            if (b.size == 0) {
                printf("File is empty.\n");
                continue;
            }
            if (off >= b.size && argc == 1)
                off = 0;   /* wrap around when continuing past the end */
            hexdump(&b, off, len);
            /* avoid size_t overflow for huge lengths */
            next_off = (off < b.size && len < b.size - off) ? off + len : b.size;
        } else if (is_cmd(c, "w", "write")) {
            size_t off, n;
            uint8_t *v;
            if (argc < 3 || parse_hex_number(argv[1], &off) != 0) {
                printf("Usage: w <offset> <hex bytes...>\n");
                continue;
            }
            if (parse_hex_bytes(argc - 2, argv + 2, &v, &n) != 0)
                continue;
            if (write_bytes(&b, off, v, n) == 0) {
                printf("Wrote %zu byte(s) at 0x%08zX\n", n, off);
                hexdump(&b, off, n);
            }
            free(v);
        } else if (!strcmp(c, "fill")) {
            size_t off, len, val, i;
            uint8_t *v;
            if (argc != 4 || parse_hex_number(argv[1], &off) || parse_hex_number(argv[2], &len) ||
                parse_hex_number(argv[3], &val) || val > 0xFF || len == 0) {
                printf("Usage: fill <offset> <length> <byte>\n");
                continue;
            }
            if (off >= b.size || len > b.size - off) {   /* check before allocating */
                printf("Range is outside the file (size 0x%zX)\n", b.size);
                continue;
            }
            v = (uint8_t *)malloc(len);
            if (!v) {
                printf("Out of memory\n");
                continue;
            }
            for (i = 0; i < len; i++)
                v[i] = (uint8_t)val;
            if (write_bytes(&b, off, v, len) == 0)
                printf("Filled 0x%zX byte(s) at 0x%08zX with 0x%02X\n", len, off, (unsigned)val);
            free(v);
        } else if (!strcmp(c, "find")) {
            size_t n;
            uint8_t *v;
            if (argc < 2) {
                printf("Usage: find <hex bytes...>\n");
                continue;
            }
            if (parse_hex_bytes(argc - 1, argv + 1, &v, &n) != 0)
                continue;
            print_find(&b, v, n);
            free(v);
        } else if (is_cmd(c, "u", "undo")) {
            size_t first = 0, last = 0, n = buf_undo(&b, &first, &last);
            if (!n) {
                printf("Nothing to undo.\n");
            } else {
                printf("Undone: %zu byte(s)\n", n);
                hexdump(&b, first, last - first + 1);
            }
        } else if (!strcmp(c, "diff")) {
            print_diff(&b);
        } else if (!strcmp(c, "revert")) {
            if (buf_modified(&b) && ask_yes_no("Discard all unsaved changes?")) {
                memcpy(b.data, b.orig, b.size);
                b.undo_n = 0;
                printf("Reverted.\n");
            }
        } else if (is_cmd(c, "s", "save")) {
            if (argc > 1)
                buf_save(&b, argv[1]);
            else
                buf_save(&b, b.path);
        } else if (!strcmp(c, "ports")) {
            list_ports();
        } else if (!strcmp(c, "flash")) {
            flash_opts o;
            char err[256];
            int i, ok = 1;
            if (argc < 2) {
                printf("Usage: flash <port> [options]   (see 'h')\n");
                continue;
            }
            flash_opts_default(&o);
            o.port = argv[1];
            for (i = 2; i < argc; i++) {
                int r = flash_parse_option(&o, argc, argv, &i, err, sizeof err);
                if (r < 0) {
                    printf("%s\n", err);
                    ok = 0;
                    break;
                }
                if (r == 0) {
                    printf("Unknown option: %s\n", argv[i]);
                    ok = 0;
                    break;
                }
            }
            if (ok)
                do_flash(&b, &o, 1);
        } else {
            printf("Unknown command '%s' - type 'h' for help\n", c);
        }
    }
    buf_free(&b);
    return 0;
}

/* ============================================================== commands */
static int cmd_info(int argc, char **argv)
{
    fwbuf b;
    if (argc != 1) {
        usage();
        return 2;
    }
    if (buf_load(&b, argv[0]) != 0)
        return 1;
    print_info(&b);
    buf_free(&b);
    return 0;
}

static int cmd_dump(int argc, char **argv)
{
    fwbuf b;
    size_t off = 0, len = 0x100;
    if (argc < 1 || argc > 3) {
        usage();
        return 2;
    }
    if (argc > 1 && parse_hex_number(argv[1], &off) != 0) {
        fprintf(stderr, "Invalid offset: %s\n", argv[1]);
        return 2;
    }
    if (argc > 2) {
        if (!strcmp(argv[2], "all"))
            len = SIZE_MAX;
        else if (parse_hex_number(argv[2], &len) != 0) {
            fprintf(stderr, "Invalid length: %s\n", argv[2]);
            return 2;
        }
    }
    if (buf_load(&b, argv[0]) != 0)
        return 1;
    if (b.size == 0)
        printf("File is empty.\n");
    else
        hexdump(&b, off, len);
    buf_free(&b);
    return 0;
}

static int cmd_patch(int argc, char **argv)
{
    fwbuf b;
    const char *out = NULL;
    char *args[MAX_TOKENS];
    int i, n = 0, rc = 1;
    size_t off, cnt;
    uint8_t *v;

    for (i = 0; i < argc && n < MAX_TOKENS; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else
            args[n++] = argv[i];
    }
    if (n < 3) {
        usage();
        return 2;
    }
    if (parse_hex_number(args[1], &off) != 0) {
        fprintf(stderr, "Invalid offset: %s\n", args[1]);
        return 2;
    }
    if (parse_hex_bytes(n - 2, args + 2, &v, &cnt) != 0)
        return 2;
    if (buf_load(&b, args[0]) != 0) {
        free(v);
        return 1;
    }
    if (write_bytes(&b, off, v, cnt) == 0) {
        hexdump(&b, off, cnt);
        rc = buf_save(&b, out ? out : b.path) == 0 ? 0 : 1;
    }
    free(v);
    buf_free(&b);
    return rc;
}

static int cmd_flash(int argc, char **argv)
{
    fwbuf b;
    flash_opts o;
    char err[256];
    int i, yes = 0, rc;

    if (argc < 2) {
        usage();
        return 2;
    }
    if (buf_load(&b, argv[0]) != 0)
        return 1;
    flash_opts_default(&o);
    o.port = argv[1];

    for (i = 2; i < argc; i++) {
        int r;
        if (!strcmp(argv[i], "-y") || !strcmp(argv[i], "--yes")) {
            yes = 1;
            continue;
        }
        if (!strcmp(argv[i], "--patch")) {
            char spec[512], *eq;
            size_t off, cnt;
            uint8_t *v;
            char *hexarg[1];
            if (i + 1 >= argc) {
                fprintf(stderr, "--patch requires OFF=HEX\n");
                buf_free(&b);
                return 2;
            }
            snprintf(spec, sizeof spec, "%s", argv[++i]);
            eq = strchr(spec, '=');
            if (!eq) {
                fprintf(stderr, "--patch expects OFF=HEX, got \"%s\"\n", spec);
                buf_free(&b);
                return 2;
            }
            *eq = '\0';
            hexarg[0] = eq + 1;
            if (parse_hex_number(spec, &off) != 0 || parse_hex_bytes(1, hexarg, &v, &cnt) != 0) {
                fprintf(stderr, "Invalid --patch value\n");
                buf_free(&b);
                return 2;
            }
            if (write_bytes(&b, off, v, cnt) != 0) {
                /* never flash an image whose requested patch could not be applied */
                free(v);
                buf_free(&b);
                return 2;
            }
            printf("Patched %zu byte(s) at 0x%08zX (in memory)\n", cnt, off);
            free(v);
            continue;
        }
        r = flash_parse_option(&o, argc, argv, &i, err, sizeof err);
        if (r <= 0) {
            fprintf(stderr, "%s\n", r < 0 ? err : "Unknown option");
            if (r == 0)
                fprintf(stderr, "  -> %s\n", argv[i]);
            buf_free(&b);
            return 2;
        }
    }

    rc = do_flash(&b, &o, !yes);
    buf_free(&b);
    return rc == 0 ? 0 : 1;
}

/* ================================================================== main */
int main(int argc, char **argv)
{
    int i, n = 0, no_color = 0;
    char **args = (char **)malloc(sizeof(char *) * (size_t)(argc + 1));
    int rc;

    if (!args)
        return 1;
    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--no-color"))
            no_color = 1;
        else
            args[n++] = argv[i];
    }
    args[n] = NULL;
    init_color(no_color);
    signal(SIGINT, on_sigint);

    if (n < 2 || is_cmd(args[1], "-h", "--help") || !strcmp(args[1], "help")) {
        usage();
        rc = n < 2 ? 2 : 0;
    } else if (!strcmp(args[1], "--version")) {
        printf("fwtool %s\n", FWTOOL_VERSION);
        rc = 0;
    } else if (!strcmp(args[1], "info")) {
        rc = cmd_info(n - 2, args + 2);
    } else if (!strcmp(args[1], "dump")) {
        rc = cmd_dump(n - 2, args + 2);
    } else if (!strcmp(args[1], "patch")) {
        rc = cmd_patch(n - 2, args + 2);
    } else if (!strcmp(args[1], "ports")) {
        list_ports();
        rc = 0;
    } else if (!strcmp(args[1], "flash")) {
        rc = cmd_flash(n - 2, args + 2);
    } else if (!strcmp(args[1], "edit")) {
        if (n != 3) {
            usage();
            rc = 2;
        } else {
            rc = cmd_edit(args[2]);
        }
    } else {
        fprintf(stderr, "Unknown command: %s\n\n", args[1]);
        usage();
        rc = 2;
    }
    free(args);
    return rc;
}
