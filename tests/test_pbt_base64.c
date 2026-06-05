/*
 * test_pbt_base64.c - Property-based tests for base64.c
 *
 * Uses prop.h (minimal C89 PBT framework). Tests roundtrip and
 * structural invariants over 1000 random trials each.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#define PROP_IMPLEMENTATION
#include "prop.h"
#include "base64.h"
#include <stdio.h>
#include <string.h>

/* Property: encode then decode returns original bytes */
PROP_TEST(base64_roundtrip) {
    unsigned char orig[256] = {0};
    char encoded[1024];
    unsigned char decoded[256];
    int len;
    int enc_len;
    int dec_len;
    int i;

    /* Generate random byte sequence (0-255 bytes) */
    len = PROP_INT(0, 255);

    for (i = 0; i < len; i++) {
        orig[i] = (unsigned char)PROP_INT(0, 255);
    }

    enc_len = Base64Encode(orig, len, encoded, (int)sizeof(encoded));
    PROP_CHECK(enc_len >= 0);

    /* Empty input should produce empty output */
    if (len == 0) {
        PROP_CHECK(enc_len == 0);
        return;
    }

    dec_len = Base64Decode(encoded, decoded, (int)sizeof(decoded));
    PROP_CHECK(dec_len == len);

    for (i = 0; i < len; i++) {
        PROP_CHECK(decoded[i] == orig[i]);
    }
}

/* Property: encoded output contains only valid base64 chars */
PROP_TEST(base64_output_is_valid_alphabet) {
    unsigned char orig[64] = {0};
    char encoded[256];
    int len;
    int enc_len;
    int i;

    len = PROP_INT(0, 63);

    for (i = 0; i < len; i++) {
        orig[i] = (unsigned char)PROP_INT(0, 255);
    }

    enc_len = Base64Encode(orig, len, encoded, (int)sizeof(encoded));
    PROP_CHECK(enc_len >= 0);

    for (i = 0; i < enc_len; i++) {
        char ch;
        int valid;
        ch = encoded[i];
        valid = 0;
        if (ch >= 'A' && ch <= 'Z') valid = 1;
        if (ch >= 'a' && ch <= 'z') valid = 1;
        if (ch >= '0' && ch <= '9') valid = 1;
        if (ch == '+' || ch == '/' || ch == '=') valid = 1;
        PROP_CHECK(valid);
    }
}

/* Property: encoded length is ceil(n/3)*4 */
PROP_TEST(base64_output_length) {
    unsigned char orig[100] = {0};
    char encoded[512];
    int len;
    int enc_len;
    int expected;
    int i;

    len = PROP_INT(0, 99);

    for (i = 0; i < len; i++) {
        orig[i] = (unsigned char)PROP_INT(0, 255);
    }

    enc_len = Base64Encode(orig, len, encoded, (int)sizeof(encoded));
    expected = ((len + 2) / 3) * 4;

    if (len == 0) {
        PROP_CHECK(enc_len == 0);
    } else {
        PROP_CHECK(enc_len == expected);
    }
}

/* Property: decode of invalid base64 returns -1 */
PROP_TEST(base64_decode_rejects_invalid) {
    unsigned char out[16];
    int dec_len;
    char bad[64];
    int bad_len;
    int i;

    /* Build a string with an invalid character at position 0 */
    bad_len = PROP_INT(2, 30);
    for (i = 0; i < bad_len; i++) {
        unsigned char ch;
        ch = (unsigned char)PROP_CHAR_RAW();
        /* Position 0: must be a char the decoder cannot skip or accept */
        if (i == 0) {
            do {
                ch = (unsigned char)PROP_CHAR_RAW();
            } while ((ch >= 'A' && ch <= 'Z') ||
                     (ch >= 'a' && ch <= 'z') ||
                     (ch >= '0' && ch <= '9') ||
                     ch == '+' || ch == '/' || ch == '=' ||
                     ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
                     ch == '\0');
        }
        bad[i] = (char)ch;
    }
    bad[bad_len] = '\0';

    dec_len = Base64Decode(bad, out, (int)sizeof(out));
    PROP_CHECK(dec_len == -1);
}

int main(void)
{
    prop_seed(0);

    PROP_RUN(base64_roundtrip,           1000);
    PROP_RUN(base64_output_is_valid_alphabet, 1000);
    PROP_RUN(base64_output_length,       1000);
    PROP_RUN(base64_decode_rejects_invalid, 1000);

    return prop_summary();
}
