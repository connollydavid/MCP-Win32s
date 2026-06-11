/*
 * mini_crt.c - a minimal, freestanding C runtime for the Win32s 1.25a target.
 *
 * WHY THIS EXISTS. The MinGW-w64 toolchain links the C runtime against
 * msvcrt.dll. msvcrt.dll is NOT present on a bare Windows 3.1 + Win32s 1.25a
 * system (Win32s ships the older CRTDLL.DLL instead), so a normally-linked
 * mcp-w32s.exe fails to load there with "Cannot find MSVCRT.DLL". CI never
 * caught this because the PEs run under Wine, which provides msvcrt.dll. This
 * was surfaced by the Phase 6 real-hardware test (see plan/PHASE6.md Finding
 * #1) and matches the device's own stated goal of "zero DLL dependencies
 * beyond what the OS provides".
 *
 * WHAT IT DOES. It is compiled ONLY into mcp-w32s.exe (the deployed device;
 * the test binaries keep the ordinary CRT, since they run on the dev host /
 * Wine, not on Win32s). Linked with -nostdlib, it supplies (a) the program
 * entry point and (b) the handful of C-library functions the device actually
 * uses or the compiler emits. Every routine is a thin wrapper over an API that
 * Win32s 1.25a guarantees (kernel32 + user32), so the resulting binary imports
 * ONLY kernel32 and user32. No msvcrt, no CRTDLL, no external runtime.
 *
 * The device's main() takes no arguments and reads GetCommandLineA() itself,
 * so the entry point needs no argv construction at all.
 *
 * Strictly C89 (matches the project's language constraint). No floating point,
 * no 486+ instructions (the bodies are plain byte/word loops and Win32 calls).
 *
 * This is free and unencumbered software released into the public domain.
 */

#include <windows.h>
#include <stddef.h>     /* size_t only - no CRT */

/* The real program. Defined in mcp-w32s.c. It reads GetCommandLineA(). */
extern int main(void);

/*
 * Program entry point. Named mainCRTStartup so the linker uses it as the
 * console-subsystem entry without an explicit -e. No CRT startup runs: we just
 * call main() and turn its return value into the process exit code. (GUI
 * threads, atexit, locale, stdio buffering, signal setup - none are needed by
 * this server.)
 */
void mainCRTStartup(void)
{
    ExitProcess((UINT)main());
}

/*
 * GCC emits a call to __main in the prologue of main() (it normally runs C++
 * global constructors via __do_global_ctors). This is a C program with none,
 * so an empty stub is correct and complete.
 */
void __main(void)
{
}

/* ----------------------------------------------------------------------- */
/* Heap. malloc/calloc/free over the process heap (kernel32).              */
/* ----------------------------------------------------------------------- */

void *malloc(size_t n)
{
    /* HeapAlloc(0 bytes) is well-defined and returns a valid pointer; keep
       the libc contract that malloc(0) yields a free-able pointer. */
    return HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n);
}

void *calloc(size_t count, size_t size)
{
    SIZE_T total = (SIZE_T)count * (SIZE_T)size;
    /* Guard the multiply overflow (cheap, no FP): if it wrapped, fail. */
    if (size != 0 && (total / (SIZE_T)size) != (SIZE_T)count) {
        return NULL;
    }
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
}

void free(void *p)
{
    if (p != NULL) {
        HeapFree(GetProcessHeap(), 0, p);
    }
}

/* ----------------------------------------------------------------------- */
/* Process termination.                                                    */
/* ----------------------------------------------------------------------- */

void exit(int code)
{
    ExitProcess((UINT)code);
}

void abort(void)
{
    ExitProcess((UINT)3);
}

/* ----------------------------------------------------------------------- */
/* Memory and string primitives.                                           */
/*                                                                         */
/* These are provided even where the device's own source uses only some of */
/* them, because GCC also EMITS calls to memcpy/memset/memmove/memcmp (and  */
/* occasionally strlen) for aggregate copies and initialisers - the        */
/* freestanding contract requires the program to supply them. Plain C89.   */
/* ----------------------------------------------------------------------- */

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned char  v = (unsigned char)c;
    while (n-- > 0) {
        *d++ = v;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n-- > 0) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        while (n-- > 0) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n-- > 0) {
            *--d = *--s;
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n-- > 0) {
        if (*pa != *pb) {
            return (int)*pa - (int)*pb;
        }
        pa++;
        pb++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p != '\0') {
        p++;
    }
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n-- > 0) {
        if (*a != *b) {
            return (int)(unsigned char)*a - (int)(unsigned char)*b;
        }
        if (*a == '\0') {
            break;
        }
        a++;
        b++;
    }
    return 0;
}
