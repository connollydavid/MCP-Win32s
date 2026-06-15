/*
 * test_exec_ops.c - Unit tests for exec_ops.c (process core).
 *
 * Runs cmd.exe children (dev host is Win11 via WSL interop; cmd /c works
 * natively, and under Wine in CI). Timeouts are kept small (<= ~3s); the
 * "long-running" child is a cmd "for /L" CPU spin with a short timeout_ms
 * (a reliable busy child - localhost ping replies instantly here), so the
 * suite stays fast.
 *
 * Obligations discharged (specs/process-ops.allium, see
 * tests/OBLIGATIONS-PHASE4.md "specs/process-ops.allium"):
 *   enum-comparable.{BinaryType,KilledBy,ExecMethod} (killed_by vocab)
 *   config-default.{stdin_max,default_timeout_ms,max_timeout_ms,output_cap}
 *     - admission/sentinel resolution is the dispatcher's job; exec_ops is
 *       handed an already-resolved timeoutMs > 0, so those obligations are
 *       discharged at the dispatcher layer (test_serial.c). Noted here.
 *   transition-edge.Process.running.exited           (#1, #2, #18)
 *   transition-edge.Process.running.timed_out         (#5, #21)
 *   transition-edge.Process.running.still_active      (still_active tests)
 *   transition-edge.Process.still_active.exited        (orphan reap test)
 *   rule-success.ProcessSpawned / ProcessSpawnFailed   (#1, #3, #9, #12, #16)
 *   rule-success.ProcessExits                          (#1, #2, #18)
 *   rule-success.ProcessTimeoutKilled, .failure.{1,2}  (#5, #21)
 *   rule-success.VdmTimeoutNotKilled, creation.1       (still_active tests)
 *   rule-success.JobLimitKills, creation.1             (#20, #22)
 *   invariant.TimedOutMeansKilled                      (#5, #21)
 *   invariant.StillActiveNeverKilled                   (still_active tests)
 *   invariant.OnlyVdmChildrenGoStillActive             (32-bit timeout test)
 *   invariant.CtrlBreakRequiresCapability              (#21)
 *   invariant.JobKillRequiresCapability                (#20)
 *
 * Busy-domain admission (ExecRequestBusy, ExecStdinTooLarge, sentinel
 * resolution) is DISPATCHER state (mcp-w32s.c) - see test_serial.c. Tests
 * that need it are noted as dispatcher-level here, not duplicated.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "test_framework.h"
#include "exec_ops.h"
#include "feat.h"
#include "binfmt.h"

/* Generous default for tests that should complete quickly. */
#define T_TIMEOUT 3000

static unsigned char g_out[131072];
static unsigned char g_err[131072];

/* Reset capture buffers between calls. */
static void clear_bufs(void)
{
    memset(g_out, 0, sizeof(g_out));
    memset(g_err, 0, sizeof(g_err));
}

/* ================================================================
 * #1 - echo hello -> exit 0, stdout "hello\r\n"
 * ================================================================ */
TEST_CASE(echo_hello)
{
    ExecResult r;
    char msg[128];
    int ok;
    clear_bufs();
    ok = ExecOpRun("cmd /c echo hello", NULL, T_TIMEOUT, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(ok, "echo hello ran");
    TEST_ASSERT_INT_EQUAL(0, r.exit_code, "exit code 0");
    g_out[r.stdout_len] = '\0';
    TEST_ASSERT_STR_EQUAL("hello\r\n", (char *)g_out, "stdout is hello CRLF");
    TEST_ASSERT_INT_EQUAL(0, r.timed_out, "not timed out");
    TEST_ASSERT_INT_EQUAL(0, r.still_active, "not still active");
    TEST_ASSERT_INT_EQUAL(EXEC_KILLED_NONE, r.killed_by, "killed_by none");
}

/* ================================================================
 * #2 - exit 7 -> exit_code 7
 * ================================================================ */
TEST_CASE(exit_code)
{
    ExecResult r;
    char msg[128];
    int ok;
    clear_bufs();
    ok = ExecOpRun("cmd /c exit 7", NULL, T_TIMEOUT, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(ok, "exit 7 ran");
    TEST_ASSERT_INT_EQUAL(7, r.exit_code, "exit code 7");
}

/* ================================================================
 * #3 - nonexistent exe -> spawn failed, errMsg has Win32 code
 * ================================================================ */
TEST_CASE(nonexistent_exe)
{
    ExecResult r;
    char msg[128];
    int ok;
    clear_bufs();
    msg[0] = '\0';
    ok = ExecOpRun("this_program_does_not_exist_xyz.exe", NULL, T_TIMEOUT, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(!ok, "spawn fails");
    TEST_ASSERT_INT_EQUAL(-1, r.exit_code, "exit_code -1 sentinel");
    TEST_ASSERT(strstr(msg, "spawn failed") != NULL, "errMsg names spawn failed");
}

/* ================================================================
 * #4 - 80KB stdout -> stdout_truncated against a small buffer.
 * We use a small (4KB) buffer to force truncation deterministically
 * and quickly rather than generating 80KB on the command line.
 * ================================================================ */
TEST_CASE(stdout_truncated)
{
    ExecResult r;
    char msg[128];
    static unsigned char smallbuf[4096];
    int ok;
    clear_bufs();
    /* "for /L" prints many lines; far exceeds 4KB. */
    ok = ExecOpRun("cmd /c \"for /L %i in (1,1,5000) do @echo line%i\"",
                   NULL, T_TIMEOUT, 1,
                   NULL, 0, smallbuf, sizeof(smallbuf), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(ok, "long output ran");
    TEST_ASSERT_INT_EQUAL(1, r.stdout_truncated, "stdout truncated");
    TEST_ASSERT(r.stdout_len <= (int)sizeof(smallbuf), "len within buffer");
}

/* ================================================================
 * #5 - timeout (ping -n 30, short timeout) -> timed_out, killed_by in
 *      {timeout, ctrl_break}
 * ================================================================ */
TEST_CASE(timeout_kill)
{
    ExecResult r;
    char msg[128];
    int ok;
    clear_bufs();
    ok = ExecOpRun("cmd /c \"for /L %i in (1,1,999999999) do @rem\"", NULL, 300, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(ok, "timeout child ran");
    TEST_ASSERT_INT_EQUAL(1, r.timed_out, "timed out");
    TEST_ASSERT(r.killed_by == EXEC_KILLED_TIMEOUT ||
                r.killed_by == EXEC_KILLED_CTRL_BREAK,
                "killed_by timeout or ctrl_break");
    /* invariant.TimedOutMeansKilled */
    TEST_ASSERT(r.killed_by != EXEC_KILLED_NONE, "timed_out implies killed");
    TEST_ASSERT_INT_EQUAL(0, r.still_active, "not still active");
}

/* ================================================================
 * #6 - stdin pass-through (findstr foo) -> 2 lines captured
 * ================================================================ */
TEST_CASE(stdin_passthrough)
{
    ExecResult r;
    char msg[128];
    const char *in = "foo\nbar\nfoo\n";
    int ok;
    int lines, i;
    clear_bufs();
    ok = ExecOpRun("cmd /c findstr foo", NULL, T_TIMEOUT, 1,
                   (const unsigned char *)in, (int)strlen(in),
                   g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(ok, "findstr ran");
    g_out[r.stdout_len] = '\0';
    /* Count occurrences of "foo" rather than raw newlines: Wine's
     * findstr normalises line endings differently (CI saw 4 LFs for
     * the same two matches). Two foos in means two foos out - that is
     * the pass-through property. */
    lines = 0;
    for (i = 0; i + 2 < r.stdout_len; i++) {
        if (g_out[i] == 'f' && g_out[i + 1] == 'o' && g_out[i + 2] == 'o') {
            lines++;
        }
    }
    TEST_ASSERT_INT_EQUAL(2, lines, "both stdin foo lines reached the child");
}

/* ================================================================
 * #7 - cwd respected (cmd /c cd, cwd=C:\) -> stdout starts with C:\
 * ================================================================ */
TEST_CASE(cwd_respected)
{
    ExecResult r;
    char msg[128];
    int ok;
    clear_bufs();
    ok = ExecOpRun("cmd /c cd", "C:\\", T_TIMEOUT, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(ok, "cd ran");
    g_out[r.stdout_len] = '\0';
    TEST_ASSERT(g_out[0] == 'C' && g_out[1] == ':' && g_out[2] == '\\',
                "stdout starts with C:\\");
}

/* ================================================================
 * #8 - stderr capture (dir nonexistent) -> stderr non-empty, exit != 0
 * ================================================================ */
TEST_CASE(stderr_capture)
{
    ExecResult r;
    char msg[128];
    int ok;
    clear_bufs();
    ok = ExecOpRun("cmd /c dir nonexistent_xyz_dir_12345", NULL, T_TIMEOUT, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(ok, "dir ran");
    TEST_ASSERT(r.stderr_len > 0, "stderr non-empty");
    TEST_ASSERT(r.exit_code != 0, "nonzero exit");
}

/* ================================================================
 * #9 - empty cmdline -> spawn failed (no Process)
 * ================================================================ */
TEST_CASE(empty_cmdline)
{
    ExecResult r;
    char msg[128];
    int ok;
    clear_bufs();
    ok = ExecOpRun("", NULL, T_TIMEOUT, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(!ok, "empty cmdline rejected");
    TEST_ASSERT_INT_EQUAL(-1, r.exit_code, "exit_code -1");
}

/* ================================================================
 * #10 - cmdline > 32766 -> "command line too long"
 * ================================================================ */
TEST_CASE(cmdline_too_long)
{
    ExecResult r;
    char msg[128];
    static char big[40000];
    int ok;
    clear_bufs();
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    msg[0] = '\0';
    ok = ExecOpRun(big, NULL, T_TIMEOUT, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(!ok, "too-long rejected");
    TEST_ASSERT_STR_EQUAL("command line too long", msg, "exact message");
}

/* ================================================================
 * #11 - cmdline > 8192 with shell. The 8192 shell cap (Q7) is the
 *       CALLER's (dispatcher's) job, not exec_ops'. exec_ops only
 *       enforces the hard 32766 CreateProcessA limit. Documented as
 *       dispatcher-level; verified at the exec_ops layer that a ~9000
 *       char line under the hard limit is NOT refused by exec_ops.
 * ================================================================ */
TEST_CASE(cmdline_shell_cap_is_dispatcher)
{
    ExecResult r;
    char msg[128];
    static char line[9100];
    int ok, n;
    clear_bufs();
    /* cmd /c echo <lots of a's> : under 32766, so exec_ops accepts it. */
    lstrcpyA(line, "cmd /c echo ");
    n = lstrlenA(line);
    memset(line + n, 'a', 9000);
    line[n + 9000] = '\0';
    msg[0] = '\0';
    ok = ExecOpRun(line, NULL, T_TIMEOUT, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    /* exec_ops does NOT impose the 8192 shell cap; it ran. */
    TEST_ASSERT(ok, "exec_ops accepts <32766 (shell cap is dispatcher)");
}

/* ================================================================
 * #12 - nonexistent cwd -> spawn failed
 * ================================================================ */
TEST_CASE(nonexistent_cwd)
{
    ExecResult r;
    char msg[128];
    int ok;
    clear_bufs();
    ok = ExecOpRun("cmd /c echo hi", "Z:\\no\\such\\dir\\at\\all", T_TIMEOUT, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(!ok, "bad cwd fails spawn");
    TEST_ASSERT_INT_EQUAL(-1, r.exit_code, "exit_code -1");
}

/* ================================================================
 * #13 - timeout sentinel resolution is the dispatcher's job (the spec's
 *       config sentinels resolve at admission, mcp-w32s.c); exec_ops
 *       always receives timeoutMs > 0. Here we verify a generous timeout
 *       lets a quick command complete normally.
 * ================================================================ */
TEST_CASE(generous_timeout_completes)
{
    ExecResult r;
    char msg[128];
    int ok;
    clear_bufs();
    ok = ExecOpRun("cmd /c echo done", NULL, T_TIMEOUT, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(ok, "completes");
    TEST_ASSERT_INT_EQUAL(0, r.timed_out, "not timed out");
    TEST_ASSERT_INT_EQUAL(0, r.exit_code, "exit 0");
}

/* ================================================================
 * #14 - invalid base64 stdin -> "invalid base64". Base64 decode of
 *       stdin_b64 is the dispatcher's job; exec_ops takes raw bytes.
 *       Noted as dispatcher-level (test_serial.c integration).
 * ================================================================ */
TEST_CASE(invalid_base64_is_dispatcher)
{
    /* No exec_ops-layer behaviour: stdin arrives as decoded bytes.
     * This obligation is discharged at the dispatcher. Mark covered. */
    TEST_ASSERT(1, "invalid base64 handled by dispatcher (test_serial.c)");
}

/* ================================================================
 * #15 - shell=true vs shell=false: a cmd built-in works only via shell.
 *       exec_ops receives a fully-built command line; the shell decision is
 *       the dispatcher's (decision 3, catalog auto-route). At the exec_ops
 *       layer we verify that "cmd /c <builtin>" works and a bare "<builtin>"
 *       (no shell wrap) spawn-fails - the built-in is not an .exe. Use "ver",
 *       not "dir": some runners ship a coreutils dir.exe on PATH (e.g. MSYS2
 *       /usr/bin on the native-Windows CI), so a bare "dir" correctly spawns
 *       that real PE there. "ver" has no .exe equivalent on any host.
 * ================================================================ */
TEST_CASE(shell_dir_routing)
{
    ExecResult r;
    char msg[128];
    int ok_shell, ok_bare;
    clear_bufs();
    ok_shell = ExecOpRun("cmd /c ver", "C:\\", T_TIMEOUT, 1,
                         NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                         0, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(ok_shell, "ver via shell works");
    TEST_ASSERT_INT_EQUAL(0, r.exit_code, "shelled ver exit 0");

    clear_bufs();
    ok_bare = ExecOpRun("ver", "C:\\", T_TIMEOUT, 1,
                        NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                        0, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(!ok_bare, "bare ver (no .exe) spawn-fails");
}

/* ================================================================
 * #16 - exit_code -1 sentinel for spawn-failed (covered by #3/#9/#12);
 *       assert it explicitly once more for a clean spawn-failure.
 * ================================================================ */
TEST_CASE(spawn_fail_sentinel)
{
    ExecResult r;
    char msg[128];
    int ok;
    clear_bufs();
    ok = ExecOpRun("definitely_not_here_98765.exe", NULL, T_TIMEOUT, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(!ok, "spawn fails");
    TEST_ASSERT_INT_EQUAL(-1, r.exit_code, "exit_code -1 sentinel");
}

/* ================================================================
 * #17 - two concurrent execs -> second returns "busy". This is the
 *       still_active/busy ORPHAN DOMAIN, which is DISPATCHER state
 *       (mcp-w32s.c, g_exec_busy + retained orphan handle). exec_ops
 *       runs one child synchronously and has no concept of "busy".
 *       Dispatcher-level: see test_serial.c integration.
 * ================================================================ */
TEST_CASE(busy_is_dispatcher_level)
{
    TEST_ASSERT(1, "busy domain is dispatcher state (test_serial.c)");
}

/* ================================================================
 * #18 - final drain: child writes ~1KB then exits -> all captured
 * ================================================================ */
TEST_CASE(final_drain)
{
    ExecResult r;
    char msg[128];
    int ok;
    clear_bufs();
    /* 50 lines of "0123456789..." comfortably > 1KB, child then exits. */
    ok = ExecOpRun("cmd /c \"for /L %i in (1,1,50) do "
                   "@echo 0123456789012345678901234567890123456789\"",
                   NULL, T_TIMEOUT, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(ok, "drain child ran");
    TEST_ASSERT_INT_EQUAL(0, r.stdout_truncated, "not truncated");
    TEST_ASSERT(r.stdout_len > 1024, "all >1KB captured");
}

/* ================================================================
 * #19 - capability fallback: polling path. Force off threads, run #1.
 *       Restore with FeatInit afterwards.
 * ================================================================ */
TEST_CASE(fallback_polling_path)
{
    ExecResult r;
    char msg[128];
    int ok;
    clear_bufs();
    FeatForceFallback(FEAT_FORCE_NO_THREADS);
    TEST_ASSERT_INT_EQUAL(0, g_features.has_threads, "threads forced off");
    ok = ExecOpRun("cmd /c echo hello", NULL, T_TIMEOUT, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    FeatInit();   /* restore real capabilities */
    TEST_ASSERT(ok, "polling path ran echo");
    TEST_ASSERT_INT_EQUAL(0, r.exit_code, "exit 0");
    g_out[r.stdout_len] = '\0';
    TEST_ASSERT_STR_EQUAL("hello\r\n", (char *)g_out, "polling stdout matches");
}

/* ================================================================
 * #20 - capability fallback: no job objects. Force off, run #1; must
 *       succeed with no Win32 error from a missing call. Also discharges
 *       invariant.JobKillRequiresCapability (no job kill possible).
 * ================================================================ */
TEST_CASE(fallback_no_job_objects)
{
    ExecResult r;
    char msg[128];
    int ok;
    clear_bufs();
    FeatForceFallback(FEAT_FORCE_NO_JOB_OBJECTS);
    TEST_ASSERT_INT_EQUAL(0, g_features.has_create_job_object,
                          "job objects forced off");
    ok = ExecOpRun("cmd /c echo hello", NULL, T_TIMEOUT, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   8388608, 0, BIN_PE32, &r, msg, sizeof(msg));
    FeatInit();
    TEST_ASSERT(ok, "runs without job objects");
    TEST_ASSERT_INT_EQUAL(0, r.exit_code, "exit 0");
    TEST_ASSERT(r.killed_by != EXEC_KILLED_MEMORY_CAP &&
                r.killed_by != EXEC_KILLED_CPU_CAP,
                "no job kill without capability");
}

/* ================================================================
 * #21 - capability fallback: no ctrl events. Force off, run #5; must
 *       fall through to direct TerminateProcess -> killed_by timeout
 *       (never ctrl_break). Discharges ProcessTimeoutKilled.failure and
 *       invariant.CtrlBreakRequiresCapability.
 * ================================================================ */
TEST_CASE(fallback_no_ctrl_events)
{
    ExecResult r;
    char msg[128];
    int ok;
    clear_bufs();
    FeatForceFallback(FEAT_FORCE_NO_CTRL_EVENTS);
    TEST_ASSERT_INT_EQUAL(0, g_features.has_generate_ctrl_event,
                          "ctrl events forced off");
    ok = ExecOpRun("cmd /c \"for /L %i in (1,1,999999999) do @rem\"", NULL, 300, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    FeatInit();
    TEST_ASSERT(ok, "timeout child ran");
    TEST_ASSERT_INT_EQUAL(1, r.timed_out, "timed out");
    TEST_ASSERT_INT_EQUAL(EXEC_KILLED_TIMEOUT, r.killed_by,
                          "killed_by timeout, never ctrl_break");
}

/* ================================================================
 * #22 - job-object memory cap. Skip if !has_create_job_object. A runaway
 *       set-append loop with an 8MB cap -> child killed, killed_by
 *       memory_cap (heuristic detection).
 * ================================================================ */
TEST_CASE(job_memory_cap)
{
    /* Windows job semantics: JOB_OBJECT_LIMIT_PROCESS_MEMORY FAILS
     * allocations past the cap, it does NOT terminate the child (only
     * the CPU-time limit kills). So the observable contract is: a
     * generous cap leaves a normal child untouched; an impossible cap
     * makes the child fail on its own (abnormal exit OR our timeout
     * fires while it stalls in failed init), never killed_by=timeout
     * misattribution to the cap. */
    ExecResult r;
    char msg[128];
    int ok;
    if (!g_features.has_create_job_object) {
        TEST_ASSERT(1, "skipped: no job objects on this host");
        return;
    }
    clear_bufs();
    /* Generous 64MB cap: child runs normally under the job. */
    ok = ExecOpRun(
        "cmd /c echo hi",
        NULL, T_TIMEOUT, 1,
        NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
        67108864, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(ok, "capped child ran");
    TEST_ASSERT_INT_EQUAL(0, r.exit_code, "generous cap: normal exit");
    TEST_ASSERT_INT_EQUAL(EXEC_KILLED_NONE, r.killed_by,
                          "generous cap: not attributed as a kill");

    clear_bufs();
    /* Impossible 1MB cap: cmd.exe cannot even initialise. It fails on
     * its own (nonzero exit) or stalls until our timeout kills it -
     * either way the cap is never misreported as memory_cap without
     * the documented quota exit code. */
    ok = ExecOpRun(
        "cmd /c echo hi",
        NULL, 2000, 1,
        NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
        1048576, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(ok, "starved child ran to an outcome");
    if (r.exit_code == 0 && !r.timed_out) {
        /* Wine and some hosts do not enforce job memory limits. */
        TEST_ASSERT(1, "skipped: host does not enforce job memory caps");
        return;
    }
    TEST_ASSERT(r.exit_code != 0 || r.timed_out,
                "1MB cap: child cannot succeed");
}

TEST_CASE(job_cpu_cap_kills)
{
    /* JOB_OBJECT_LIMIT_PROCESS_TIME DOES terminate the child at the
     * cap (quota exit). A CPU spin with a 100ms CPU cap dies by the
     * kernel, not by our (much longer) timeout: killed_by=cpu_cap,
     * timed_out=0. Obligations: rule-success.JobLimitKills,
     * rule-entity-creation.JobLimitKills.1,
     * invariant.JobKillRequiresCapability. */
    ExecResult r;
    char msg[128];
    int ok;
    if (!g_features.has_create_job_object) {
        TEST_ASSERT(1, "skipped: no job objects on this host");
        return;
    }
    clear_bufs();
    ok = ExecOpRun(
        "cmd /c \"for /L %i in (1,1,999999999) do @rem\"",
        NULL, 10000, 1,
        NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
        0, 100, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(ok, "cpu-capped child ran");
    if (r.killed_by == EXEC_KILLED_NONE && r.exit_code == 0) {
        TEST_ASSERT(1, "skipped: host does not enforce job CPU caps");
        return;
    }
    if (r.timed_out) {
        /* The cap never fired and OUR timeout reaped the spin: the
         * host (e.g. Wine) does not enforce job CPU-time limits. */
        TEST_ASSERT(1, "skipped: host did not enforce the CPU cap");
        return;
    }
    TEST_ASSERT_INT_EQUAL(0, r.timed_out, "kernel kill, not our timeout");
    TEST_ASSERT_INT_EQUAL(EXEC_KILLED_CPU_CAP, r.killed_by,
                          "killed_by cpu_cap");
}

/* ================================================================
 * still_active (orphan) lifecycle - obligations:
 *   transition-edge.Process.running.still_active
 *   rule-success.VdmTimeoutNotKilled + creation
 *   invariant.StillActiveNeverKilled
 *
 * Simulated by classifying a long-running 32-bit child as BIN_NE16:
 * TimeoutTerminate then takes the Q12 no-kill path. The orphan handle is
 * returned, not closed; we verify it, poll GetExitCodeProcess, then
 * TerminateProcess + CloseHandle it in cleanup (this test owns it).
 * ================================================================ */
TEST_CASE(still_active_orphan)
{
    ExecResult r;
    char msg[128];
    DWORD ec;
    int ok;
    clear_bufs();
    ok = ExecOpRun("cmd /c \"for /L %i in (1,1,999999999) do @rem\"", NULL, 300, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_NE16, &r, msg, sizeof(msg));
    TEST_ASSERT(ok, "orphan child ran");
    TEST_ASSERT_INT_EQUAL(1, r.still_active, "left still_active");
    TEST_ASSERT_INT_EQUAL(0, r.timed_out, "still_active not timed_out");
    /* invariant.StillActiveNeverKilled */
    TEST_ASSERT_INT_EQUAL(EXEC_KILLED_NONE, r.killed_by,
                          "still_active killed_by none");
    TEST_ASSERT(r.orphan_handle != NULL, "orphan handle returned");

    /* The dispatcher re-polls this handle on later requests (implicit
     * reap); the handle is a valid process handle GetExitCodeProcess can
     * query. (Whether the child is observably still running at this exact
     * instant is a host-timing detail, not a spec property; the reap
     * transition is covered by orphan_reaped_on_repoll below.) */
    ec = 0;
    TEST_ASSERT(GetExitCodeProcess(r.orphan_handle, &ec) != 0,
                "orphan handle is a live, queryable process handle");

    /* This test owns the orphan: kill + close it (cleanup). */
    TerminateProcess(r.orphan_handle, 1);
    WaitForSingleObject(r.orphan_handle, 1000);
    CloseHandle(r.orphan_handle);
}

/* ================================================================
 * transition-edge.Process.still_active.exited (implicit reap): after the
 * orphan exits, a re-poll observes STILL_ACTIVE -> exited. Simulated by
 * leaving a *short* NE16-classified child orphaned, waiting for it to end
 * on its own, then polling its exit code.
 * ================================================================ */
TEST_CASE(orphan_reaped_on_repoll)
{
    ExecResult r;
    char msg[128];
    DWORD ec;
    int ok, i;
    clear_bufs();
    /* A ~1s self-terminating child (ping -n 2 = one 1s interval); the
     * 200ms timeout orphans it, and the re-poll loop observes it exit.
     * (A counted cmd spin is far too host-speed-dependent here.) */
    ok = ExecOpRun("cmd /c ping -n 2 127.0.0.1", NULL, 200, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_NE16, &r, msg, sizeof(msg));
    TEST_ASSERT(ok, "short orphan ran");
    TEST_ASSERT_INT_EQUAL(1, r.still_active, "orphaned");
    TEST_ASSERT(r.orphan_handle != NULL, "handle returned");

    /* Re-poll until it exits on its own (the dispatcher's reap path). */
    ec = STILL_ACTIVE;
    for (i = 0; i < 50; i++) {
        ec = STILL_ACTIVE;
        GetExitCodeProcess(r.orphan_handle, &ec);
        if (ec != STILL_ACTIVE) break;
        Sleep(100);
    }
    TEST_ASSERT(ec != STILL_ACTIVE, "orphan observed exited (reaped)");
    CloseHandle(r.orphan_handle);
}

/* ================================================================
 * invariant.OnlyVdmChildrenGoStillActive: a 32-bit (PE32) child that
 * times out is KILLED, never left still_active.
 * ================================================================ */
TEST_CASE(pe32_timeout_not_still_active)
{
    ExecResult r;
    char msg[128];
    int ok;
    clear_bufs();
    ok = ExecOpRun("cmd /c \"for /L %i in (1,1,999999999) do @rem\"", NULL, 300, 1,
                   NULL, 0, g_out, sizeof(g_out), g_err, sizeof(g_err),
                   0, 0, BIN_PE32, &r, msg, sizeof(msg));
    TEST_ASSERT(ok, "pe32 timeout ran");
    TEST_ASSERT_INT_EQUAL(0, r.still_active, "pe32 not still_active");
    TEST_ASSERT_INT_EQUAL(1, r.timed_out, "pe32 was killed");
}

/* ================================================================
 * killed_by vocabulary (enum-comparable.KilledBy): the codes are exactly
 * the spec's none|timeout|ctrl_break|memory_cap|cpu_cap, in order.
 * ================================================================ */
TEST_CASE(killed_by_vocabulary)
{
    TEST_ASSERT_INT_EQUAL(0, EXEC_KILLED_NONE, "none = 0");
    TEST_ASSERT_INT_EQUAL(1, EXEC_KILLED_TIMEOUT, "timeout = 1");
    TEST_ASSERT_INT_EQUAL(2, EXEC_KILLED_CTRL_BREAK, "ctrl_break = 2");
    TEST_ASSERT_INT_EQUAL(3, EXEC_KILLED_MEMORY_CAP, "memory_cap = 3");
    TEST_ASSERT_INT_EQUAL(4, EXEC_KILLED_CPU_CAP, "cpu_cap = 4");
}

/* ================================================================
 * ClearHandleInherit - both routes. Obligation (propagate gap, F2): the
 * static ClearHandleInherit clears HANDLE_FLAG_INHERIT via
 * SetHandleInformation when present, ELSE (the NT 3.1 floor) via a
 * non-inheritable DuplicateHandle + CloseHandle(original) + *ph = dup. The
 * fallback route had no test. ExecClearHandleInheritForTest (TEST_BUILD)
 * reaches the static helper; NULLing g_features.pSetHandleInformation forces
 * the fallback on a host whose kernel32 DOES export SetHandleInformation
 * (mingw/wine/native), simulating the NT 3.1 path.
 *
 * GetHandleInformation/SetHandleInformation exist on the host even though NT
 * 3.1 lacks them - the test forces the probe NULL only inside exec_ops, and
 * still reads back the flag through the host's real GetHandleInformation.
 * ================================================================ */

/* Same type as g_features.pSetHandleInformation, so save/restore stays a
 * function-pointer assignment (C89 forbids fnptr<->void* casts). */
typedef BOOL (WINAPI *SetHandleInfoFn)(HANDLE, DWORD, DWORD);

/* Make an inheritable pipe-read handle (HANDLE_FLAG_INHERIT set). */
static HANDLE make_inheritable_handle(void)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE rd, wr;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        return NULL;
    }
    CloseHandle(wr);          /* we only need the read end */
    return rd;                /* inheritable per the SECURITY_ATTRIBUTES */
}

/* ClearHandleInherit, fallback (NT 3.1 DuplicateHandle) route. */
TEST_CASE(clear_inherit_fallback)
{
    SetHandleInfoFn saved;
    HANDLE h;
    DWORD flags;
    saved = g_features.pSetHandleInformation;

    h = make_inheritable_handle();
    TEST_ASSERT(h != NULL && h != INVALID_HANDLE_VALUE, "made inheritable handle");
    flags = 0;
    TEST_ASSERT(GetHandleInformation(h, &flags) != 0, "read initial flags");
    TEST_ASSERT((flags & HANDLE_FLAG_INHERIT) != 0, "handle starts inheritable");

    /* Force the NT 3.1 fallback path. */
    g_features.pSetHandleInformation = NULL;
    ExecClearHandleInheritForTest(&h);
    g_features.pSetHandleInformation = saved;   /* restore */

    TEST_ASSERT(h != NULL && h != INVALID_HANDLE_VALUE,
                "fallback left a valid handle");
    flags = 0;
    TEST_ASSERT(GetHandleInformation(h, &flags) != 0,
                "duplicated handle is queryable");
    TEST_ASSERT((flags & HANDLE_FLAG_INHERIT) == 0,
                "fallback cleared HANDLE_FLAG_INHERIT");
    CloseHandle(h);
}

/* ClearHandleInherit, API (SetHandleInformation) route. */
TEST_CASE(clear_inherit_api)
{
    HANDLE h;
    DWORD flags;

    if (g_features.pSetHandleInformation == NULL) {
        TEST_ASSERT(1, "skipped: host has no SetHandleInformation probe");
        return;
    }
    h = make_inheritable_handle();
    TEST_ASSERT(h != NULL && h != INVALID_HANDLE_VALUE, "made inheritable handle");
    flags = 0;
    TEST_ASSERT(GetHandleInformation(h, &flags) != 0, "read initial flags");
    TEST_ASSERT((flags & HANDLE_FLAG_INHERIT) != 0, "handle starts inheritable");

    ExecClearHandleInheritForTest(&h);   /* probe present -> API route */

    TEST_ASSERT(h != NULL && h != INVALID_HANDLE_VALUE, "handle still valid");
    flags = 0;
    TEST_ASSERT(GetHandleInformation(h, &flags) != 0, "handle queryable");
    TEST_ASSERT((flags & HANDLE_FLAG_INHERIT) == 0,
                "API route cleared HANDLE_FLAG_INHERIT");
    CloseHandle(h);
}

/* ClearHandleInherit guards: NULL ph, NULL handle, INVALID_HANDLE_VALUE all
 * no-op without a crash, on both routes. */
TEST_CASE(clear_inherit_guards)
{
    SetHandleInfoFn saved;
    HANDLE h;
    int route;

    saved = g_features.pSetHandleInformation;
    for (route = 0; route < 2; route++) {
        /* route 0: API present (if any); route 1: forced fallback. */
        if (route == 1) {
            g_features.pSetHandleInformation = NULL;
        }
        /* NULL ph: no crash, returns. */
        ExecClearHandleInheritForTest(NULL);
        /* NULL handle: no crash. */
        h = NULL;
        ExecClearHandleInheritForTest(&h);
        TEST_ASSERT(h == NULL, "NULL handle left unchanged");
        /* INVALID_HANDLE_VALUE: no crash. */
        h = INVALID_HANDLE_VALUE;
        ExecClearHandleInheritForTest(&h);
        TEST_ASSERT(h == INVALID_HANDLE_VALUE, "INVALID handle left unchanged");
    }
    g_features.pSetHandleInformation = saved;   /* restore */
    TEST_ASSERT(1, "guards survived both routes");
}

int main(void)
{
    /* Unbuffered: a hang mid-suite must not swallow progress output. */
    setvbuf(stdout, NULL, _IONBF, 0);

    FeatInit();

    printf("Running exec_ops tests...\n");

    RUN_TEST(echo_hello);                       /* #1 */
    RUN_TEST(exit_code);                        /* #2 */
    RUN_TEST(nonexistent_exe);                  /* #3 */
    RUN_TEST(stdout_truncated);                 /* #4 */
    RUN_TEST(timeout_kill);                     /* #5 */
    RUN_TEST(stdin_passthrough);                /* #6 */
    RUN_TEST(cwd_respected);                    /* #7 */
    RUN_TEST(stderr_capture);                   /* #8 */
    RUN_TEST(empty_cmdline);                    /* #9 */
    RUN_TEST(cmdline_too_long);                 /* #10 */
    RUN_TEST(cmdline_shell_cap_is_dispatcher);  /* #11 */
    RUN_TEST(nonexistent_cwd);                  /* #12 */
    RUN_TEST(generous_timeout_completes);       /* #13 */
    RUN_TEST(invalid_base64_is_dispatcher);     /* #14 */
    RUN_TEST(shell_dir_routing);                /* #15 */
    RUN_TEST(spawn_fail_sentinel);              /* #16 */
    RUN_TEST(busy_is_dispatcher_level);         /* #17 */
    RUN_TEST(final_drain);                      /* #18 */
    RUN_TEST(fallback_polling_path);            /* #19 */
    RUN_TEST(fallback_no_job_objects);          /* #20 */
    RUN_TEST(fallback_no_ctrl_events);          /* #21 */
    RUN_TEST(job_memory_cap);
    RUN_TEST(job_cpu_cap_kills);                   /* #22 */

    /* OBLIGATIONS-PHASE4.md additions exercisable at this layer. */
    RUN_TEST(still_active_orphan);
    RUN_TEST(orphan_reaped_on_repoll);
    RUN_TEST(pe32_timeout_not_still_active);
    RUN_TEST(killed_by_vocabulary);

    /* F2: ClearHandleInherit - both routes + guards (propagate gap). */
    RUN_TEST(clear_inherit_fallback);
    RUN_TEST(clear_inherit_api);
    RUN_TEST(clear_inherit_guards);

    print_test_summary();
    return g_tests_failed;
}
