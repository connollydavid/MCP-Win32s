/*
 * serial.c - Serial port configuration and transport for MCP-Win32s
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <string.h>
#include "serial.h"

/*
 * find_flag - Find a /FLAG: style argument in a command-line string.
 *
 * Returns pointer to the character after the colon, or NULL if not found.
 * Case-insensitive comparison for the flag name.
 */
static const char *find_flag(const char *cmdLine, const char *flag)
{
    const char *p;
    int flagLen;

    if (cmdLine == NULL || flag == NULL) {
        return NULL;
    }

    flagLen = 0;
    while (flag[flagLen] != '\0') {
        flagLen++;
    }

    p = cmdLine;
    while (*p != '\0') {
        /* Look for '/' */
        if (*p == '/') {
            const char *start;
            int match;
            int i;

            start = p + 1;
            match = 1;
            for (i = 0; i < flagLen && start[i] != '\0'; i++) {
                char a, b;
                a = start[i];
                b = flag[i];
                /* Simple uppercase conversion for A-Z */
                if (a >= 'a' && a <= 'z') a = a - ('a' - 'A');
                if (b >= 'a' && b <= 'z') b = b - ('a' - 'A');
                if (a != b) {
                    match = 0;
                    break;
                }
            }
            if (match && i == flagLen && start[i] == ':') {
                return &start[i + 1];
            }
        }
        p++;
    }

    return NULL;
}

/*
 * copy_until_space - Copy characters from src to dst until whitespace or end.
 * Null-terminates dst. Returns number of characters copied.
 */
static int copy_until_space(const char *src, char *dst, int dst_size)
{
    int i;

    i = 0;
    while (src[i] != '\0' && src[i] != ' ' && src[i] != '\t' &&
           i < dst_size - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return i;
}

/*
 * simple_atoi - Convert decimal string to integer. No error checking.
 * Stops at first non-digit. Returns 0 for empty/invalid input.
 */
static int simple_atoi(const char *s)
{
    int result;

    result = 0;
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return result;
}

int ParseCommandLine(const char *cmdLine, TransportConfig *config)
{
    const char *val;

    if (config == NULL) {
        return 0;
    }

    memset(config, 0, sizeof(TransportConfig));

    /* Default to serial on COM1 */
    config->transport = TRANSPORT_SERIAL;
    copy_until_space(DEFAULT_PORT, config->port, sizeof(config->port));
    config->baudRate = DEFAULT_BAUD_RATE;

    if (cmdLine == NULL || cmdLine[0] == '\0') {
        return 1;
    }

    /* Check for /SERIAL:COMx */
    val = find_flag(cmdLine, "SERIAL");
    if (val != NULL) {
        config->transport = TRANSPORT_SERIAL;
        copy_until_space(val, config->port, sizeof(config->port));
        config->baudRate = DEFAULT_BAUD_RATE;
        return 1;
    }

    /* Check for /TCP:port */
    val = find_flag(cmdLine, "TCP");
    if (val != NULL) {
        char portStr[16];
        config->transport = TRANSPORT_TCP;
        copy_until_space(val, portStr, sizeof(portStr));
        config->tcpPort = simple_atoi(portStr);
        if (config->tcpPort <= 0 || config->tcpPort > 65535) {
            return 0;
        }
        return 1;
    }

    /* Check for /PIPE:path */
    val = find_flag(cmdLine, "PIPE");
    if (val != NULL) {
        config->transport = TRANSPORT_PIPE;
        copy_until_space(val, config->pipeName, sizeof(config->pipeName));
        return 1;
    }

    /* No recognized flag - keep defaults (serial COM1) */
    return 1;
}

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

    /* Configure baud rate, 8N1, no flow control */
    BuildSerialDCB(baudRate, &dcb);
    if (!SetCommState(hPort, &dcb)) {
        CloseHandle(hPort);
        return INVALID_HANDLE_VALUE;
    }

    /* Configure read timeouts */
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
