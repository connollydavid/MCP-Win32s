/*
 * audit.h - Durable, fail-closed audit log for memory writes
 *
 * Spec: memory-ops.allium (entity AuditRecord; invariant
 * PokeIsAuditedFailClosed; the device half of PokeRequiresBothArmingLayers).
 *
 * Every memory WRITE (poke) must be recorded here BEFORE it is reported,
 * and a write that cannot be recorded must NOT happen (fail closed - never
 * an unlogged mutation). Two independent gates live here:
 *
 *   1. The device WRITE ARM (/ALLOWMEMWRITE). The device honours a poke
 *      only when armed, regardless of which client sent it - so a direct
 *      wire client a bridge-only gate could never reach is still bound.
 *      This is the device half of PokeRequiresBothArmingLayers (the bridge
 *      --allow-memory-write advertisement gate is the other, independent
 *      half).
 *
 *   2. The audit SINK. A per-record append (CreateFileA + seek-end +
 *      WriteFile + close): Win32s-safe, crash-durable, integer-only, ANSI.
 *      At startup, if the arm is requested but the sink is not writable,
 *      the arm is DROPPED (a poke that could not be logged must never be
 *      armable) - so a mid-session audit failure is rare by construction.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef AUDIT_H
#define AUDIT_H

/*
 * AuditConfigure - Install the write arm and the audit sink at startup.
 *
 *   armRequested  nonzero iff the operator passed /ALLOWMEMWRITE.
 *   path          the audit log path (/AUDIT:path), or NULL/"" to use the
 *                 default (audit-mem.log next to the executable).
 *
 * When armRequested is set, the sink's writability is verified; if it is
 * NOT writable the arm is DROPPED (fail-closed startup check). Returns the
 * EFFECTIVE armed state (1 = armed and the sink is writable; 0 = disarmed).
 * Call once from main after FeatInit; tests call it directly.
 */
int AuditConfigure(int armRequested, const char *path);

/*
 * AuditIsArmed - Whether memory writes are armed (the device
 * /ALLOWMEMWRITE wire arm). The MemoryPoked rule's `memory_write_armed`
 * precondition; the device half of PokeRequiresBothArmingLayers. A poke
 * with this clear is refused at the wire, binding every client.
 */
int AuditIsArmed(void);

/*
 * AuditIsWritable - Probe whether the configured sink can currently be
 * opened for append. The pre-write fail-closed check (a poke whose record
 * cannot be written must not mutate memory). Returns 1 if writable.
 */
int AuditIsWritable(void);

/*
 * AuditWritePoke - Append exactly one poke record to the sink, durably.
 *
 * One integer-only, ANSI, append-only line (spec: entity AuditRecord):
 *   <GetTickCount> POKE tier=<t> token=<tok> pid=<n> addr=<hex> len=<n>
 *   written=<n> partial=<0|1> cmd=<catalogued cmdline>
 *
 * Per-record CreateFileA(OPEN_ALWAYS) + SetFilePointer(END) + WriteFile +
 * CloseHandle - no retained handle, crash-durable, Win32s-safe. Returns 1
 * on a durable write; 0 on failure, in which case the CALLER MUST refuse
 * the poke and perform no write (PokeIsAuditedFailClosed - no unlogged
 * mutation). addr/len/written are unsigned 32-bit values; partial is 0/1.
 */
int AuditWritePoke(const char *tier, const char *token, int pid,
                   const char *command,
                   unsigned long addr, unsigned long len,
                   unsigned long bytes_written, int partial);

#endif /* AUDIT_H */
