/* Firmware transfer protocols: RAW, XMODEM (128) and XMODEM-1K. */
#ifndef FW_FLASH_H
#define FW_FLASH_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#include "serial.h"

typedef enum { PROTO_RAW, PROTO_XMODEM, PROTO_XMODEM1K } flash_proto;

typedef struct {
    const char *port;
    long baud;
    ser_parity parity;
    int stopbits;
    flash_proto proto;
    size_t chunk_size;       /* RAW: bytes per write */
    unsigned chunk_delay_ms; /* RAW: pause after each chunk */
    unsigned start_delay_ms; /* pause before the transfer starts */
    int reset;               /* pulse DTR/RTS before start */
    unsigned xmodem_wait_s;  /* XMODEM: how long to wait for the receiver */
} flash_opts;

/* Set by the SIGINT handler (Ctrl+C) to abort a running transfer. */
extern volatile sig_atomic_t g_cancel;

void flash_opts_default(flash_opts *o);

/* Parses one option at argv[*i] (and its value).
 * Returns 1 if consumed (*i points to the last consumed arg), 0 if not a flash option, -1 on error. */
int flash_parse_option(flash_opts *o, int argc, char **argv, int *i, char *err, size_t errlen);

/* Transfers the data. Returns 0 on success, -1 on error, -2 if cancelled. */
int flash_run(const flash_opts *o, const uint8_t *data, size_t len);

const char *flash_proto_name(flash_proto p);
void flash_print_option_help(void);
uint16_t crc16_xmodem(const uint8_t *data, size_t len);

#endif
