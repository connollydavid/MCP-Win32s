/*
 * mem_ops.c - Tiered, user-mode memory peek/poke.
 *
 * The device's highest-stakes capability: bounded, consented, logged ring-3
 * memory read/write. See mem_ops.h for the contract and memory-ops.allium for
 * the spec. We use exactly what each OS family grants ring-3 - never escalating
 * (no ring-0, VxD, call gate). The reach TIER follows the OS family:
 *
 *   process    (NT+):     ReadProcessMemory/WriteProcessMemory on a child we
 *                         launched and retain, named by an opaque token. The
 *                         spawn-retain table is the SOLE process-tier target
 *                         boundary - no OpenProcess-by-PID, ever.
 *   arena      (Win9x):   direct guarded loads/stores into the reachable arenas.
 *   shared_vm  (Win32s):  direct guarded loads/stores into the single shared VM.
 *
 * ReadProcessMemory/WriteProcessMemory/VirtualQueryEx are NT-only cross-process
 * APIs: they are resolved via GetProcAddress (NEVER statically imported) so the
 * binary stays loadable on bare Win32s. Local VirtualQuery/IsBadReadPtr (the
 * pre-NT guard) are Win32s-safe and called directly.
 *
 * The two arithmetic guards (MemParseU32, MemRangeInBounds) reference no Win32
 * type, so they compile natively for the theft host PBT harness under the
 * MEM_OPS_HOST_PURE guard (the rest of this file needs the full Win32 surface).
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

/* ------------------------------------------------------------------
 * Pure arithmetic guards (SAFETY PINS #1 and #2). These reference no
 * Win32 type, so the theft host harness pulls in only this section by
 * compiling with MEM_OPS_HOST_PURE; the live Win32 paths below are then
 * excluded (the host shim has no MEMORY_BASIC_INFORMATION/CreateProcessA).
 * ------------------------------------------------------------------ */

#include <stddef.h>     /* NULL - available before windows.h on the host build */

/*
 * MemParseU32 - parse a wire address/length string into a 32-bit value
 * (SAFETY PIN #1). Hex ("0x"/"0X" prefix) or decimal, in [0, 0xFFFFFFFF].
 * Returns 1 and sets *out on a well-formed value; 0 (out untouched) on empty,
 * garbage, a trailing non-digit, or overflow past 32 bits.
 */
int MemParseU32(const char *s, unsigned long *out)
{
    unsigned long val;
    unsigned long digit;
    int isHex;
    int sawDigit;

    if (s == NULL || s[0] == '\0') {
        return 0;
    }

    isHex = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        isHex = 1;
        s += 2;
        if (s[0] == '\0') {
            return 0;           /* "0x" with no digits */
        }
    }

    val = 0;
    sawDigit = 0;
    for (; s[0] != '\0'; s++) {
        char c = s[0];
        if (c >= '0' && c <= '9') {
            digit = (unsigned long)(c - '0');
        } else if (isHex && c >= 'a' && c <= 'f') {
            digit = (unsigned long)(c - 'a' + 10);
        } else if (isHex && c >= 'A' && c <= 'F') {
            digit = (unsigned long)(c - 'A' + 10);
        } else {
            return 0;           /* trailing non-digit / garbage */
        }

        if (isHex) {
            /* Overflow past 32 bits: any nonzero nibble above bit 28. */
            if (val > (0xFFFFFFFFUL >> 4)) {
                return 0;
            }
            val = (val << 4) | digit;
        } else {
            /* Decimal overflow guard without 64-bit math. */
            if (val > 0xFFFFFFFFUL / 10UL) {
                return 0;
            }
            val *= 10UL;
            if (val > 0xFFFFFFFFUL - digit) {
                return 0;
            }
            val += digit;
        }
        sawDigit = 1;
    }

    if (!sawDigit) {
        return 0;
    }
    *out = val;
    return 1;
}

/*
 * MemRangeInBounds - the overflow-safe range guard (SAFETY PIN #2). Returns 1
 * iff len <= cap AND addr + len does not wrap past 0xFFFFFFFF, computed without
 * 32-bit wraparound. A len of 0 is admitted (an empty access touches nothing).
 */
int MemRangeInBounds(unsigned long addr, unsigned long len, unsigned long cap)
{
    if (len > cap) {
        return 0;
    }
    /* addr + len <= 0xFFFFFFFF, rearranged so the sum never overflows. */
    if (addr > 0xFFFFFFFFUL - len) {
        return 0;
    }
    return 1;
}

#ifndef MEM_OPS_HOST_PURE

#include <windows.h>
#include "mem_ops.h"
#include "feat.h"
#include "audit.h"
#include "argv.h"

/* MEM_WRITECOPY-style protection constants may be absent from very old SDK
 * headers; define under guards so the decision logic always builds. */
#ifndef PAGE_NOACCESS
#define PAGE_NOACCESS          0x01
#endif
#ifndef PAGE_READONLY
#define PAGE_READONLY          0x02
#endif
#ifndef PAGE_READWRITE
#define PAGE_READWRITE         0x04
#endif
#ifndef PAGE_WRITECOPY
#define PAGE_WRITECOPY         0x08
#endif
#ifndef PAGE_EXECUTE_READWRITE
#define PAGE_EXECUTE_READWRITE 0x40
#endif
#ifndef PAGE_EXECUTE_WRITECOPY
#define PAGE_EXECUTE_WRITECOPY 0x80
#endif
#ifndef PAGE_GUARD
#define PAGE_GUARD             0x100
#endif

#ifndef ERROR_PARTIAL_COPY
#define ERROR_PARTIAL_COPY     299L
#endif

/* The largest CreateProcessA command line (incl. NUL); mirrors exec_ops. */
#define MEM_MAX_CMDLINE 32766

/* ------------------------------------------------------------------
 * Delay-loaded NT cross-process APIs. Resolved via GetProcAddress on
 * first use so the binary loads on bare Win32s (a static import would
 * make the loader reject it). The typedefs match the WINAPI signatures.
 * ------------------------------------------------------------------ */

typedef BOOL (WINAPI *MemRpmFn)(HANDLE, LPCVOID, LPVOID, FeatSizeT, FeatSizeT *);
typedef BOOL (WINAPI *MemWpmFn)(HANDLE, LPVOID, LPCVOID, FeatSizeT, FeatSizeT *);
typedef FeatSizeT (WINAPI *MemVqxFn)(HANDLE, LPCVOID,
                                     PMEMORY_BASIC_INFORMATION, FeatSizeT);

static MemRpmFn g_pReadProcessMemory = NULL;
static MemWpmFn g_pWriteProcessMemory = NULL;
static MemVqxFn g_pVirtualQueryEx = NULL;
static int      g_ntResolved = 0;

/*
 * MemResolveNt - resolve the NT cross-process APIs once. Leaves the pointers
 * NULL on a host that lacks them (pre-NT), so callers fall through to the
 * pre-NT guarded local path.
 */
static void MemResolveNt(void)
{
    HMODULE k32;

    if (g_ntResolved) {
        return;
    }
    g_ntResolved = 1;
    k32 = GetModuleHandleA("kernel32.dll");
    if (k32 == NULL) {
        return;
    }
    g_pReadProcessMemory =
        (MemRpmFn)GetProcAddress(k32, "ReadProcessMemory");
    g_pWriteProcessMemory =
        (MemWpmFn)GetProcAddress(k32, "WriteProcessMemory");
    g_pVirtualQueryEx =
        (MemVqxFn)GetProcAddress(k32, "VirtualQueryEx");
}

/* ------------------------------------------------------------------
 * The pre-NT accessibility decision (SAFETY PIN #3). Pure over the MBI.
 * ------------------------------------------------------------------ */

unsigned long MemRegionAccessibleDecision(const MEMORY_BASIC_INFORMATION *mbi,
                                          unsigned long addr,
                                          unsigned long len,
                                          int forWrite)
{
    unsigned long regionBase;
    unsigned long regionEnd;
    unsigned long avail;
    DWORD prot;

    if (mbi == NULL) {
        return 0;
    }
    /* Only committed memory is ever accessible. */
    if (mbi->State != MEM_COMMIT) {
        return 0;
    }

    prot = mbi->Protect;
    /* A guard page or no-access page is never readable. */
    if (prot & PAGE_GUARD) {
        return 0;
    }
    if (prot == PAGE_NOACCESS) {
        return 0;
    }
    if (forWrite) {
        /* Write needs an explicitly writable protection. */
        if (!(prot == PAGE_READWRITE ||
              prot == PAGE_WRITECOPY ||
              prot == PAGE_EXECUTE_READWRITE ||
              prot == PAGE_EXECUTE_WRITECOPY)) {
            return 0;
        }
    }

    /* The requested prefix that falls inside this region. */
    regionBase = (unsigned long)mbi->BaseAddress;
    regionEnd = regionBase + (unsigned long)mbi->RegionSize;
    if (addr < regionBase || addr >= regionEnd) {
        return 0;               /* addr not in the queried region */
    }
    avail = regionEnd - addr;
    if (avail >= len) {
        return len;             /* whole range accessible */
    }
    return avail;               /* partial prefix */
}

/* ------------------------------------------------------------------
 * The tier (OS family -> reach).
 * ------------------------------------------------------------------ */

MemTier MemTierCurrent(void)
{
    if (g_features.is_nt) {
        return MEM_TIER_PROCESS;
    }
    if (g_features.is_win9x) {
        return MEM_TIER_ARENA;
    }
    if (g_features.is_win32s) {
        return MEM_TIER_SHARED_VM;
    }
    return MEM_TIER_NONE;
}

const char *MemTierName(MemTier tier)
{
    switch (tier) {
    case MEM_TIER_PROCESS:   return "process";
    case MEM_TIER_ARENA:     return "arena";
    case MEM_TIER_SHARED_VM: return "shared_vm";
    default:                 return "none";
    }
}

/* ------------------------------------------------------------------
 * The spawn-retain process table. 8 static slots, no heap. The token is
 * the SOLE capability boundary (RetainedTokenValid); tokens are never
 * reused (a monotonic seq that only increments).
 * ------------------------------------------------------------------ */

typedef struct {
    int    in_use;
    char   token[MEM_TOKEN_LEN];
    HANDLE handle;
    int    pid;
    char   command[MEM_MAX_CMDLINE];
} MemSlot;

static MemSlot       g_slots[MEM_PROC_SLOTS];
static unsigned long g_seq = 0;        /* monotonic, never reused */

/*
 * MemMakeToken - build "m<seq>-<lcg>" into out. The LCG suffix is integer-only
 * hardening (NOT the security boundary - that is table membership). wsprintfA
 * keeps the format integer-only.
 */
static void MemMakeToken(unsigned long seq, int pid, char *out, int outSize)
{
    unsigned long lcg;

    lcg = (unsigned long)GetTickCount() + seq + (unsigned long)pid;
    lcg = lcg * 1103515245UL + 12345UL;
    /* Five decimal digits of the LCG state for guess-resistance. */
    lcg = lcg % 100000UL;
    if (outSize > 0) {
        wsprintfA(out, "m%lu-%lu", seq, lcg);
    }
}

/*
 * MemFindSlot - locate the live slot whose token string-matches, or NULL.
 */
static MemSlot *MemFindSlot(const char *token)
{
    int i;

    if (token == NULL || token[0] == '\0') {
        return NULL;
    }
    for (i = 0; i < MEM_PROC_SLOTS; i++) {
        if (g_slots[i].in_use && lstrcmpA(g_slots[i].token, token) == 0) {
            return &g_slots[i];
        }
    }
    return NULL;
}

void MemSpawnRetain(const Catalog *cat, int unsafeMode,
                    const char **argv, int argc, MemSpawnResult *out)
{
    const CatalogEntry *entry;
    MemSlot *slot;
    char cmdLine[MEM_MAX_CMDLINE];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    UINT oldErrMode;
    BOOL spawnOk;
    int i;

    out->ok = 0;
    out->token[0] = '\0';
    out->pid = 0;
    out->reason[0] = '\0';

    if (argv == NULL || argc <= 0 || argv[0] == NULL || argv[0][0] == '\0') {
        lstrcpynA(out->reason, "empty command", (int)sizeof(out->reason));
        return;
    }

    /* Catalog gate (SAFETY PIN #5), identical semantics to HandleExec: when
     * unsafeMode != 0 OR cat == NULL the gate is unenforced; otherwise argv[0]
     * must resolve, and a shell builtin is not a retainable target. */
    if (unsafeMode == 0 && cat != NULL) {
        entry = CatalogLookup(cat, argv[0]);
        if (entry == NULL) {
            lstrcpynA(out->reason, "command not in catalog",
                      (int)sizeof(out->reason));
            return;
        }
        if (CatalogEntryIsBuiltin(entry)) {
            lstrcpynA(out->reason, "shell builtin is not a spawn-retain target",
                      (int)sizeof(out->reason));
            return;
        }
    }

    /* Find a free slot before spawning (no eviction on a full table). */
    slot = NULL;
    for (i = 0; i < MEM_PROC_SLOTS; i++) {
        if (!g_slots[i].in_use) {
            slot = &g_slots[i];
            break;
        }
    }
    if (slot == NULL) {
        lstrcpynA(out->reason, "process table full", (int)sizeof(out->reason));
        return;
    }

    /* Build the command line from argv (CreateProcessA quoting). */
    if (ArgvJoin(argv, argc, cmdLine, (int)sizeof(cmdLine)) < 0) {
        lstrcpynA(out->reason, "command line too long",
                  (int)sizeof(out->reason));
        return;
    }

    /* Thin dedicated spawn: hidden window, no pipes, no wait. Error mode
     * suppresses hard-error dialogs (a hidden child popping a blocking dialog
     * would otherwise wedge the server). */
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));

    oldErrMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    spawnOk = CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE,
                             0, NULL, NULL, &si, &pi);
    SetErrorMode(oldErrMode);

    if (!spawnOk) {
        wsprintfA(out->reason, "spawn failed: %lu",
                  (unsigned long)GetLastError());
        return;
    }

    /* Retain the process handle; the thread handle is not needed. */
    CloseHandle(pi.hThread);

    g_seq++;
    MemMakeToken(g_seq, (int)pi.dwProcessId, slot->token,
                 (int)sizeof(slot->token));
    slot->in_use = 1;
    slot->handle = pi.hProcess;
    slot->pid = (int)pi.dwProcessId;
    lstrcpynA(slot->command, cmdLine, (int)sizeof(slot->command));

    lstrcpynA(out->token, slot->token, (int)sizeof(out->token));
    out->pid = slot->pid;
    out->ok = 1;
}

HANDLE MemTokenHandle(const char *token)
{
    MemSlot *slot = MemFindSlot(token);
    return slot != NULL ? slot->handle : NULL;
}

int MemTokenPid(const char *token)
{
    MemSlot *slot = MemFindSlot(token);
    return slot != NULL ? slot->pid : 0;
}

const char *MemTokenCommand(const char *token)
{
    MemSlot *slot = MemFindSlot(token);
    return slot != NULL ? slot->command : "";
}

int MemRelease(const char *token)
{
    MemSlot *slot = MemFindSlot(token);

    if (slot == NULL) {
        return 0;
    }
    CloseHandle(slot->handle);
    slot->in_use = 0;
    slot->handle = NULL;
    return 1;
}

int MemTerminate(const char *token)
{
    MemSlot *slot = MemFindSlot(token);

    if (slot == NULL) {
        return 0;
    }
    TerminateProcess(slot->handle, 1);
    CloseHandle(slot->handle);
    slot->in_use = 0;
    slot->handle = NULL;
    return 1;
}

void MemReleaseAll(void)
{
    int i;

    for (i = 0; i < MEM_PROC_SLOTS; i++) {
        if (g_slots[i].in_use) {
            CloseHandle(g_slots[i].handle);
            g_slots[i].in_use = 0;
            g_slots[i].handle = NULL;
        }
    }
}

/* ------------------------------------------------------------------
 * peek / poke - the access path. Both enforce the range/token/region
 * floor before any load/store.
 * ------------------------------------------------------------------ */

void MemPeek(const char *token, unsigned long addr, unsigned long len,
             unsigned char *out, MemPeekResult *res)
{
    MemTier tier;

    res->ok = 0;
    res->has_process = 0;
    res->bytes_read = 0;
    res->truncated = 0;
    res->reason[0] = '\0';

    if (!MemRangeInBounds(addr, len, MEM_MAX_ACCESS)) {
        lstrcpynA(res->reason, "range out of bounds", (int)sizeof(res->reason));
        return;
    }

    tier = MemTierCurrent();
    MemResolveNt();

    /* A device with no memory tier never reads (defence in depth: the bridge
     * prunes the tools, but a direct wire client must be refused too). */
    if (tier == MEM_TIER_NONE) {
        lstrcpynA(res->reason, "no memory capability", (int)sizeof(res->reason));
        return;
    }

    if (tier == MEM_TIER_PROCESS) {
        /* Process tier: a token is REQUIRED. A token-less peek must never fall
         * through to the local path - that would read the device's OWN memory
         * rather than a retained child (the spawn-retain table is the SOLE
         * process-tier target boundary; process = null is pre-NT only). */
        HANDLE h;
        FeatSizeT got;

        if (token == NULL || token[0] == '\0') {
            lstrcpynA(res->reason, "token required on the process tier",
                      (int)sizeof(res->reason));
            return;
        }
        h = MemTokenHandle(token);
        if (h == NULL) {
            lstrcpynA(res->reason, "invalid token", (int)sizeof(res->reason));
            return;
        }
        if (g_pReadProcessMemory == NULL) {
            lstrcpynA(res->reason, "ReadProcessMemory unavailable",
                      (int)sizeof(res->reason));
            return;
        }
        res->has_process = 1;
        got = 0;
        if (g_pReadProcessMemory(h, (LPCVOID)addr, out, (FeatSizeT)len, &got)) {
            res->bytes_read = (unsigned long)got;
            res->ok = 1;
            return;
        }
        if (GetLastError() == (DWORD)ERROR_PARTIAL_COPY) {
            res->bytes_read = (unsigned long)got;
            res->truncated = 1;
            res->ok = 1;
            return;
        }
        lstrcpynA(res->reason, "read failed", (int)sizeof(res->reason));
        return;
    }

    /* Pre-NT tiers (arena/shared_vm) ONLY: local guarded read into the shared
     * address space. The accessible prefix is computed from VirtualQuery; a
     * spanning range clamps. NOTE: never taken on the NT dev host/CI (tier is
     * process) - live verification is on real pre-NT hardware, a later
     * work-item (OBLIGATIONS-5.3.md). */
    {
        MEMORY_BASIC_INFORMATION mbi;
        unsigned long n;

        memset(&mbi, 0, sizeof(mbi));
        if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == 0) {
            lstrcpynA(res->reason, "address not accessible",
                      (int)sizeof(res->reason));
            return;
        }
        n = MemRegionAccessibleDecision(&mbi, addr, len, 0);
        if (n == 0) {
            lstrcpynA(res->reason, "address not accessible",
                      (int)sizeof(res->reason));
            return;
        }
        if (n < len) {
            res->truncated = 1;
        }
        memcpy(out, (const void *)addr, (size_t)n);
        res->bytes_read = n;
        res->ok = 1;
    }
}

void MemPoke(const char *token, unsigned long addr,
             const unsigned char *src, unsigned long len,
             MemPokeResult *res)
{
    MemTier tier;
    HANDLE h;
    const char *auditTok;
    const char *auditCmd;
    int auditPid;

    res->ok = 0;
    res->has_process = 0;
    res->bytes_written = 0;
    res->partial = 0;
    res->reason[0] = '\0';

    /* (1) Device write arm (/ALLOWMEMWRITE), the device half of the two-layer
     * arm. Clear -> refused with no write, binding every client. */
    if (!AuditIsArmed()) {
        lstrcpynA(res->reason, "memory writes not armed (/ALLOWMEMWRITE)",
                  (int)sizeof(res->reason));
        return;
    }

    /* (2) Overflow-safe range floor. */
    if (!MemRangeInBounds(addr, len, MEM_MAX_ACCESS)) {
        lstrcpynA(res->reason, "range out of bounds", (int)sizeof(res->reason));
        return;
    }

    tier = MemTierCurrent();
    MemResolveNt();
    h = NULL;

    /* A device with no memory tier never writes (defence in depth: the bridge
     * prunes the tool, but a direct wire client must be refused too). */
    if (tier == MEM_TIER_NONE) {
        lstrcpynA(res->reason, "no memory capability", (int)sizeof(res->reason));
        return;
    }

    /* (3) Target / region floor. Process tier: a token is REQUIRED - a
     * token-less poke must never fall through to the local path, which would
     * write the device's OWN memory rather than a retained child. Pre-NT:
     * VirtualQuery + a writable-region decision; a non-writable or spanning
     * region is rejected WHOLE (never a partial pre-NT write). */
    if (tier == MEM_TIER_PROCESS) {
        if (token == NULL || token[0] == '\0') {
            lstrcpynA(res->reason, "token required on the process tier",
                      (int)sizeof(res->reason));
            return;
        }
        h = MemTokenHandle(token);
        if (h == NULL) {
            lstrcpynA(res->reason, "invalid token", (int)sizeof(res->reason));
            return;
        }
        if (g_pWriteProcessMemory == NULL) {
            lstrcpynA(res->reason, "WriteProcessMemory unavailable",
                      (int)sizeof(res->reason));
            return;
        }
        res->has_process = 1;
    } else {
        /* Pre-NT (arena/shared_vm) guarded path ONLY - never taken on NT
         * (real pre-NT hardware only). */
        MEMORY_BASIC_INFORMATION mbi;
        unsigned long n;

        memset(&mbi, 0, sizeof(mbi));
        if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == 0) {
            lstrcpynA(res->reason, "address not writable",
                      (int)sizeof(res->reason));
            return;
        }
        n = MemRegionAccessibleDecision(&mbi, addr, len, 1);
        if (n < len) {
            lstrcpynA(res->reason, "address not writable",
                      (int)sizeof(res->reason));
            return;
        }
    }

    /* (4) Audit sink writable (fail-closed): a poke that cannot be recorded
     * performs NO write. */
    if (!AuditIsWritable()) {
        lstrcpynA(res->reason, "audit sink not writable",
                  (int)sizeof(res->reason));
        return;
    }

    /* All floors passed - perform the write, then record the ACTUAL bytes. */
    if (res->has_process) {
        FeatSizeT wrote;

        wrote = 0;
        if (g_pWriteProcessMemory(h, (LPVOID)addr, src, (FeatSizeT)len,
                                  &wrote)) {
            res->bytes_written = (unsigned long)wrote;
        } else if (GetLastError() == (DWORD)ERROR_PARTIAL_COPY) {
            res->bytes_written = (unsigned long)wrote;
            res->partial = 1;
        } else {
            lstrcpynA(res->reason, "write failed", (int)sizeof(res->reason));
            return;
        }
    } else {
        /* Pre-NT: the whole range was admitted; store it directly. */
        memcpy((void *)addr, src, (size_t)len);
        res->bytes_written = len;
    }

    /* Audit the actual write (spec PokeIsAuditedFailClosed: the record matches
     * the result). Attribute to the retained child's command on the process
     * tier, the tier name pre-NT. */
    auditTok = (token != NULL) ? token : "";
    auditPid = MemTokenPid(token);
    auditCmd = res->has_process ? MemTokenCommand(token) : MemTierName(tier);
    if (auditCmd[0] == '\0') {
        auditCmd = MemTierName(tier);
    }
    /* Fail LOUD if the record could not be written (elicit decision 4): the
     * AuditIsWritable pre-check makes this near-impossible (the sink was
     * writable a moment ago), but a genuine post-check failure (disk full,
     * permission change) must never be silent. The mutation already happened;
     * surface it as an error so the write is never reported as a clean,
     * unlogged success. */
    if (!AuditWritePoke(MemTierName(tier), auditTok, auditPid, auditCmd,
                        addr, len, res->bytes_written, res->partial)) {
        lstrcpynA(res->reason,
                  "audit write failed after the memory was modified - "
                  "investigate the audit log",
                  (int)sizeof(res->reason));
        return;             /* res->ok stays 0: a write is never silently unlogged */
    }

    res->ok = 1;
}

#ifdef TEST_BUILD
void MemResetForTest(void)
{
    int i;

    for (i = 0; i < MEM_PROC_SLOTS; i++) {
        g_slots[i].in_use = 0;
        g_slots[i].handle = NULL;
        g_slots[i].pid = 0;
        g_slots[i].token[0] = '\0';
        g_slots[i].command[0] = '\0';
    }
    g_seq = 0;
}
#endif

#endif /* MEM_OPS_HOST_PURE */
