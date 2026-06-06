/*
 * feat.h - Runtime feature detection for capability uplift
 *
 * The binary's baseline target is Win32s 1.25a: every required path
 * works there. On newer hosts (Win 9x, NT, XP, Win 10+) FeatInit probes
 * APIs via GetProcAddress at startup and records the results here, so
 * exec/pty paths can uplift at runtime. None of the delay-loaded APIs
 * may be referenced by name at link time - they exist only as the p*
 * function pointers below - otherwise the import resolver rejects the
 * binary on Win32s.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef FEAT_H
#define FEAT_H

#include <windows.h>

/* Flag masks for FeatForceFallback (test-only) */
#define FEAT_FORCE_NO_THREADS      0x0001
#define FEAT_FORCE_NO_JOB_OBJECTS  0x0002
#define FEAT_FORCE_NO_CTRL_EVENTS  0x0004
#define FEAT_FORCE_NO_PTY          0x0008
#define FEAT_FORCE_NO_BINARY_TYPE  0x0010

/*
 * COORD_SIZE_T - SIZE_T is not in old SDK headers; use DWORD-compatible
 * pointer-sized unsigned for the attribute-list size out-param.
 */
typedef unsigned long FeatSizeT;

typedef struct {
    /* OS version */
    int win_major;
    int win_minor;
    int win_build;
    int is_win32s;       /* GetVersion high bit + major==3 (+ thread probe) */
    int is_win9x;        /* GetVersion high bit + major==4 */
    int is_nt;           /* GetVersion high bit clear */
    int is_wow64;        /* IsWow64Process(GetCurrentProcess()) - defaults 0 */

    /* Boolean capability flags (mirror function-pointer presence) */
    int has_threads;
    int has_create_job_object;
    int has_get_binary_type;
    int has_is_wow64_process;
    int has_generate_ctrl_event;
    int has_query_full_image_name;
    int has_create_pseudo_console;
    int has_proc_thread_attr_list;
    int has_set_process_mitigation;

    /* Function pointers - NULL when capability absent */
    HANDLE  (WINAPI *pCreateJobObjectA)(LPSECURITY_ATTRIBUTES, LPCSTR);
    BOOL    (WINAPI *pAssignProcessToJobObject)(HANDLE, HANDLE);
    BOOL    (WINAPI *pSetInformationJobObject)(HANDLE, int, LPVOID, DWORD);
    BOOL    (WINAPI *pGetBinaryTypeA)(LPCSTR, LPDWORD);
    BOOL    (WINAPI *pIsWow64Process)(HANDLE, BOOL *);
    BOOL    (WINAPI *pGenerateConsoleCtrlEvent)(DWORD, DWORD);
    BOOL    (WINAPI *pQueryFullProcessImageNameA)(HANDLE, DWORD, LPSTR, LPDWORD);
    LONG    (WINAPI *pCreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, void **);
    void    (WINAPI *pClosePseudoConsole)(void *);
    LONG    (WINAPI *pResizePseudoConsole)(void *, COORD);
    BOOL    (WINAPI *pInitializeProcThreadAttributeList)(LPVOID, DWORD, DWORD, FeatSizeT *);
    BOOL    (WINAPI *pUpdateProcThreadAttribute)(LPVOID, DWORD, DWORD, PVOID, FeatSizeT, PVOID, FeatSizeT *);
    void    (WINAPI *pDeleteProcThreadAttributeList)(LPVOID);
    BOOL    (WINAPI *pSetProcessMitigationPolicy)(int, PVOID, FeatSizeT);
} Features;

extern Features g_features;

/*
 * FeatInit - Probe the host once, before any exec / catalog load /
 * ready message. Populates g_features.
 */
void FeatInit(void);

/*
 * FeatVersionString - Human-readable host description for the ready
 * message, e.g. "Windows 10.0.19045 (NT)". Static buffer; valid after
 * FeatInit.
 */
const char *FeatVersionString(void);

/*
 * FeatForceFallback - Test-only: zero out the selected capability flags
 * AND their function pointers (FEAT_FORCE_* mask), so fallback code
 * paths can be exercised on uplifted hosts. Returns the mask applied.
 */
int FeatForceFallback(int flagsMask);

#endif /* FEAT_H */
