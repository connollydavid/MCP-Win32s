/*
 * serial.c - Serial port backend for MCP-Win32s
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <string.h>
#include "serial.h"

void BuildSerialDCB(DWORD baudRate, DCB *dcb)
{
    if (dcb == NULL) {
        return;
    }

    memset(dcb, 0, sizeof(DCB));
    dcb->DCBlength = sizeof(DCB);
    dcb->BaudRate = baudRate;
    dcb->ByteSize = 8;
    dcb->Parity = NOPARITY;
    dcb->StopBits = ONESTOPBIT;
    /* No flow control */
    dcb->fOutxCtsFlow = FALSE;
    dcb->fOutxDsrFlow = FALSE;
    dcb->fDtrControl = DTR_CONTROL_ENABLE;
    dcb->fRtsControl = RTS_CONTROL_ENABLE;
    dcb->fOutX = FALSE;
    dcb->fInX = FALSE;
    dcb->fBinary = TRUE;
}

void BuildSerialTimeouts(COMMTIMEOUTS *timeouts)
{
    if (timeouts == NULL) {
        return;
    }

    memset(timeouts, 0, sizeof(COMMTIMEOUTS));
    timeouts->ReadIntervalTimeout = 50;
    timeouts->ReadTotalTimeoutMultiplier = 10;
    timeouts->ReadTotalTimeoutConstant = 50;
    /* Write timeouts left at 0 (no timeout) */
}

HANDLE OpenSerialPort(const char *portName, DWORD baudRate)
{
    HANDLE hPort;
    DCB dcb;
    COMMTIMEOUTS timeouts;

    hPort = CreateFileA(portName,
                        GENERIC_READ | GENERIC_WRITE,
                        0, NULL, OPEN_EXISTING, 0, NULL);

    if (hPort == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }

    BuildSerialDCB(baudRate, &dcb);
    if (!SetCommState(hPort, &dcb)) {
        CloseHandle(hPort);
        return INVALID_HANDLE_VALUE;
    }

    BuildSerialTimeouts(&timeouts);
    if (!SetCommTimeouts(hPort, &timeouts)) {
        CloseHandle(hPort);
        return INVALID_HANDLE_VALUE;
    }

    return hPort;
}

void CloseSerialPort(HANDLE hPort)
{
    if (hPort != INVALID_HANDLE_VALUE) {
        CloseHandle(hPort);
    }
}

/* ========================================================
 * Serial Transport backend (vtable over the COM handle)
 * ======================================================== */

static int serial_read(Transport *t, void *buf, int len)
{
    DWORD n;
    if (!ReadFile(t->io.handle, buf, (DWORD)len, &n, NULL)) {
        return -1;
    }
    return (int)n;   /* 0 = no data within timeout / closed */
}

static int serial_write(Transport *t, const void *buf, int len)
{
    DWORD n;
    if (!WriteFile(t->io.handle, buf, (DWORD)len, &n, NULL)) {
        return -1;
    }
    return (int)n;
}

static void serial_close(Transport *t)
{
    if (t->io.handle != INVALID_HANDLE_VALUE) {
        CloseHandle(t->io.handle);
        t->io.handle = INVALID_HANDLE_VALUE;
    }
}

int SerialBackendOpen(const TransportConfig *cfg, Transport *out,
                      char *err, int errSize)
{
    HANDLE h;

    h = OpenSerialPort(cfg->port, cfg->baudRate);
    if (h == INVALID_HANDLE_VALUE) {
        if (err != NULL && errSize > 0) {
            lstrcpynA(err, "failed to open serial port", errSize);
        }
        return 0;
    }

    out->name = "serial";
    out->kind = TRANSPORT_SERIAL;
    out->flags = 0;
    out->read = serial_read;
    out->write = serial_write;
    out->close = serial_close;
    out->accept = NULL;          /* point-to-point: no accept */
    out->io.handle = h;
    return 1;
}

void SerialBackendRegister(void)
{
    TransportBackend b;
    b.kind = TRANSPORT_SERIAL;
    b.name = "serial";
    b.probe = NULL;              /* always available */
    b.open = SerialBackendOpen;
    TransportRegister(&b);
}
