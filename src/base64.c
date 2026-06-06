/*
 * base64.c - Base64 encode/decode for MCP-Win32s
 *
 * Standard alphabet, integer-only, C89, i386-safe.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <stddef.h>
#include "base64.h"

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*
 * b64_index - Return the index of a base64 character, or -1 if invalid.
 * Uses a small lookup for the standard alphabet.
 */
static int b64_index(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

int Base64Encode(const unsigned char *in, int in_len, char *out, int out_size)
{
    int i;
    int out_pos;

    if (out == NULL || out_size < 1) {
        return 0;
    }

    if (in_len == 0) {
        out[0] = '\0';
        return 0;
    }

    if (in == NULL) {
        return 0;
    }

    /* Calculate required output size: ceil(in_len/3)*4 + 1 for NUL */
    {
        int needed;
        needed = ((in_len + 2) / 3) * 4 + 1;
        if (out_size < needed) {
            return 0;
        }
    }

    out_pos = 0;

    for (i = 0; i < in_len; i += 3) {
        unsigned char a, b, c;
        int remaining;

        remaining = in_len - i;

        a = in[i];
        b = (remaining > 1) ? in[i + 1] : 0;
        c = (remaining > 2) ? in[i + 2] : 0;

        out[out_pos++] = b64_table[(a >> 2) & 0x3F];
        out[out_pos++] = b64_table[((a & 0x03) << 4) | ((b >> 4) & 0x0F)];

        if (remaining > 1) {
            out[out_pos++] = b64_table[((b & 0x0F) << 2) | ((c >> 6) & 0x03)];
        } else {
            out[out_pos++] = '=';
        }

        if (remaining > 2) {
            out[out_pos++] = b64_table[c & 0x3F];
        } else {
            out[out_pos++] = '=';
        }
    }

    out[out_pos] = '\0';
    return out_pos;
}

int Base64Decode(const char *in, unsigned char *out, int out_size)
{
    int in_len;
    int i;
    int out_pos;
    /* Accumulator is unsigned: the rolling (buf << 6) would overflow a
     * signed int after enough high-value sextets - undefined behaviour
     * in C89 (found by the theft/UBSan host harness, Phase 4). Only the
     * low bits are ever extracted, so unsigned wraparound is correct. */
    unsigned int buf;
    int buf_bits;

    if (in == NULL || out == NULL || out_size < 1) {
        return -1;
    }

    /* Calculate input length */
    in_len = 0;
    while (in[in_len] != '\0') {
        in_len++;
    }

    if (in_len == 0) {
        out[0] = '\0';
        return 0;
    }

    buf = 0;
    buf_bits = 0;
    out_pos = 0;

    for (i = 0; i < in_len; i++) {
        unsigned char c;
        int val;

        c = (unsigned char)in[i];

        /* Skip whitespace */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }

        /* Handle padding */
        if (c == '=') {
            /* Padding is only valid at the end */
            break;
        }

        /* Look up value */
        val = b64_index(c);
        if (val < 0) {
            return -1;
        }

        buf = (buf << 6) | (unsigned int)val;
        buf_bits += 6;

        /* Extract bytes when we have 8+ bits */
        while (buf_bits >= 8) {
            if (out_pos >= out_size) {
                return -1;
            }
            buf_bits -= 8;
            out[out_pos++] = (unsigned char)((buf >> buf_bits) & 0xFF);
        }
    }

    /* Discard remaining bits (should be 0 for valid input) */
    if (buf_bits >= 6) {
        /* Last group had padding that left non-zero bits — invalid */
        unsigned int remaining_val;
        remaining_val = buf & (((unsigned int)1 << buf_bits) - 1u);
        if (remaining_val != 0u) {
            return -1;
        }
    }

    out[out_pos] = '\0';
    return out_pos;
}
