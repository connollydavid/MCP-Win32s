/*
 * test_base64.c - Unit tests for base64.c
 *
 * Tests encoding, decoding, roundtrip, edge cases, and error conditions.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <stdio.h>
#include <string.h>
#include "test_framework.h"
#include "base64.h"

/* ========================================================
 * Encode - Happy Path
 * ======================================================== */

TEST_CASE(encode_empty) {
    char out[16];
    int len;
    len = Base64Encode(NULL, 0, out, sizeof(out));
    TEST_ASSERT_INT_EQUAL(0, len, "encode empty returns 0");
    TEST_ASSERT_STR_EQUAL("", out, "output is empty string");
}

TEST_CASE(encode_one_byte) {
    unsigned char in[1];
    char out[16];
    int len;
    in[0] = 'f';
    len = Base64Encode(in, 1, out, sizeof(out));
    TEST_ASSERT_INT_EQUAL(4, len, "encode 1 byte returns 4");
    TEST_ASSERT_STR_EQUAL("Zg==", out, "f -> Zg==");
}

TEST_CASE(encode_two_bytes) {
    unsigned char in[2];
    char out[16];
    int len;
    in[0] = 'f';
    in[1] = 'o';
    len = Base64Encode(in, 2, out, sizeof(out));
    TEST_ASSERT_INT_EQUAL(4, len, "encode 2 bytes returns 4");
    TEST_ASSERT_STR_EQUAL("Zm8=", out, "fo -> Zm8=");
}

TEST_CASE(encode_three_bytes) {
    unsigned char in[3];
    char out[16];
    int len;
    in[0] = 'f';
    in[1] = 'o';
    in[2] = 'o';
    len = Base64Encode(in, 3, out, sizeof(out));
    TEST_ASSERT_INT_EQUAL(4, len, "encode 3 bytes returns 4");
    TEST_ASSERT_STR_EQUAL("Zm9v", out, "foo -> Zm9v");
}

TEST_CASE(encode_four_bytes) {
    unsigned char in[4];
    char out[16];
    int len;
    in[0] = 'f';
    in[1] = 'o';
    in[2] = 'o';
    in[3] = 'b';
    len = Base64Encode(in, 4, out, sizeof(out));
    TEST_ASSERT_INT_EQUAL(8, len, "encode 4 bytes returns 8");
    TEST_ASSERT_STR_EQUAL("Zm9vYg==", out, "foob -> Zm9vYg==");
}

TEST_CASE(encode_six_bytes) {
    unsigned char in[6];
    char out[16];
    int len;
    int j;
    const char *src;
    src = "foobar";
    for (j = 0; j < 6; j++) {
        in[j] = (unsigned char)src[j];
    }
    len = Base64Encode(in, 6, out, sizeof(out));
    TEST_ASSERT_INT_EQUAL(8, len, "encode 6 bytes returns 8");
    TEST_ASSERT_STR_EQUAL("Zm9vYmFy", out, "foobar -> Zm9vYmFy");
}

/* ========================================================
 * Decode - Happy Path
 * ======================================================== */

TEST_CASE(decode_Zg) {
    unsigned char out[16];
    int len;
    len = Base64Decode("Zg==", out, sizeof(out));
    TEST_ASSERT_INT_EQUAL(1, len, "decode Zg== returns 1");
    TEST_ASSERT_INT_EQUAL('f', out[0], "decoded byte is 'f'");
}

TEST_CASE(decode_Zm8) {
    unsigned char out[16];
    int len;
    len = Base64Decode("Zm8=", out, sizeof(out));
    TEST_ASSERT_INT_EQUAL(2, len, "decode Zm8= returns 2");
    TEST_ASSERT_INT_EQUAL('f', out[0], "first byte 'f'");
    TEST_ASSERT_INT_EQUAL('o', out[1], "second byte 'o'");
}

TEST_CASE(decode_Zm9v) {
    unsigned char out[16];
    int len;
    len = Base64Decode("Zm9v", out, sizeof(out));
    TEST_ASSERT_INT_EQUAL(3, len, "decode Zm9v returns 3");
    TEST_ASSERT_INT_EQUAL('f', out[0], "f");
    TEST_ASSERT_INT_EQUAL('o', out[1], "o");
    TEST_ASSERT_INT_EQUAL('o', out[2], "o");
}

TEST_CASE(decode_foobar) {
    unsigned char out[16];
    int len;
    len = Base64Decode("Zm9vYmFy", out, sizeof(out));
    TEST_ASSERT_INT_EQUAL(6, len, "decode returns 6");
    TEST_ASSERT_STR_EQUAL("foobar", (const char *)out, "decoded is foobar");
}

/* ========================================================
 * Decode - Error Cases
 * ======================================================== */

TEST_CASE(decode_bad_char) {
    unsigned char out[16];
    int len;
    len = Base64Decode("AB!=", out, sizeof(out));
    TEST_ASSERT_INT_EQUAL(-1, len, "bad char returns -1");
}

TEST_CASE(decode_whitespace_accepted) {
    unsigned char out[16];
    int len;
    len = Base64Decode("Zm9v\nYmFy", out, sizeof(out));
    TEST_ASSERT_INT_EQUAL(6, len, "whitespace skipped, returns 6");
    TEST_ASSERT_STR_EQUAL("foobar", (const char *)out, "decoded is foobar");
}

/* ========================================================
 * Roundtrip
 * ======================================================== */

TEST_CASE(roundtrip_256_bytes) {
    unsigned char orig[256];
    unsigned char decoded[256];
    char encoded[1024];
    int i;
    int enc_len;
    int dec_len;

    for (i = 0; i < 256; i++) {
        orig[i] = (unsigned char)i;
    }

    enc_len = Base64Encode(orig, 256, encoded, sizeof(encoded));
    TEST_ASSERT_INT_EQUAL(344, enc_len, "encode 256 bytes -> 344 chars");

    dec_len = Base64Decode(encoded, decoded, sizeof(decoded));
    TEST_ASSERT_INT_EQUAL(256, dec_len, "decode returns 256");

    for (i = 0; i < 256; i++) {
        if (decoded[i] != (unsigned char)i) {
            TEST_ASSERT_INT_EQUAL(i, decoded[i], "byte matches at index");
        }
    }
    TEST_ASSERT_INT_EQUAL(0, 0, "all 256 bytes roundtrip identical");
}

TEST_CASE(encode_buffer_too_small) {
    unsigned char in[3];
    char out[4];
    int len;
    in[0] = 'f';
    in[1] = 'o';
    in[2] = 'o';
    /* Need 5 bytes (4 + NUL), only have 4 */
    len = Base64Encode(in, 3, out, sizeof(out));
    TEST_ASSERT_INT_EQUAL(0, len, "too-small buffer returns 0");
}

int main(void)
{
    RUN_TEST(encode_empty);
    RUN_TEST(encode_one_byte);
    RUN_TEST(encode_two_bytes);
    RUN_TEST(encode_three_bytes);
    RUN_TEST(encode_four_bytes);
    RUN_TEST(encode_six_bytes);
    RUN_TEST(decode_Zg);
    RUN_TEST(decode_Zm8);
    RUN_TEST(decode_Zm9v);
    RUN_TEST(decode_foobar);
    RUN_TEST(decode_bad_char);
    RUN_TEST(decode_whitespace_accepted);
    RUN_TEST(roundtrip_256_bytes);
    RUN_TEST(encode_buffer_too_small);

    print_test_summary();
    return g_tests_failed;
}
