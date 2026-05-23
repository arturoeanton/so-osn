#include <math.h>
#include <stdint.h>

/*
 * Tiny libm. Accuracy is not the priority — these implementations
 * are good enough for TCC's runtime tables, basic graphing toys,
 * and "did this program actually run?" smoke tests. Replace with
 * a real libm if you ever need precise results.
 *
 * The kernel disables SSE for its own code, but user-mode ELFs
 * get an active FPU at boot (CR0.MP set, EM clear, FNINIT done),
 * so the x87 instructions emitted by clang for plain `double`
 * arithmetic work. No SSE intrinsics here.
 */

/* ---- IEEE bit pokes ---- */

static inline uint64_t dbits(double x) {
    union { double d; uint64_t u; } u = { x };
    return u.u;
}

int isnan(double x) {
    uint64_t u = dbits(x);
    uint64_t exp  = (u >> 52) & 0x7FF;
    uint64_t mant = u & 0xFFFFFFFFFFFFFULL;
    return exp == 0x7FF && mant != 0;
}

int isinf(double x) {
    uint64_t u = dbits(x);
    uint64_t exp  = (u >> 52) & 0x7FF;
    uint64_t mant = u & 0xFFFFFFFFFFFFFULL;
    return exp == 0x7FF && mant == 0;
}

int isfinite(double x) {
    uint64_t u = dbits(x);
    uint64_t exp = (u >> 52) & 0x7FF;
    return exp != 0x7FF;
}

/* isnormal: true iff x is a finite, non-zero, non-subnormal number.
 * Equivalently: exponent field is in [1, 2046]. */
int isnormal(double x) {
    uint64_t u = dbits(x);
    uint64_t exp = (u >> 52) & 0x7FF;
    return (exp != 0 && exp != 0x7FF);
}

/* ---- Trivial wrappers ---- */

double fabs(double x)  { return x < 0 ? -x : x; }
float  fabsf(float x)  { return x < 0 ? -x : x; }

double floor(double x) {
    /* Round toward -infinity. Handles negative correctly. */
    double t = (double)(int64_t)x;
    if (t > x) t -= 1.0;
    return t;
}
float floorf(float x)  { return (float)floor(x); }

double ceil(double x) {
    double t = (double)(int64_t)x;
    if (t < x) t += 1.0;
    return t;
}
float ceilf(float x)   { return (float)ceil(x); }

double trunc(double x) { return (double)(int64_t)x; }

double round(double x) {
    return (x >= 0) ? floor(x + 0.5) : ceil(x - 0.5);
}

double fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    double q = trunc(x / y);
    return x - q * y;
}

/* ---- Iterative approximations ---- */

double sqrt(double x) {
    if (x < 0)   return NAN;
    if (x == 0)  return 0;
    /* Newton iteration: r_{n+1} = (r_n + x/r_n) / 2 */
    double r = x;
    for (int i = 0; i < 40; i++) {
        if (r == 0) break;
        double next = 0.5 * (r + x / r);
        if (next == r) break;
        r = next;
    }
    return r;
}

float sqrtf(float x) { return (float)sqrt(x); }

/* exp via the standard truncated series exp(x) = sum x^k/k!. Argument
 * reduction halves x repeatedly and squares the result, keeping the
 * series accurate over a wider range. */
double exp(double x) {
    if (isnan(x)) return x;
    int shrinks = 0;
    while (x > 0.5)  { x *= 0.5;  shrinks++; if (shrinks > 32) break; }
    while (x < -0.5) { x *= 0.5;  shrinks++; if (shrinks > 32) break; }
    double term = 1.0;
    double sum  = 1.0;
    for (int k = 1; k < 30; k++) {
        term *= x / (double)k;
        sum  += term;
    }
    for (int i = 0; i < shrinks; i++) sum *= sum;
    return sum;
}

/* log via ln(1+y) Taylor where y = (x-1)/(x+1), then doubled. */
double log(double x) {
    if (x <= 0) return NAN;
    /* Bring x into a small range around 1. */
    int scale = 0;
    while (x > 2.0)  { x *= 0.5; scale++; }
    while (x < 0.5)  { x *= 2.0; scale--; }
    double y = (x - 1) / (x + 1);
    double y2 = y * y;
    double term = y;
    double sum  = 0.0;
    for (int k = 1; k < 50; k += 2) {
        sum  += term / (double)k;
        term *= y2;
    }
    return 2.0 * sum + (double)scale * M_LN2;
}

double log2 (double x) { return log(x) / M_LN2;  }
double log10(double x) { return log(x) / M_LN10; }

/* ldexp: multiply by a power of two by adjusting the IEEE 754 exponent
 * field directly when possible. Falls back to repeated mul/div when n
 * is out of the normal range so the result is still numerically OK. */
double ldexp(double x, int n) {
    if (x == 0.0 || n == 0) return x;
    /* Saturate huge n into a series of bounded shifts to avoid
     * trying to construct a sub-/super-normal exponent in one step. */
    while (n >  1023) { x *= 2.0; n -= 1; }
    while (n < -1022) { x *= 0.5; n += 1; }
    /* Build 2^n with bias 1023, then multiply. */
    union { double d; unsigned long long u; } pow2;
    pow2.u = ((unsigned long long)(n + 1023)) << 52;
    return x * pow2.d;
}

float ldexpf(float x, int n) { return (float)ldexp((double)x, n); }

double pow(double base, double exp_) {
    if (base == 0) return exp_ == 0 ? 1.0 : 0.0;
    /* For non-positive bases skip log path; integer exponents only. */
    if (base < 0) {
        long long n = (long long)exp_;
        if ((double)n != exp_) return NAN;
        double r = 1.0;
        double b = (n < 0) ? 1.0 / base : base;
        if (n < 0) n = -n;
        for (long long i = 0; i < n; i++) r *= b;
        return r;
    }
    return exp(exp_ * log(base));
}

/* ---- Trig via range reduction + Taylor. ---- */

static double sin_core(double x) {
    /* sin Taylor x - x^3/6 + x^5/120 - ... for |x| <= pi/2. */
    double x2 = x * x;
    double term = x;
    double sum  = 0.0;
    for (int k = 1; k < 30; k += 2) {
        sum += term;
        term *= -x2 / (double)((k + 1) * (k + 2));
    }
    return sum;
}

double sin(double x) {
    /* Reduce mod 2*pi. */
    double two_pi = 2.0 * M_PI;
    while (x >  M_PI) x -= two_pi;
    while (x < -M_PI) x += two_pi;
    /* Reduce to [-pi/2, pi/2] by reflection. */
    if (x >  M_PI_2) x =  M_PI - x;
    if (x < -M_PI_2) x = -M_PI - x;
    return sin_core(x);
}

double cos(double x) { return sin(x + M_PI_2); }

double tan(double x) {
    double c = cos(x);
    if (c == 0) return NAN;
    return sin(x) / c;
}

/* atan via Taylor on |x|<=1, identity atan(x) = pi/2 - atan(1/x) elsewhere. */
double atan(double x) {
    if (x > 1.0)  return M_PI_2 - atan(1.0 / x);
    if (x < -1.0) return -M_PI_2 - atan(1.0 / x);
    double x2 = x * x;
    double term = x;
    double sum  = 0.0;
    for (int k = 1; k < 60; k += 2) {
        sum += term / (double)k;
        term *= -x2;
    }
    return sum;
}

double atan2(double y, double x) {
    if (x > 0) return atan(y / x);
    if (x < 0) {
        return atan(y / x) + (y >= 0 ? M_PI : -M_PI);
    }
    return (y >= 0) ? M_PI_2 : -M_PI_2;
}

/* asin via the identity asin(x) = atan(x / sqrt(1 - x^2)) for
 * |x| < 1; endpoints saturated to ±π/2. */
double asin(double x) {
    if (x >= 1.0)  return  M_PI_2;
    if (x <= -1.0) return -M_PI_2;
    return atan(x / sqrt(1.0 - x * x));
}

double acos(double x) {
    return M_PI_2 - asin(x);
}

/* sinh = (e^x - e^-x) / 2, cosh = (e^x + e^-x) / 2.
 * Not great for large |x| (loss of precision) but fine for the
 * range Lua scripts realistically pass in. */
double sinh(double x) {
    double e = exp(x);
    return (e - 1.0 / e) * 0.5;
}

double cosh(double x) {
    double e = exp(x);
    return (e + 1.0 / e) * 0.5;
}

double tanh(double x) {
    /* Avoid overflow for large |x|. */
    if (x >  20.0) return  1.0;
    if (x < -20.0) return -1.0;
    double ex = exp(x), eix = 1.0 / ex;
    return (ex - eix) / (ex + eix);
}

/* frexp: pull the IEEE 754 exponent out by punning the double,
 * normalize the mantissa back into [0.5, 1.0). Caller gets the
 * unbiased exponent in *e. Mirrors how glibc does it on x86_64. */
double frexp(double x, int *e) {
    if (x == 0.0) { *e = 0; return 0.0; }
    union { double d; unsigned long long u; } pun;
    pun.d = x;
    int raw = (int)((pun.u >> 52) & 0x7FF);
    if (raw == 0x7FF) { *e = 0; return x; }   /* inf / NaN — pass-through */
    *e = raw - 1022;
    /* Reset exponent field to bias-1 (= 0x3FE) so mantissa
     * lands in [0.5, 1.0). */
    pun.u = (pun.u & 0x800FFFFFFFFFFFFFULL) | ((unsigned long long)0x3FE << 52);
    return pun.d;
}

/* modf: split into integer (in *iptr) and fractional parts (return). */
double modf(double x, double *iptr) {
    double i = (double)(long long)x;
    /* For negative x with no fractional part, (long long)x truncates
     * toward zero; that's already correct for modf. */
    if (iptr) *iptr = i;
    return x - i;
}
