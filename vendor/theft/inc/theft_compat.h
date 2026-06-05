#ifndef THEFT_COMPAT_H
#define THEFT_COMPAT_H

#if __STDC_VERSION__ >= 199901L

/* C99 or later: use standard headers */

#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

typedef uint64_t theft_uint64;
typedef uint32_t theft_uint32;
typedef uint16_t theft_uint16;
typedef uint8_t  theft_uint8;

#define THEFT_PRIu64 PRIu64
#define THEFT_PRId64 PRId64
#define THEFT_PRIx64 PRIx64
#define THEFT_PRIu32 PRIu32
#define THEFT_PRId32 PRId32

#else

/* C89: rely on compiler extensions for fixed-width types
 * GCC provides <stdint.h> as an extension even in -std=c89 mode.
 * VC6 needs __int64-based definitions. */

/* GCC supports variadic macros as an extension in C89 mode.
 * Define GREATEST_VA_ARGS so greatest.h's RUN_TESTp works. */
#ifndef GREATEST_VA_ARGS
#define GREATEST_VA_ARGS
#endif

#if defined(_MSC_VER) && (_MSC_VER < 1300)
typedef unsigned __int64 uint64_t;
typedef __int64 int64_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned short uint16_t;
typedef short int16_t;
typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef int bool;
#define true 1
#define false 0
#define THEFT_PRIu64 "I64u"
#define THEFT_PRId64 "I64d"
#define THEFT_PRIx64 "I64x"
#define THEFT_PRIu32 "u"
#define THEFT_PRId32 "d"
#else
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#define THEFT_PRIu64 PRIu64
#define THEFT_PRId64 PRId64
#define THEFT_PRIx64 PRIx64
#define THEFT_PRIu32 PRIu32
#define THEFT_PRId32 PRId32
#endif

typedef uint64_t theft_uint64;
typedef uint32_t theft_uint32;
typedef uint16_t theft_uint16;
typedef uint8_t  theft_uint8;

#endif

/* Sanity-check type sizes */
typedef char theft_static_assert_uint64[sizeof(theft_uint64) == 8 ? 1 : -1];
typedef char theft_static_assert_uint32[sizeof(theft_uint32) == 4 ? 1 : -1];
typedef char theft_static_assert_uint16[sizeof(theft_uint16) == 2 ? 1 : -1];
typedef char theft_static_assert_uint8[sizeof(theft_uint8) == 1 ? 1 : -1];

#endif
