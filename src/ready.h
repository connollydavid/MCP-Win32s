/*
 * ready.h - Extended per-connection ready message
 *
 * Builds the JSON ready line the server sends as the first message on
 * every connection (spec: wire-contract.allium contract ReadyHandshake):
 * status, codepage (OEM, for decoding captured exec output - Q6),
 * version, transport, and the features capability object from
 * g_features. Call after FeatInit.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef READY_H
#define READY_H

/*
 * BuildReadyMessage - Build the newline-terminated ready JSON line.
 *
 * transportName - active backend name (e.g. "tcp", "serial")
 * warning       - optional warning string (e.g. "catalog not loaded");
 *                 NULL or "" omits the key
 * json/jsonSize - output buffer
 *
 * Returns: bytes written (excluding null terminator), 0 if buffer too
 * small. (Deviation from the PHASE4.md sketch signature: transport and
 * warning are parameters because only the dispatcher knows them; the
 * codepage is GetOEMCP(), not GetACP(), because the field exists to
 * decode OEM pipe output per Q6.)
 */
int BuildReadyMessage(const char *transportName, const char *warning,
                      char *json, int jsonSize);

#endif /* READY_H */
