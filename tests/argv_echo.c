/*
 * argv_echo.c - PBT helper for test_argv.c (plan/PHASE4.md argv roundtrip).
 *
 * Prints argc on the first line, then each argv[i] base64-encoded, one per
 * line. The parent (test_argv.c) joins a random argv with ArgvJoin, spawns
 * this program via CreateProcessA, and base64-decodes the lines back to
 * verify the child received the original vector byte-for-byte. Base64 is
 * used so embedded spaces, quotes and control bytes survive the pipe.
 *
 * C89. Uses src/base64.h.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <stdio.h>
#include <string.h>
#include "base64.h"

int main(int argc, char **argv)
{
    int i;

    printf("%d\n", argc);
    for (i = 0; i < argc; i++) {
        char encoded[4096];
        int n;

        n = Base64Encode((const unsigned char *)argv[i],
                         (int)strlen(argv[i]),
                         encoded, (int)sizeof(encoded));
        if (n < 0) {
            return 1;
        }
        /* Base64Encode of empty input writes 0 bytes and "" -> empty line. */
        printf("%s\n", encoded);
    }
    return 0;
}
