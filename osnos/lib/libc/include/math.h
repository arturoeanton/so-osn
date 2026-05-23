#pragma once

/*
 * Minimal math.h — just enough for TCC's runtime / type tables and
 * for simple user programs that touch FP. Implementations are
 * software where they can be (fabs, fmod), otherwise stub to a
 * polynomial approximation. They are NOT IEEE-precise; programs
 * that need high-quality fp math should link against a real libm.
 *
 * Ring-3 ELFs CAN use float/double — the kernel inits the FPU at
 * boot. Today FPU state is NOT saved per task switch, so mixing
 * FP across multiple concurrent user tasks is unsafe. Single-task
 * use (like TCC compiling a file in the foreground) is fine.
 */

#define M_E         2.7182818284590452354
#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_SQRT2     1.41421356237309504880
#define M_SQRT1_2   0.70710678118654752440
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402

#define INFINITY    __builtin_inff()
#define NAN         __builtin_nanf("")
#define HUGE_VAL    __builtin_inf()
#define HUGE_VALF   __builtin_inff()

int isnan(double x);
/* isnormal: true if x is finite + normalized (NOT subnormal/zero/inf/NaN).
 * Used by jq to detect numbers that need special formatting. */
int isnormal(double x);
int isinf(double x);
int isfinite(double x);

double fabs (double x);
double floor(double x);
double ceil (double x);
double trunc(double x);
double round(double x);
double fmod (double x, double y);

double sqrt (double x);
double pow  (double base, double exp);
double exp  (double x);
double log  (double x);
double log2 (double x);
double log10(double x);
/* ldexp: x * 2^n. Used by TCC's preprocessor when constant-folding
 * decimal FP literals like 1.5e10 — frequently in a row, so worth
 * having even though it's trivial. */
double ldexp(double x, int n);
float  ldexpf(float x, int n);

double sin  (double x);
double cos  (double x);
double tan  (double x);
double atan (double x);
double atan2(double y, double x);
double asin (double x);
double acos (double x);
double sinh (double x);
double cosh (double x);
double tanh (double x);

/* frexp: x = m * 2^*e with 0.5 <= |m| < 1 (or m == 0).
 * modf:  return integer part in *iptr, fractional in return. */
double frexp(double x, int *e);
double modf (double x, double *iptr);

float  fabsf (float x);
float  sqrtf (float x);
float  floorf(float x);
float  ceilf (float x);
