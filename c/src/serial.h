/* Minimal cross-platform serial port abstraction (Windows / POSIX). */
#ifndef FW_SERIAL_H
#define FW_SERIAL_H

#include <stddef.h>
#include <stdint.h>

typedef struct serial_port serial_port;

typedef enum { SER_PARITY_NONE, SER_PARITY_EVEN, SER_PARITY_ODD } ser_parity;

/* Opens the port with 8 data bits, no flow control. Returns NULL on error (message in err). */
serial_port *serial_open(const char *name, long baud, ser_parity parity, int stopbits, char *err,
                         size_t errlen);
void serial_close(serial_port *sp);

/* Writes all bytes. Returns number of bytes written or -1 on error/timeout. */
long serial_write(serial_port *sp, const uint8_t *buf, size_t len);

/* Reads up to len bytes. Returns as soon as at least one byte is available.
 * Returns number of bytes read, 0 on timeout, -1 on error. */
long serial_read(serial_port *sp, uint8_t *buf, size_t len, int timeout_ms);

void serial_flush_input(serial_port *sp); /* discard received, unread data */
void serial_drain(serial_port *sp);       /* wait until all output is transmitted */
void serial_set_lines(serial_port *sp, int dtr, int rts);

typedef void (*serial_port_cb)(const char *name, const char *desc, void *user);
/* Enumerates available ports; returns number of ports found. */
int serial_list_ports(serial_port_cb cb, void *user);

void plat_sleep_ms(unsigned ms);
uint64_t plat_now_ms(void);

#endif
