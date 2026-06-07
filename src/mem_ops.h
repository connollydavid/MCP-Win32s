/*
 * mem_ops.h - Tiered, user-mode memory peek/poke (Phase 5, work-item 5.3)
 *
 * Spec: memory-ops.allium. The device's highest-stakes capability: bounded,
 * consented, logged ring-3 memory read/write. We use exactly what each OS
 * family grants ring-3 - never escalating (no ring-0, VxD, call gate). The
 * reach TIER follows the OS family (spec: enum MemTier):
 *
 *   process    (NT/2000/XP/Vista+/7-11): ReadProcessMemory/WriteProcessMemory
 *              on a CHILD WE LAUNCHED AND RETAIN, named by an opaque token.
 *              No OpenProcess-by-PID, ever - the spawn-retain table is the
 *              SOLE process-tier target boundary.
 *   arena      (Win9x): direct guarded loads/stores into the reachable arenas.
 *   shared_vm  (Win32s): direct guarded loads/stores into the single shared VM.
 *
 * On the two pre-NT tiers there is no per-process token: the address is into
 * the shared address space directly (token == NULL / "").
 *
 * Hard constraints (bind THIS binary's own source, per CLAUDE.md): C89 only,
 * declarations at block top, slash-star comments only, no floating point,
 * i386, ANSI APIs, no threads.
 * ReadProcessMemory/WriteProcessMemory/VirtualQueryEx are
 * NT-only cross-process APIs: they MUST be resolved via GetProcAddress (never
 * statically imported) so the binary stays loadable on bare Win32s. Local
 * VirtualQuery/IsBadReadPtr/IsBadWritePtr (the pre-NT guard) are Win32s-safe.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef MEM_OPS_H
#define MEM_OPS_H

#include <windows.h>
#include "catalog.h"

/* The bounded process table: 8 slots, no heap (spec note: an implementation
 * detail, not a pinned case - the test pins exhaustion behaviour and
 * release-on-disconnect, never the number). */
#define MEM_PROC_SLOTS   8

/* The maximum bytes one peek/poke may touch (spec: config.max_access_length).
 * The range guard rejects any request whose length exceeds this. */
#define MEM_MAX_ACCESS   65536UL

/* Token buffer size: "m" + monotonic seq + "-" + LCG suffix, comfortably
 * within 32 bytes; padded for safety. */
#define MEM_TOKEN_LEN    40

/* The device's memory-reach tier (spec: enum MemTier). The wire/JSON strings
 * are none|process|arena|shared_vm (MemTierName). */
typedef enum {
    MEM_TIER_NONE = 0,
    MEM_TIER_PROCESS,
    MEM_TIER_ARENA,
    MEM_TIER_SHARED_VM
} MemTier;

/* ----------------------------------------------------------------------
 * Pure guards - OS-independent, factored out of the live syscall so they
 * are unit- AND theft-host-PBT-testable on every host (the process tier is
 * the only one CI can execute; these arithmetic/decision guards are tested
 * everywhere). SAFETY PINS #1, #2, #3.
 * ---------------------------------------------------------------------- */

/*
 * MemParseU32 - Parse a wire address/length STRING into a 32-bit value
 * (spec: invariant AddressIsWellFormed, SAFETY PIN #1). Accepts hex
 * ("0x" / "0X" prefix) or decimal, in [0, 0xFFFFFFFF]. Returns 1 and sets
 * *out on a well-formed value; 0 (out untouched) on empty, garbage, a
 * trailing non-digit, or overflow past 32 bits. The reason addresses are
 * wire STRINGS at all: a full 32-bit address overflows the signed-int JSON
 * parser, so it can never ride the integer path.
 */
int MemParseU32(const char *s, unsigned long *out);

/*
 * MemRangeInBounds - The overflow-safe range guard (spec: invariant
 * MemoryAccessRangeBounded, SAFETY PIN #2 - the single most critical
 * arithmetic check). Returns 1 iff len <= cap AND addr + len does NOT wrap
 * past 0xFFFFFFFF (computed without 32-bit wraparound, the classic
 * off-by-overflow that turns a bounded read out-of-bounds). A len of 0 is
 * admitted (an empty access touches nothing).
 */
int MemRangeInBounds(unsigned long addr, unsigned long len, unsigned long cap);

/*
 * MemRegionAccessibleDecision - The pre-NT accessibility decision over a
 * VirtualQuery result (spec: invariant PreNtAccessGuarded, SAFETY PIN #3).
 * PURE over the MBI, so it is testable against a synthetic
 * MEMORY_BASIC_INFORMATION without touching real memory.
 *
 * Given the queried region info, the requested [addr, addr+len) and a write
 * flag, returns how many bytes of the requested prefix are accessible:
 *   == len   the whole range is accessible
 *   0<n<len  only the prefix [addr, addr+n) is accessible -> a peek CLAMPS
 *            to n (truncated), a poke is REJECTED (whole-or-nothing, never a
 *            partial pre-NT write into a guarded region)
 *   0        nothing is accessible -> reject
 * A region is read-accessible iff State == MEM_COMMIT and protection is not
 * PAGE_NOACCESS / PAGE_GUARD; write-accessible additionally requires a
 * writable protection (PAGE_READWRITE / PAGE_WRITECOPY / the EXECUTE_* write
 * variants). MEM_FREE / MEM_RESERVE yield 0.
 */
unsigned long MemRegionAccessibleDecision(const MEMORY_BASIC_INFORMATION *mbi,
                                          unsigned long addr,
                                          unsigned long len,
                                          int forWrite);

/* ----------------------------------------------------------------------
 * The tier (OS-family -> reach). Read from g_features (feat.h).
 * ---------------------------------------------------------------------- */

/*
 * MemTierCurrent - The device's tier from the OS family: is_nt -> process,
 * is_win9x -> arena, is_win32s -> shared_vm, else none (spec: enum MemTier).
 */
MemTier MemTierCurrent(void);

/*
 * MemTierName - The wire/JSON string for a tier
 * (none|process|arena|shared_vm). Static storage; never NULL.
 */
const char *MemTierName(MemTier tier);

/* ----------------------------------------------------------------------
 * The spawn-retain process table (process tier). The token is the SOLE
 * capability boundary (RetainedTokenValid). Tokens are never reused.
 * ---------------------------------------------------------------------- */

typedef struct {
    int  ok;                       /* 1 = retained; 0 = refused/failed */
    char token[MEM_TOKEN_LEN];     /* opaque "m<seq>-<rand>"; "" on failure */
    int  pid;
    char reason[96];               /* failure reason when !ok */
} MemSpawnResult;

/*
 * MemSpawnRetain - Launch argv[0..argc) and RETAIN its process handle as a
 * memory target (spec: rule ProcessRetained). A thin dedicated CreateProcessA
 * (hidden window, NO capture/wait, unlike ExecOpRun), retained in the bounded
 * table; returns a never-reused opaque token + pid.
 *
 * argv[0] MUST be catalogued (spec: invariant SpawnRetainCommandIsCatalogued,
 * SAFETY PIN #5) - else spawnRetain is a launch-anything bypass of the device
 * whitelist. The gate is the exec gate, identical semantics: when
 * unsafeMode != 0 OR cat == NULL the gate is unenforced (the /UNSAFE
 * precedent); otherwise argv[0] must resolve via CatalogLookup. Shell
 * builtins are NOT a valid spawn-retain target (no process to retain) and are
 * refused.
 *
 * A full table -> ok = 0 with an explanatory reason (no silent eviction).
 */
void MemSpawnRetain(const Catalog *cat, int unsafeMode,
                    const char **argv, int argc, MemSpawnResult *out);

/*
 * MemTokenHandle - Resolve a token to its retained process HANDLE, or NULL
 * if the token is unknown, released or terminated (spec: invariant
 * RetainedTokenValid, SAFETY PIN #4: only a live `retained`-status slot
 * whose token string-matches resolves). The handle never crosses the wire;
 * it is internal device state.
 */
HANDLE MemTokenHandle(const char *token);

/*
 * MemTokenPid / MemTokenCommand - Accessors for a live token's pid and
 * catalogued command line (used to attribute an audit record). 0 / "" when
 * the token is not live.
 */
int         MemTokenPid(const char *token);
const char *MemTokenCommand(const char *token);

/*
 * MemRelease - Relinquish the retained handle and free the slot WITHOUT
 * killing the child (spec: rule ProcessReleased - status retained ->
 * released). The token is consumed; never valid again. Returns 1 if the
 * token was live (retained), 0 otherwise (absent / already consumed).
 */
int MemRelease(const char *token);

/*
 * MemTerminate - TerminateProcess the retained child and free the slot
 * (spec: rule ProcessTerminated - status retained -> terminated). The token
 * is consumed. Returns 1 if the token was live, 0 otherwise.
 */
int MemTerminate(const char *token);

/*
 * MemReleaseAll - Close every retained handle and clear the table. Called on
 * connection close (the accept loop) so a disconnect never leaks a handle.
 */
void MemReleaseAll(void);

/* ----------------------------------------------------------------------
 * peek / poke - the access path. Both enforce the address/range/token/region
 * floor (mem_access_valid) before any load/store.
 * ---------------------------------------------------------------------- */

typedef struct {
    int           ok;              /* 1 = read produced; 0 = refused */
    int           has_process;     /* 1 on the process tier (token target) */
    unsigned long bytes_read;
    int           truncated;       /* pre-NT clamp into an accessible prefix */
    char          reason[96];      /* refusal reason when !ok */
} MemPeekResult;

typedef struct {
    int           ok;              /* 1 = write produced; 0 = refused */
    int           has_process;     /* 1 on the process tier */
    unsigned long bytes_written;
    int           partial;         /* NT+ ERROR_PARTIAL_COPY prefix; pre-NT 0 */
    char          reason[96];      /* refusal reason when !ok */
} MemPokeResult;

/*
 * MemPeek - Read [addr, addr+len) from the target (spec: rule MemoryPeeked).
 * On the process tier, `token` names the retained child (RetainedTokenValid);
 * on the pre-NT tiers `token` is NULL/"" and the read is the raw shared-VM /
 * arena address (PreNtAccessGuarded: a range spanning a non-accessible region
 * CLAMPS to the accessible prefix with truncated = 1; a non-committed region
 * reads nothing). A read needs no arm. The floor: AddressIsWellFormed (caller
 * passes the already-parsed addr/len) + MemoryAccessRangeBounded (re-checked)
 * + RetainedTokenValid. `out` must hold at least `len` bytes.
 */
void MemPeek(const char *token, unsigned long addr, unsigned long len,
             unsigned char *out, MemPeekResult *res);

/*
 * MemPoke - Write src[0..len) to [addr, addr+len) (spec: rule MemoryPoked -
 * the most dangerous operation, so the precondition carries every write
 * floor at once):
 *   - memory_write_armed:  AuditIsArmed() (the device /ALLOWMEMWRITE wire arm,
 *                          PokeRequiresBothArmingLayers device half). Clear ->
 *                          refused, binding every client.
 *   - mem_access_valid:    range/token/region floor (a non-writable pre-NT
 *                          region is REJECTED WHOLE - never a partial pre-NT
 *                          write; the NT+ tier lets WriteProcessMemory report
 *                          a legitimate partial).
 *   - audit_writable:      AuditWritePoke() succeeds. Checked so that a poke
 *                          that cannot be recorded performs NO write
 *                          (PokeIsAuditedFailClosed, SAFETY PIN #6) - the audit
 *                          record is written for exactly the bytes the poke
 *                          commits, before the result is produced.
 * The audit `command` attribution is the retained child's catalogued command
 * (process tier) or the tier name (pre-NT).
 */
void MemPoke(const char *token, unsigned long addr,
             const unsigned char *src, unsigned long len,
             MemPokeResult *res);

#ifdef TEST_BUILD
/*
 * MemResetForTest - Clear the process table and the never-reused token
 * sequence so each test starts from a known state. Test-only.
 */
void MemResetForTest(void);
#endif

#endif /* MEM_OPS_H */
