/*
 * feat.c - Runtime feature detection for capability uplift
 *
 * Probes the host once at startup (FeatInit) and records OS version plus
 * the availability of delay-loaded APIs in g_features. The delay-loaded
 * APIs are resolved via GetProcAddress(GetModuleHandleA("kernel32"), ...)
 * and stored as function pointers, so none of them is referenced by name
 * at link time - that is what lets the binary load on Win32s 1.25a, where
 * most of those symbols are absent from the import resolver.
 *
 * CreateThread IS a legal static import (it is present in the Win32s
 * import library; it merely fails at runtime there), so the thread probe
 * calls it directly.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <windows.h>
#include "feat.h"

Features g_features;

/* Static buffer for FeatVersionString. */
static char g_version_string[64];

/*
 * ThreadProbeNoop - no-op routine for the CreateThread probe. Returns
 * immediately; its only purpose is to confirm or deny thread support.
 */
static DWORD WINAPI ThreadProbeNoop(LPVOID lpParam)
{
    (void)lpParam;
    return 0;
}

/*
 * ProbeThreads - Attempt CreateThread with a no-op routine. On Win32s
 * 1.25a CreateThread fails (threads unsupported). Returns 1 if threads
 * work, 0 otherwise. Closes the handle on success.
 */
static int ProbeThreads(void)
{
    HANDLE hThread;
    DWORD threadId;

    hThread = CreateThread(NULL, 0, ThreadProbeNoop, NULL, 0, &threadId);
    if (hThread == NULL) {
        return 0;
    }
    CloseHandle(hThread);
    return 1;
}

void FeatInit(void)
{
    DWORD ver;
    HMODULE hKernel;
    FARPROC proc;

    /* Zero the whole struct: every flag and pointer starts clear. */
    memset(&g_features, 0, sizeof(g_features));

    /* OS version decode (PHASE4 sequence). */
    ver = GetVersion();
    g_features.is_nt = (ver & 0x80000000UL) ? 0 : 1;
    g_features.win_major = (int)(ver & 0xFF);
    g_features.win_minor = (int)((ver >> 8) & 0xFF);
    g_features.win_build = g_features.is_nt ? (int)((ver >> 16) & 0xFFFF) : 0;

    if (!g_features.is_nt && g_features.win_major == 4) {
        g_features.is_win9x = 1;
    }
    if (!g_features.is_nt && g_features.win_major == 3) {
        g_features.is_win32s = 1;       /* presumptive */
    }

    /* Thread probe: confirm/deny threads regardless of version DWORD. */
    g_features.has_threads = ProbeThreads();
    if (!g_features.has_threads) {
        /* No threads -> Win32s (or a system with threads disabled). */
        g_features.is_win32s = 1;
    }

    /*
     * Delay-loaded API resolution. Each name appears only as a string
     * literal here - never as a link-time import.
     */
    hKernel = GetModuleHandleA("kernel32");
    if (hKernel != NULL) {
        proc = GetProcAddress(hKernel, "CreateJobObjectA");
        if (proc != NULL) {
            g_features.pCreateJobObjectA =
                (HANDLE (WINAPI *)(LPSECURITY_ATTRIBUTES, LPCSTR))proc;
        }
        proc = GetProcAddress(hKernel, "AssignProcessToJobObject");
        if (proc != NULL) {
            g_features.pAssignProcessToJobObject =
                (BOOL (WINAPI *)(HANDLE, HANDLE))proc;
        }
        proc = GetProcAddress(hKernel, "SetInformationJobObject");
        if (proc != NULL) {
            g_features.pSetInformationJobObject =
                (BOOL (WINAPI *)(HANDLE, int, LPVOID, DWORD))proc;
        }
        /* Job objects require all three of the above. */
        if (g_features.pCreateJobObjectA != NULL &&
            g_features.pAssignProcessToJobObject != NULL &&
            g_features.pSetInformationJobObject != NULL) {
            g_features.has_create_job_object = 1;
        }

        proc = GetProcAddress(hKernel, "GetBinaryTypeA");
        if (proc != NULL) {
            g_features.pGetBinaryTypeA =
                (BOOL (WINAPI *)(LPCSTR, LPDWORD))proc;
            g_features.has_get_binary_type = 1;
        }

        /* Absent on the NT 3.1 floor; callers fall back to DuplicateHandle. */
        proc = GetProcAddress(hKernel, "SetHandleInformation");
        if (proc != NULL) {
            g_features.pSetHandleInformation =
                (BOOL (WINAPI *)(HANDLE, DWORD, DWORD))proc;
        }

        proc = GetProcAddress(hKernel, "IsWow64Process");
        if (proc != NULL) {
            g_features.pIsWow64Process =
                (BOOL (WINAPI *)(HANDLE, BOOL *))proc;
            g_features.has_is_wow64_process = 1;
        }

        proc = GetProcAddress(hKernel, "GenerateConsoleCtrlEvent");
        if (proc != NULL) {
            g_features.pGenerateConsoleCtrlEvent =
                (BOOL (WINAPI *)(DWORD, DWORD))proc;
            g_features.has_generate_ctrl_event = 1;
        }

        proc = GetProcAddress(hKernel, "QueryFullProcessImageNameA");
        if (proc != NULL) {
            g_features.pQueryFullProcessImageNameA =
                (BOOL (WINAPI *)(HANDLE, DWORD, LPSTR, LPDWORD))proc;
            g_features.has_query_full_image_name = 1;
        }

        proc = GetProcAddress(hKernel, "CreatePseudoConsole");
        if (proc != NULL) {
            g_features.pCreatePseudoConsole =
                (LONG (WINAPI *)(COORD, HANDLE, HANDLE, DWORD, void **))proc;
        }
        proc = GetProcAddress(hKernel, "ClosePseudoConsole");
        if (proc != NULL) {
            g_features.pClosePseudoConsole =
                (void (WINAPI *)(void *))proc;
        }
        proc = GetProcAddress(hKernel, "ResizePseudoConsole");
        if (proc != NULL) {
            g_features.pResizePseudoConsole =
                (LONG (WINAPI *)(void *, COORD))proc;
        }
        if (g_features.pCreatePseudoConsole != NULL &&
            g_features.pClosePseudoConsole != NULL &&
            g_features.pResizePseudoConsole != NULL) {
            g_features.has_create_pseudo_console = 1;
        }

        proc = GetProcAddress(hKernel, "InitializeProcThreadAttributeList");
        if (proc != NULL) {
            g_features.pInitializeProcThreadAttributeList =
                (BOOL (WINAPI *)(LPVOID, DWORD, DWORD, FeatSizeT *))proc;
        }
        proc = GetProcAddress(hKernel, "UpdateProcThreadAttribute");
        if (proc != NULL) {
            g_features.pUpdateProcThreadAttribute =
                (BOOL (WINAPI *)(LPVOID, DWORD, DWORD, PVOID, FeatSizeT,
                                 PVOID, FeatSizeT *))proc;
        }
        proc = GetProcAddress(hKernel, "DeleteProcThreadAttributeList");
        if (proc != NULL) {
            g_features.pDeleteProcThreadAttributeList =
                (void (WINAPI *)(LPVOID))proc;
        }
        if (g_features.pInitializeProcThreadAttributeList != NULL &&
            g_features.pUpdateProcThreadAttribute != NULL &&
            g_features.pDeleteProcThreadAttributeList != NULL) {
            g_features.has_proc_thread_attr_list = 1;
        }

        proc = GetProcAddress(hKernel, "SetProcessMitigationPolicy");
        if (proc != NULL) {
            g_features.pSetProcessMitigationPolicy =
                (BOOL (WINAPI *)(int, PVOID, FeatSizeT))proc;
            g_features.has_set_process_mitigation = 1;
        }

        /*
         * 5.4: the delay-loaded -W (UTF-16) file/dir/spawn set - the `wide`
         * encoding tier. Resolved only here as string literals, never linked
         * by name, so the import resolver still loads the binary on Win32s.
         */
        proc = GetProcAddress(hKernel, "CreateFileW");
        if (proc != NULL) {
            g_features.pCreateFileW =
                (HANDLE (WINAPI *)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                   DWORD, DWORD, HANDLE))proc;
        }
        proc = GetProcAddress(hKernel, "FindFirstFileW");
        if (proc != NULL) {
            g_features.pFindFirstFileW =
                (HANDLE (WINAPI *)(LPCWSTR, void *))proc;
        }
        proc = GetProcAddress(hKernel, "FindNextFileW");
        if (proc != NULL) {
            g_features.pFindNextFileW =
                (BOOL (WINAPI *)(HANDLE, void *))proc;
        }
        proc = GetProcAddress(hKernel, "DeleteFileW");
        if (proc != NULL) {
            g_features.pDeleteFileW =
                (BOOL (WINAPI *)(LPCWSTR))proc;
        }
        proc = GetProcAddress(hKernel, "CopyFileW");
        if (proc != NULL) {
            g_features.pCopyFileW =
                (BOOL (WINAPI *)(LPCWSTR, LPCWSTR, BOOL))proc;
        }
        proc = GetProcAddress(hKernel, "MoveFileW");
        if (proc != NULL) {
            g_features.pMoveFileW =
                (BOOL (WINAPI *)(LPCWSTR, LPCWSTR))proc;
        }
        proc = GetProcAddress(hKernel, "CreateDirectoryW");
        if (proc != NULL) {
            g_features.pCreateDirectoryW =
                (BOOL (WINAPI *)(LPCWSTR, LPSECURITY_ATTRIBUTES))proc;
        }
        proc = GetProcAddress(hKernel, "RemoveDirectoryW");
        if (proc != NULL) {
            g_features.pRemoveDirectoryW =
                (BOOL (WINAPI *)(LPCWSTR))proc;
        }
        /* All eight file/dir -W APIs exist since NT 3.1; one flag covers them. */
        if (g_features.pCreateFileW != NULL &&
            g_features.pFindFirstFileW != NULL &&
            g_features.pFindNextFileW != NULL &&
            g_features.pDeleteFileW != NULL &&
            g_features.pCopyFileW != NULL &&
            g_features.pMoveFileW != NULL &&
            g_features.pCreateDirectoryW != NULL &&
            g_features.pRemoveDirectoryW != NULL) {
            g_features.has_wide_fileapi = 1;
        }

        /* CreateProcessW is tracked separately - the spawn path. */
        proc = GetProcAddress(hKernel, "CreateProcessW");
        if (proc != NULL) {
            g_features.pCreateProcessW =
                (BOOL (WINAPI *)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
                                 LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
                                 LPCWSTR, void *, LPPROCESS_INFORMATION))proc;
            g_features.has_wide_createprocess = 1;
        }
    }

    /* WoW64 self-check when IsWow64Process is present (XP SP2+). */
    if (g_features.has_is_wow64_process) {
        BOOL wow64 = FALSE;
        if (g_features.pIsWow64Process(GetCurrentProcess(), &wow64)) {
            g_features.is_wow64 = wow64 ? 1 : 0;
        }
    }
}

const char *FeatVersionString(void)
{
    const char *kind;

    if (g_features.is_win32s) {
        kind = "Win32s";
    } else if (g_features.is_win9x) {
        kind = "9x";
    } else if (g_features.is_nt) {
        kind = "NT";
    } else {
        kind = "Win16";
    }

    /* Integer-only formatting via wsprintfA - no printf %f. */
    if (g_features.is_nt) {
        wsprintfA(g_version_string, "Windows %d.%d.%d (%s)",
                  g_features.win_major, g_features.win_minor,
                  g_features.win_build, kind);
    } else {
        wsprintfA(g_version_string, "Windows %d.%d (%s)",
                  g_features.win_major, g_features.win_minor, kind);
    }

    return g_version_string;
}

int FeatForceFallback(int flagsMask)
{
    if (flagsMask & FEAT_FORCE_NO_THREADS) {
        g_features.has_threads = 0;
    }
    if (flagsMask & FEAT_FORCE_NO_JOB_OBJECTS) {
        g_features.has_create_job_object = 0;
        g_features.pCreateJobObjectA = NULL;
        g_features.pAssignProcessToJobObject = NULL;
        g_features.pSetInformationJobObject = NULL;
    }
    if (flagsMask & FEAT_FORCE_NO_CTRL_EVENTS) {
        g_features.has_generate_ctrl_event = 0;
        g_features.pGenerateConsoleCtrlEvent = NULL;
    }
    if (flagsMask & FEAT_FORCE_NO_PTY) {
        g_features.has_create_pseudo_console = 0;
        g_features.pCreatePseudoConsole = NULL;
        g_features.pClosePseudoConsole = NULL;
        g_features.pResizePseudoConsole = NULL;
    }
    if (flagsMask & FEAT_FORCE_NO_BINARY_TYPE) {
        g_features.has_get_binary_type = 0;
        g_features.pGetBinaryTypeA = NULL;
    }
    if (flagsMask & FEAT_FORCE_NO_WIDE_FILEAPI) {
        g_features.has_wide_fileapi = 0;
        g_features.pCreateFileW = NULL;
        g_features.pFindFirstFileW = NULL;
        g_features.pFindNextFileW = NULL;
        g_features.pDeleteFileW = NULL;
        g_features.pCopyFileW = NULL;
        g_features.pMoveFileW = NULL;
        g_features.pCreateDirectoryW = NULL;
        g_features.pRemoveDirectoryW = NULL;
    }
    if (flagsMask & FEAT_FORCE_NO_WIDE_CREATEPROCESS) {
        g_features.has_wide_createprocess = 0;
        g_features.pCreateProcessW = NULL;
    }

    return flagsMask;
}
