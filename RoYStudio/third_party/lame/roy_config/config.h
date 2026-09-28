/* RoY Studio build configuration for LAME 3.100 (libmp3lame only, no decoder, no frontend).
   Written for this project (replaces the autoconf-generated config.h). */
#ifndef ROY_LAME_CONFIG_H
#define ROY_LAME_CONFIG_H
#define PACKAGE "lame"
#define VERSION "3.100"
#define STDC_HEADERS 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#include <stdint.h>
#define HAVE_INT8_T 1
#define HAVE_INT16_T 1
#define HAVE_INT32_T 1
#define HAVE_INT64_T 1
#define HAVE_UINT8_T 1
#define HAVE_UINT16_T 1
#define HAVE_UINT32_T 1
#define HAVE_UINT64_T 1
typedef float ieee754_float32_t;
typedef double ieee754_float64_t;
typedef long double ieee854_float80_t;
#define HAVE_IEEE754_FLOAT32_T 1
#define HAVE_IEEE754_FLOAT64_T 1
#define HAVE_IEEE854_FLOAT80_T 1
#if defined(__x86_64__) || defined(_M_X64)
#define HAVE_XMMINTRIN_H 1
#define MIN_ARCH_SSE 1
#endif
#define USE_FAST_LOG 1
#define TAKEHIRO_IEEE754_HACK 1
#endif
