/*
 * serial.h - Serial port configuration and transport for MCP-Win32s
 *
 * Provides serial port initialization using CreateFileA/DCB/COMMTIMEOUTS.
 * Pure config-builder functions are separated from Win32 API calls
 * to allow unit testing without hardware.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef SERIAL_H
#define SERIAL_H

#include <windows.h>

/* Transport types for command-line parsing */
#define TRANSPORT_NONE   0
#define TRANSPORT_SERIAL 1
#define TRANSPORT_TCP    2
#define TRANSPORT_PIPE   3

/* Default serial settings */
#define DEFAULT_BAUD_RATE CBR_115200
#define DEFAULT_PORT      "COM1"

/* Serial port configuration (parsed from command line) */
typedef struct {
    int transport;               /* TRANSPORT_SERIAL, TRANSPORT_TCP, etc. */
    char port[32];               /* "COM1", "COM2", etc. */
    DWORD baudRate;              /* CBR_115200, CBR_57600, etc. */
    int tcpPort;                 /* TCP port number (for TRANSPORT_TCP) */
    char pipeName[260];          /* Named pipe path (for TRANSPORT_PIPE) */
} TransportConfig;

/*
 * ParseCommandLine - Parse command-line string into TransportConfig.
 *
 * Recognized formats:
 *   /SERIAL:COM1       Force serial on COM1
 *   /SERIAL:COM2       Force serial on COM2
 *   /TCP:8932          Force TCP on port 8932
 *   /PIPE:\\.\pipe\mcp Force named pipe
 *   (empty/default)    Auto-detect (defaults to serial COM1)
 *
 * Parameters:
 *   cmdLine - raw command-line string (from GetCommandLineA or argv)
 *   config  - output TransportConfig struct (zeroed first, then filled)
 *
 * Returns: 1 on success, 0 on parse error.
 */
int ParseCommandLine(const char *cmdLine, TransportConfig *config);

/*
 * BuildSerialDCB - Populate a DCB struct for serial communication.
 *
 * Sets: baud rate, 8N1, no flow control.
 * The DCB is suitable for passing to SetCommState().
 *
 * Parameters:
 *   baudRate - desired baud rate (e.g. CBR_115200)
 *   dcb      - output DCB struct
 */
void BuildSerialDCB(DWORD baudRate, DCB *dcb);

/*
 * BuildSerialTimeouts - Populate a COMMTIMEOUTS struct.
 *
 * Sets read timeouts suitable for character-by-character reading
 * in the main protocol loop.
 *
 * Parameters:
 *   timeouts - output COMMTIMEOUTS struct
 */
void BuildSerialTimeouts(COMMTIMEOUTS *timeouts);

/*
 * OpenSerialPort - Open and configure a serial port.
 *
 * Calls CreateFileA, SetCommState, SetCommTimeouts.
 *
 * Parameters:
 *   portName - e.g. "COM1"
 *   baudRate - e.g. CBR_115200
 *
 * Returns: valid HANDLE on success, INVALID_HANDLE_VALUE on failure.
 */
HANDLE OpenSerialPort(const char *portName, DWORD baudRate);

/*
 * CloseSerialPort - Close a serial port handle.
 *
 * Parameters:
 *   hPort - handle from OpenSerialPort
 */
void CloseSerialPort(HANDLE hPort);

#endif /* SERIAL_H */
