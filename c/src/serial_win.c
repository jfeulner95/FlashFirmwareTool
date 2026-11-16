/* Serial port implementation for Windows (Win32 API). */
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "serial.h"

#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#endif

struct serial_port {
    HANDLE h;
    int read_timeout; /* currently configured read timeout (ms), -1 = unknown */
};

static void win_error(char *err, size_t errlen, const char *what)
{
    DWORD code = GetLastError();
    char msg[256] = "";
    size_t n;
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, msg, sizeof msg,
                   NULL);
    n = strlen(msg);
    while (n && (msg[n - 1] == '\r' || msg[n - 1] == '\n' || msg[n - 1] == '.' || msg[n - 1] == ' '))
        msg[--n] = '\0';
    snprintf(err, errlen, "%s: %s (error %lu)", what, msg, (unsigned long)code);
}

static int set_read_timeout(serial_port *sp, int ms)
{
    COMMTIMEOUTS to;
    if (sp->read_timeout == ms)
        return 1;
    memset(&to, 0, sizeof to);
    if (ms <= 0) {
        /* return immediately with whatever is available */
        to.ReadIntervalTimeout = MAXDWORD;
    } else {
        /* return as soon as a byte arrives, or after 'ms' */
        to.ReadIntervalTimeout = MAXDWORD;
        to.ReadTotalTimeoutMultiplier = MAXDWORD;
        to.ReadTotalTimeoutConstant = (DWORD)ms;
    }
    to.WriteTotalTimeoutMultiplier = 2;
    to.WriteTotalTimeoutConstant = 5000;
    if (!SetCommTimeouts(sp->h, &to))
        return 0;
    sp->read_timeout = ms;
    return 1;
}

serial_port *serial_open(const char *name, long baud, ser_parity parity, int stopbits, char *err,
                         size_t errlen)
{
    char path[128];
    HANDLE h;
    DCB dcb;
    serial_port *sp;

    if (strncmp(name, "\\\\.\\", 4) == 0)
        snprintf(path, sizeof path, "%s", name);
    else
        snprintf(path, sizeof path, "\\\\.\\%s", name); /* required for COM10+ */

    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        win_error(err, errlen, "Cannot open port");
        return NULL;
    }

    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb)) {
        win_error(err, errlen, "GetCommState failed");
        CloseHandle(h);
        return NULL;
    }
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = 8;
    dcb.Parity = parity == SER_PARITY_EVEN ? EVENPARITY : parity == SER_PARITY_ODD ? ODDPARITY : NOPARITY;
    dcb.fParity = parity != SER_PARITY_NONE;
    dcb.StopBits = stopbits == 2 ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) {
        win_error(err, errlen, "Cannot configure port (unsupported baud rate?)");
        CloseHandle(h);
        return NULL;
    }
    SetupComm(h, 8192, 8192);

    sp = (serial_port *)calloc(1, sizeof *sp);
    if (!sp) {
        snprintf(err, errlen, "Out of memory");
        CloseHandle(h);
        return NULL;
    }
    sp->h = h;
    sp->read_timeout = -1;
    if (!set_read_timeout(sp, 1000)) {
        win_error(err, errlen, "SetCommTimeouts failed");
        serial_close(sp);
        return NULL;
    }
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return sp;
}

void serial_close(serial_port *sp)
{
    if (!sp)
        return;
    CloseHandle(sp->h);
    free(sp);
}

long serial_write(serial_port *sp, const uint8_t *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        DWORD n = 0;
        if (!WriteFile(sp->h, buf + total, (DWORD)(len - total), &n, NULL))
            return -1;
        if (n == 0)
            return -1; /* write timeout */
        total += n;
    }
    return (long)total;
}

long serial_read(serial_port *sp, uint8_t *buf, size_t len, int timeout_ms)
{
    DWORD n = 0;
    if (!set_read_timeout(sp, timeout_ms))
        return -1;
    if (!ReadFile(sp->h, buf, (DWORD)len, &n, NULL))
        return -1;
    return (long)n;
}

void serial_flush_input(serial_port *sp)
{
    PurgeComm(sp->h, PURGE_RXCLEAR);
}

void serial_drain(serial_port *sp)
{
    FlushFileBuffers(sp->h);
}

void serial_set_lines(serial_port *sp, int dtr, int rts)
{
    EscapeCommFunction(sp->h, dtr ? SETDTR : CLRDTR);
    EscapeCommFunction(sp->h, rts ? SETRTS : CLRRTS);
}

int serial_list_ports(serial_port_cb cb, void *user)
{
    HKEY key;
    DWORD i;
    int count = 0;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) !=
        ERROR_SUCCESS)
        return 0;

    for (i = 0;; i++) {
        char vname[256];
        BYTE data[256];
        DWORD vlen = sizeof vname, dlen = sizeof data - 1, type = 0;
        LONG r = RegEnumValueA(key, i, vname, &vlen, NULL, &type, data, &dlen);
        if (r == ERROR_NO_MORE_ITEMS)
            break;
        if (r != ERROR_SUCCESS || type != REG_SZ)
            continue;
        data[dlen] = '\0';
        cb((const char *)data, vname, user); /* e.g. "COM3", "\Device\USBSER000" */
        count++;
    }
    RegCloseKey(key);
    return count;
}

void plat_sleep_ms(unsigned ms)
{
    Sleep(ms);
}

uint64_t plat_now_ms(void)
{
    return (uint64_t)GetTickCount64();
}

#endif /* _WIN32 */
