/*
 * base64.h - Base64 encode/decode for MCP-Win32s
 *
 * Integer-only implementation. No floating point. C89.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef BASE64_H
#define BASE64_H

/*
 * Base64Encode - Encode binary data to base64 string.
 *
 * Standard alphabet: A-Z, a-z, 0-9, +, /. '=' padding.
 *
 * Returns output length (excluding NUL). Returns 0 if out buffer too small.
 * out_size must be >= ((in_len + 2) / 3) * 4 + 1 for guaranteed success.
 */
int Base64Encode(const unsigned char *in, int in_len, char *out, int out_size);

/*
 * Base64Decode - Decode base64 string to binary data.
 *
 * Skips whitespace (\n, \r, \t, ' '). Rejects any other non-alphabet
 * character (including '=') in the middle of data. '=' padding accepted
 * only at end.
 *
 * Returns decoded byte count, or -1 on invalid input.
 * out_size must be >= (in_len * 3 / 4) for guaranteed success.
 */
int Base64Decode(const char *in, unsigned char *out, int out_size);

#endif /* BASE64_H */
