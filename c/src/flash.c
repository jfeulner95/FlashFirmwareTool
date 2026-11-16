#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flash.h"

#define SOH     0x01
#define STX     0x02
#define EOT     0x04
#define ACK     0x06
#define NAK     0x15
#define CAN     0x18
#define CRC_REQ 'C'

volatile sig_atomic_t g_cancel = 0;

void flash_opts_default(flash_opts *o)
{
    memset(o, 0, sizeof *o);
    o->baud = 115200;
    o->parity = SER_PARITY_NONE;
    o->stopbits = 1;
    o->proto = PROTO_RAW;
    o->chunk_size = 256;
    o->xmodem_wait_s = 60;
}

const char *flash_proto_name(flash_proto p)
{
    switch (p) {
    case PROTO_XMODEM: return "XMODEM";
    case PROTO_XMODEM1K: return "XMODEM-1K";
    default: return "RAW";
    }
}

void flash_print_option_help(void)
{
    printf("  -b, --baud N          baud rate (default 115200)\n"
           "  -p, --protocol P      raw | xmodem | xmodem1k (default raw)\n"
           "      --parity X        n | e | o (default n)\n"
           "      --stop N          stop bits 1 | 2 (default 1)\n"
           "  -c, --chunk N         RAW: bytes per block (default 256)\n"
           "  -d, --delay MS        RAW: pause after each block in ms (default 0)\n"
           "      --start-delay MS  wait before starting the transfer\n"
           "      --reset           pulse DTR/RTS before starting (reset target)\n"
           "      --wait S          XMODEM: seconds to wait for the receiver (default 60)\n");
}

uint16_t crc16_xmodem(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;
    size_t i;
    int b;
    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* ------------------------------------------------------------ options */
static int parse_long(const char *s, long min, long max, long *out)
{
    char *end;
    long v;
    if (!s || !*s)
        return 0;
    v = strtol(s, &end, 10);
    if (*end || v < min || v > max)
        return 0;
    *out = v;
    return 1;
}

static int lower_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        if (ca != *b)
            return 0;
    }
    return *a == *b;
}

int flash_parse_option(flash_opts *o, int argc, char **argv, int *i, char *err, size_t errlen)
{
    const char *opt = argv[*i];
    const char *val = (*i + 1 < argc) ? argv[*i + 1] : NULL;
    long v;

#define NEED_VALUE()                                                  \
    do {                                                              \
        if (!val) {                                                   \
            snprintf(err, errlen, "Option %s requires a value", opt); \
            return -1;                                                \
        }                                                             \
        (*i)++;                                                       \
    } while (0)
#define BAD_VALUE()                                                  \
    do {                                                             \
        snprintf(err, errlen, "Invalid value for %s: %s", opt, val); \
        return -1;                                                   \
    } while (0)

    if (!strcmp(opt, "-b") || !strcmp(opt, "--baud")) {
        NEED_VALUE();
        if (!parse_long(val, 50, 20000000, &v))
            BAD_VALUE();
        o->baud = v;
    } else if (!strcmp(opt, "-p") || !strcmp(opt, "--protocol")) {
        NEED_VALUE();
        if (lower_eq(val, "raw"))
            o->proto = PROTO_RAW;
        else if (lower_eq(val, "xmodem"))
            o->proto = PROTO_XMODEM;
        else if (lower_eq(val, "xmodem1k") || lower_eq(val, "xmodem-1k") || lower_eq(val, "1k"))
            o->proto = PROTO_XMODEM1K;
        else
            BAD_VALUE();
    } else if (!strcmp(opt, "--parity")) {
        NEED_VALUE();
        if (lower_eq(val, "n") || lower_eq(val, "none"))
            o->parity = SER_PARITY_NONE;
        else if (lower_eq(val, "e") || lower_eq(val, "even"))
            o->parity = SER_PARITY_EVEN;
        else if (lower_eq(val, "o") || lower_eq(val, "odd"))
            o->parity = SER_PARITY_ODD;
        else
            BAD_VALUE();
    } else if (!strcmp(opt, "--stop")) {
        NEED_VALUE();
        if (!parse_long(val, 1, 2, &v))
            BAD_VALUE();
        o->stopbits = (int)v;
    } else if (!strcmp(opt, "-c") || !strcmp(opt, "--chunk")) {
        NEED_VALUE();
        if (!parse_long(val, 1, 1 << 20, &v))
            BAD_VALUE();
        o->chunk_size = (size_t)v;
    } else if (!strcmp(opt, "-d") || !strcmp(opt, "--delay")) {
        NEED_VALUE();
        if (!parse_long(val, 0, 600000, &v))
            BAD_VALUE();
        o->chunk_delay_ms = (unsigned)v;
    } else if (!strcmp(opt, "--start-delay")) {
        NEED_VALUE();
        if (!parse_long(val, 0, 600000, &v))
            BAD_VALUE();
        o->start_delay_ms = (unsigned)v;
    } else if (!strcmp(opt, "--wait")) {
        NEED_VALUE();
        if (!parse_long(val, 1, 3600, &v))
            BAD_VALUE();
        o->xmodem_wait_s = (unsigned)v;
    } else if (!strcmp(opt, "--reset")) {
        o->reset = 1;
    } else {
        return 0;
    }
    return 1;
#undef NEED_VALUE
#undef BAD_VALUE
}

/* ----------------------------------------------------------- progress */
static int g_last_permille = -1;

static void progress(size_t done, size_t total)
{
    const int width = 40;
    int permille = total ? (int)((double)done * 1000.0 / (double)total) : 1000;
    int filled = permille * width / 1000;
    if (permille == g_last_permille)
        return;
    g_last_permille = permille;
    fprintf(stderr, "\r[%.*s%*s] %5.1f%%  %zu/%zu bytes", filled, "########################################",
            width - filled, "", permille / 10.0, done, total);
    if (done >= total)
        fputc('\n', stderr);
    fflush(stderr);
}

/* Sleeps in small slices so that Ctrl+C stays responsive. Returns 0 if cancelled. */
static int sleep_cancellable(unsigned ms)
{
    uint64_t end = plat_now_ms() + ms;
    while (plat_now_ms() < end) {
        uint64_t left = end - plat_now_ms();
        if (g_cancel)
            return 0;
        plat_sleep_ms(left > 50 ? 50 : (unsigned)left);
    }
    return !g_cancel;
}

/* ---------------------------------------------------------------- RAW */
static int send_raw(serial_port *sp, const flash_opts *o, const uint8_t *data, size_t len)
{
    size_t off;
    for (off = 0; off < len; off += o->chunk_size) {
        size_t n = len - off < o->chunk_size ? len - off : o->chunk_size;
        if (g_cancel)
            return -2;
        if (serial_write(sp, data + off, n) != (long)n) {
            fprintf(stderr, "\nWrite error at offset 0x%08zX\n", off);
            return -1;
        }
        serial_drain(sp);
        progress(off + n, len);
        if (o->chunk_delay_ms && !sleep_cancellable(o->chunk_delay_ms))
            return -2;
    }
    if (len == 0)
        progress(0, 0);
    return 0;
}

/* ------------------------------------------------------------- XMODEM */
/* Returns the byte, -1 on timeout, -2 on error. */
static int read_byte(serial_port *sp, int timeout_ms)
{
    uint8_t b;
    long n = serial_read(sp, &b, 1, timeout_ms);
    if (n == 1)
        return b;
    return n < 0 ? -2 : -1;
}

/* Waits for a response byte in 100 ms slices (so Ctrl+C works). */
static int wait_response(serial_port *sp, unsigned timeout_ms)
{
    uint64_t end = plat_now_ms() + timeout_ms;
    while (!g_cancel) {
        int c = read_byte(sp, 100);
        if (c != -1)
            return c;
        if (plat_now_ms() >= end)
            return -1;
    }
    return -1;
}

static void send_cancel(serial_port *sp)
{
    static const uint8_t cancel[] = {CAN, CAN, CAN};
    serial_write(sp, cancel, sizeof cancel);
    serial_drain(sp);
}

static int send_xmodem(serial_port *sp, const flash_opts *o, const uint8_t *data, size_t len)
{
    uint8_t pkt[3 + 1024 + 2];
    size_t block_size = o->proto == PROTO_XMODEM1K ? 1024 : 128;
    size_t off = 0;
    unsigned blk = 1;
    int use_crc = -1, attempt;
    uint64_t deadline = plat_now_ms() + (uint64_t)o->xmodem_wait_s * 1000u;

    printf("Waiting for receiver ('C' or NAK), max. %u s ... (Ctrl+C to cancel)\n", o->xmodem_wait_s);
    fflush(stdout);
    while (use_crc < 0) {
        int c;
        if (g_cancel) {
            send_cancel(sp);
            return -2;
        }
        if (plat_now_ms() > deadline) {
            fprintf(stderr, "Timeout: receiver did not respond\n");
            return -1;
        }
        c = read_byte(sp, 200);
        if (c == CRC_REQ)
            use_crc = 1;
        else if (c == NAK)
            use_crc = 0;
        else if (c == CAN) {
            fprintf(stderr, "Receiver cancelled the transfer (CAN)\n");
            return -1;
        } else if (c == -2) {
            fprintf(stderr, "Read error\n");
            return -1;
        }
    }
    serial_flush_input(sp); /* drop additional 'C's the receiver may have queued */

    if (!use_crc && block_size == 1024) {
        printf("Receiver does not support CRC -> falling back to 128-byte blocks\n");
        block_size = 128;
    }
    printf("Receiver ready: %s, %zu-byte blocks\n", use_crc ? "CRC16" : "checksum", block_size);

    while (off < len) {
        size_t n = len - off < block_size ? len - off : block_size;
        size_t pkt_len = 3 + block_size;
        pkt[0] = block_size == 1024 ? STX : SOH;
        pkt[1] = (uint8_t)(blk & 0xFF);
        pkt[2] = (uint8_t)(0xFF - (blk & 0xFF));
        memcpy(pkt + 3, data + off, n);
        if (n < block_size)
            memset(pkt + 3 + n, 0x1A, block_size - n); /* pad with SUB */
        if (use_crc) {
            uint16_t crc = crc16_xmodem(pkt + 3, block_size);
            pkt[pkt_len++] = (uint8_t)(crc >> 8);
            pkt[pkt_len++] = (uint8_t)(crc & 0xFF);
        } else {
            uint8_t sum = 0;
            size_t k;
            for (k = 0; k < block_size; k++)
                sum = (uint8_t)(sum + pkt[3 + k]);
            pkt[pkt_len++] = sum;
        }

        for (attempt = 1; attempt <= 10; attempt++) {
            int r;
            if (g_cancel) {
                send_cancel(sp);
                return -2;
            }
            if (serial_write(sp, pkt, pkt_len) != (long)pkt_len) {
                fprintf(stderr, "\nWrite error\n");
                return -1;
            }
            r = wait_response(sp, 10000);
            if (r == ACK)
                break;
            if (r == CAN) {
                fprintf(stderr, "\nReceiver cancelled the transfer (CAN)\n");
                return -1;
            }
            if (r == -2) {
                fprintf(stderr, "\nRead error\n");
                return -1;
            }
            if (g_cancel)
                continue; /* handled at top of loop */
            fprintf(stderr, "\nBlock %u: %s - retry %d/10\n", blk,
                    r == NAK  ? "NAK"
                    : r == -1 ? "timeout"
                              : "unexpected response",
                    attempt);
        }
        if (attempt > 10) {
            fprintf(stderr, "Block %u: too many errors, aborting\n", blk);
            send_cancel(sp);
            return -1;
        }
        off += n;
        blk++;
        progress(off, len);
    }

    for (attempt = 1; attempt <= 10; attempt++) {
        static const uint8_t eot = EOT;
        int r;
        if (g_cancel)
            return -2;
        serial_write(sp, &eot, 1);
        r = wait_response(sp, 5000);
        if (r == ACK)
            return 0;
    }
    fprintf(stderr, "EOT was not acknowledged\n");
    return -1;
}

/* --------------------------------------------------------------- run */
int flash_run(const flash_opts *o, const uint8_t *data, size_t len)
{
    char err[512];
    serial_port *sp;
    uint64_t t0, dt;
    int rc;

    sp = serial_open(o->port, o->baud, o->parity, o->stopbits, err, sizeof err);
    if (!sp) {
        fprintf(stderr, "%s\n", err);
        return -1;
    }
    printf("Opened %s @ %ld baud, 8%c%d, protocol %s\n", o->port, o->baud,
           o->parity == SER_PARITY_EVEN  ? 'E'
           : o->parity == SER_PARITY_ODD ? 'O'
                                         : 'N',
           o->stopbits, flash_proto_name(o->proto));

    rc = 0;
    if (o->reset) {
        printf("Reset pulse via DTR/RTS\n");
        serial_set_lines(sp, 0, 0);
        plat_sleep_ms(100);
        serial_set_lines(sp, 1, 1);
        plat_sleep_ms(100);
    }
    if (o->start_delay_ms) {
        printf("Waiting %u ms before start\n", o->start_delay_ms);
        if (!sleep_cancellable(o->start_delay_ms))
            rc = -2;
    }
    fflush(stdout);

    g_last_permille = -1;
    t0 = plat_now_ms();
    if (rc == 0) {
        serial_flush_input(sp);
        rc = o->proto == PROTO_RAW ? send_raw(sp, o, data, len) : send_xmodem(sp, o, data, len);
    }
    dt = plat_now_ms() - t0;
    serial_close(sp);

    if (rc == 0) {
        double secs = dt / 1000.0;
        printf("Done: %zu bytes in %.2f s (%.1f KiB/s)\n", len, secs, secs > 0 ? len / secs / 1024.0 : 0.0);
    } else if (rc == -2) {
        fprintf(stderr, "\nTransfer cancelled.\n");
    } else {
        fprintf(stderr, "Transfer failed.\n");
    }
    return rc;
}
