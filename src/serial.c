/*
 * serial.c - Serial port backend for MCP-Win32s
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <string.h>
#include "serial.h"
#include "uart.h"      /* the Win32s tier gate + direct-UART route */
#include "strutil.h"   /* McpStrCpyN (the NT 3.1 floor lacks lstrcpynA) */

#ifdef TEST_BUILD
/* The route SerialBackendOpen last selected (uart.h UartLastRouteForTest): the
 * dispatch-gate test reads this to pin the tier decision (SECURITY PIN #1)
 * WITHOUT driving a real port. */
static int g_serial_route = UART_ROUTE_NONE;
int UartLastRouteForTest(void)
{
    return g_serial_route;
}
#endif

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

    /*
     * Interval-only timeouts (total = 0): ReadFile BLOCKS until the first
     * byte arrives, then returns each burst after a 50ms lull. This is the
     * classic serial-terminal read pattern - a serial line has no orderly
     * "close" event, so an idle line must NOT end the session. With a total
     * timeout (the old config) ReadFile returned 0 bytes on idle, which the
     * transport layer reads as a peer close. With interval-only, idle simply
     * blocks; the session ends only on a real comms error (ReadFile fails).
     * See MSDN "Time-Outs": interval timing "does not begin until the first
     * character is received".
     */
    memset(timeouts, 0, sizeof(COMMTIMEOUTS));
    timeouts->ReadIntervalTimeout = 50;
    timeouts->ReadTotalTimeoutMultiplier = 0;
    timeouts->ReadTotalTimeoutConstant = 0;
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

    /*
     * A serial line has no orderly close, so this never reports 0 (which the
     * transport contract reserves for "peer closed"). Interval-only timeouts
     * make ReadFile block until a burst arrives, so the loop normally runs
     * once; the guard is defensive (a stray 0-byte success just waits again),
     * and it can never busy-spin because the blocking read holds inside
     * ReadFile. The session ends only when ReadFile fails (a comms error).
     */
    for (;;) {
        if (!ReadFile(t->io.handle, buf, (DWORD)len, &n, NULL)) {
            return -1;
        }
        if (n > 0) {
            return (int)n;
        }
    }
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

    /*
     * TIER GATE (uart.allium ServingViaUartImpliesWin32s, SECURITY PIN #1): on
     * the Win32s tier the Win32 comm API is an exported-but-stubbed no-op
     * (SetCommState -> err 120), so the 16550 is driven directly by bare ring-3
     * port I/O. This MUST be the FIRST decision and is the SOLE selector of the
     * direct route - it also covers TransportOpen's /AUTO fallback, which
     * re-enters here and re-checks the tier, so the dangerous branch is
     * structurally unreachable off Win32s.
     */
    if (UartTierWantsDirect()) {
#ifdef TEST_BUILD
        /* CI/NT cannot execute a ring-3 IN (it #GPs); the route DECISION is what
         * is pinned. Record it and stop short of any port I/O - the live open is
         * the on-target hardware acceptance. */
        g_serial_route = UART_ROUTE_DIRECT_UART;
        if (err != NULL && errSize > 0) {
            McpStrCpyN(err, "win32s direct-uart route (test: no port I/O)",
                      errSize);
        }
        return 0;
#else
        return UartBackendOpenDirect(cfg, out, err, errSize);
#endif
    }
#ifdef TEST_BUILD
    g_serial_route = UART_ROUTE_OS_SERIAL;
#endif

    h = OpenSerialPort(cfg->port, cfg->baudRate);
    if (h == INVALID_HANDLE_VALUE) {
        if (err != NULL && errSize > 0) {
            McpStrCpyN(err, "failed to open serial port", errSize);
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
