/*
 * serial.h - Serial port backend for MCP-Win32s
 *
 * Serial-port configuration (CreateFileA/DCB/COMMTIMEOUTS) plus the
 * serial Transport backend. Transport selection and command-line
 * parsing now live in transport.h (they are transport-level).
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef SERIAL_H
#define SERIAL_H

#include <windows.h>
#include "transport.h"

/*
 * BuildSerialDCB - Populate a DCB for 8N1, no flow control, at baudRate.
 */
void BuildSerialDCB(DWORD baudRate, DCB *dcb);

/*
 * BuildSerialTimeouts - Populate a COMMTIMEOUTS for the read loop.
 */
void BuildSerialTimeouts(COMMTIMEOUTS *timeouts);

/*
 * OpenSerialPort - Open + configure a COM port. Returns a valid HANDLE
 * or INVALID_HANDLE_VALUE on failure.
 */
HANDLE OpenSerialPort(const char *portName, DWORD baudRate);

/*
 * CloseSerialPort - Close a serial port handle.
 */
void CloseSerialPort(HANDLE hPort);

/*
 * SerialBackendOpen - Open a serial Transport for cfg (port + baud).
 * Returns 1 on success, 0 with errMsg on failure.
 */
int SerialBackendOpen(const TransportConfig *cfg, Transport *out,
                      char *err, int errSize);

/*
 * SerialBackendRegister - Register the serial backend in the registry.
 */
void SerialBackendRegister(void);

#endif /* SERIAL_H */
