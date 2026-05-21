#pragma once

/*
 * Floating-point limits. Matches IEEE-754 binary32 / binary64 /
 * x86 extended precision (80-bit) — what an x86_64 toolchain
 * defaults to. TCC's internal type tables read these constants
 * verbatim.
 *
 * The kernel itself is built with `-mno-sse`, but ring-3 user
 * tasks have an FPU available (initialised at boot), so float and
 * double arithmetic is fair game in ELFs.
 */

#define FLT_RADIX        2

#define FLT_MANT_DIG     24
#define DBL_MANT_DIG     53
#define LDBL_MANT_DIG    64

#define FLT_DIG          6
#define DBL_DIG          15
#define LDBL_DIG         18

#define FLT_EPSILON      1.19209290e-7F
#define DBL_EPSILON      2.2204460492503131e-16
#define LDBL_EPSILON     1.0842021724855044e-19L

#define FLT_MIN          1.17549435e-38F
#define DBL_MIN          2.2250738585072014e-308
#define LDBL_MIN         3.3621031431120935e-4932L

#define FLT_MAX          3.40282347e+38F
#define DBL_MAX          1.7976931348623157e+308
#define LDBL_MAX         1.18973149535723176502e+4932L

#define FLT_MIN_EXP      (-125)
#define DBL_MIN_EXP      (-1021)
#define LDBL_MIN_EXP     (-16381)

#define FLT_MAX_EXP      128
#define DBL_MAX_EXP      1024
#define LDBL_MAX_EXP     16384

#define FLT_MIN_10_EXP   (-37)
#define DBL_MIN_10_EXP   (-307)
#define LDBL_MIN_10_EXP  (-4931)

#define FLT_MAX_10_EXP   38
#define DBL_MAX_10_EXP   308
#define LDBL_MAX_10_EXP  4932

#define FLT_ROUNDS       1   /* round to nearest */
