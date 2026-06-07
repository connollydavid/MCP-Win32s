/*
 * mem_target.c - Round-trip helper child for test_mem_ops.c.
 *
 * The parent spawn-retains this child, then ReadProcessMemory/
 * WriteProcessMemory its address space to verify the process-tier peek/poke
 * path. It just sleeps long enough for the parent to finish, then exits; no
 * arguments are needed.
 *
 * C89. Win32 only.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <windows.h>

int main(void)
{
    Sleep(20000);
    return 0;
}
