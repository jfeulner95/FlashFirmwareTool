/* Serial port implementation for Linux / macOS (POSIX termios). */
#ifndef _WIN32

#define _DEFAULT_SOURCE      /* cfmakeraw, nanosleep, readlink, ... (glibc) */
#define _DARWIN_C_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "serial.h"

struct serial_port {
    int fd;
};

static const struct { long baud; speed_t speed; } baud_table[] = {
    {1200, B1200}, {2400, B2400}, {4800, B4800}, {9600, B9600}, {19200, B19200},
    {38400, B38400}, {57600, B57600}, {115200, B115200}, {230400, B230400},
#ifdef B460800
    {460800, B460800},
#endif
#ifdef B500000
    {500000, B500000},
#endif
#ifdef B576000
    {576000, B576000},
#endif
#ifdef B921600
    {921600, B921600},
#endif
#ifdef B1000000
    {1000000, B1000000},
#endif
#ifdef B1152000
    {1152000, B1152000},
#endif
#ifdef B1500000
    {1500000, B1500000},
#endif
#ifdef B2000000
    {2000000, B2000000},
#endif
#ifdef B2500000
    {2500000, B2500000},
#endif
#ifdef B3000000
    {3000000, B3000000},
#endif
#ifdef B3500000
    {3500000, B3500000},
#endif
#ifdef B4000000
    {4000000, B4000000},
#endif
};

serial_port *serial_open(const char *name, long baud, ser_parity parity, int stopbits,
                         char *err, size_t errlen)
{
    struct termios tio;
    speed_t speed = 0;
    int found = 0, fd, flags;
    size_t i;
    serial_port *sp;

    for (i = 0; i < sizeof baud_table / sizeof baud_table[0]; i++) {
        if (baud_table[i].baud == baud) {
            speed = baud_table[i].speed;
            found = 1;
            break;
        }
    }
    if (!found) {
        snprintf(err, errlen, "Unsupported baud rate: %ld", baud);
        return NULL;
    }

    fd = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        snprintf(err, errlen, "Cannot open %s: %s%s", name, strerror(errno),
                 errno == EACCES ? " (hint: add your user to the 'dialout' group)" : "");
        return NULL;
    }
    if (tcgetattr(fd, &tio) != 0) {
        snprintf(err, errlen, "%s is not a serial port: %s", name, strerror(errno));
        close(fd);
        return NULL;
    }

    cfmakeraw(&tio);
    tio.c_cflag &= ~(CSIZE | CSTOPB | PARENB | PARODD);
    tio.c_cflag |= CS8 | CLOCAL | CREAD;
#ifdef CRTSCTS
    tio.c_cflag &= ~CRTSCTS;
#endif
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    if (parity != SER_PARITY_NONE) {
        tio.c_cflag |= PARENB;
        if (parity == SER_PARITY_ODD)
            tio.c_cflag |= PARODD;
    }
    if (stopbits == 2)
        tio.c_cflag |= CSTOPB;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        snprintf(err, errlen, "Cannot configure %s: %s", name, strerror(errno));
        close(fd);
        return NULL;
    }

    /* switch back to blocking mode; reads use select() for timeouts */
    flags = fcntl(fd, F_GETFL);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    tcflush(fd, TCIOFLUSH);

    sp = (serial_port *)calloc(1, sizeof *sp);
    if (!sp) {
        snprintf(err, errlen, "Out of memory");
        close(fd);
        return NULL;
    }
    sp->fd = fd;
    return sp;
}

void serial_close(serial_port *sp)
{
    if (!sp)
        return;
    close(sp->fd);
    free(sp);
}

long serial_write(serial_port *sp, const uint8_t *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(sp->fd, buf + total, len - total);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return -1;
        }
        total += (size_t)n;
    }
    return (long)total;
}

long serial_read(serial_port *sp, uint8_t *buf, size_t len, int timeout_ms)
{
    fd_set rfds;
    struct timeval tv;
    ssize_t n;
    int r;

    FD_ZERO(&rfds);
    FD_SET(sp->fd, &rfds);
    tv.tv_sec = timeout_ms > 0 ? timeout_ms / 1000 : 0;
    tv.tv_usec = timeout_ms > 0 ? (timeout_ms % 1000) * 1000 : 0;

    r = select(sp->fd + 1, &rfds, NULL, NULL, &tv);
    if (r < 0)
        return errno == EINTR ? 0 : -1;
    if (r == 0)
        return 0;
    n = read(sp->fd, buf, len);
    if (n < 0)
        return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
    return (long)n;
}

void serial_flush_input(serial_port *sp)
{
    tcflush(sp->fd, TCIFLUSH);
}

void serial_drain(serial_port *sp)
{
    tcdrain(sp->fd);
}

void serial_set_lines(serial_port *sp, int dtr, int rts)
{
    int bits;
    bits = TIOCM_DTR;
    ioctl(sp->fd, dtr ? TIOCMBIS : TIOCMBIC, &bits);
    bits = TIOCM_RTS;
    ioctl(sp->fd, rts ? TIOCMBIS : TIOCMBIC, &bits);
}

/* ---------------------------------------------------------- port listing */
typedef struct { char name[128]; char desc[192]; } port_entry;

static int cmp_entry(const void *a, const void *b)
{
    return strcmp(((const port_entry *)a)->name, ((const port_entry *)b)->name);
}

#ifdef __linux__
static void read_first_line(const char *path, char *out, size_t outlen)
{
    FILE *f = fopen(path, "r");
    out[0] = '\0';
    if (!f)
        return;
    if (fgets(out, (int)outlen, f)) {
        size_t n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
            out[--n] = '\0';
    }
    fclose(f);
}
#endif

int serial_list_ports(serial_port_cb cb, void *user)
{
    port_entry entries[256];
    int count = 0, i;
    DIR *dir;
    struct dirent *de;

#ifdef __linux__
    dir = opendir("/sys/class/tty");
    if (!dir)
        return 0;
    while ((de = readdir(dir)) != NULL && count < 256) {
        char path[512], link[512], product[128];
        const char *driver;
        struct stat st;
        ssize_t n;

        if (de->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "/sys/class/tty/%s/device", de->d_name);
        if (stat(path, &st) != 0)
            continue;   /* virtual terminal, no hardware behind it */

        snprintf(path, sizeof path, "/sys/class/tty/%s/device/driver", de->d_name);
        n = readlink(path, link, sizeof link - 1);
        if (n <= 0)
            continue;
        link[n] = '\0';
        driver = strrchr(link, '/') ? strrchr(link, '/') + 1 : link;
        if (strcmp(driver, "serial8250") == 0)
            continue;   /* unused legacy ttyS placeholder */

        snprintf(path, sizeof path, "/sys/class/tty/%s/device/../product", de->d_name);
        read_first_line(path, product, sizeof product);

        snprintf(entries[count].name, sizeof entries[count].name, "/dev/%s", de->d_name);
        if (product[0])
            snprintf(entries[count].desc, sizeof entries[count].desc, "%s (%s)", product, driver);
        else
            snprintf(entries[count].desc, sizeof entries[count].desc, "%s", driver);
        count++;
    }
    closedir(dir);
#else
    /* macOS / BSD: use the call-out devices */
    dir = opendir("/dev");
    if (!dir)
        return 0;
    while ((de = readdir(dir)) != NULL && count < 256) {
        if (strncmp(de->d_name, "cu.", 3) != 0)
            continue;
        snprintf(entries[count].name, sizeof entries[count].name, "/dev/%s", de->d_name);
        snprintf(entries[count].desc, sizeof entries[count].desc, "%s", de->d_name + 3);
        count++;
    }
    closedir(dir);
#endif

    qsort(entries, (size_t)count, sizeof entries[0], cmp_entry);
    for (i = 0; i < count; i++)
        cb(entries[i].name, entries[i].desc, user);
    return count;
}

void plat_sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

uint64_t plat_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

#endif /* !_WIN32 */
