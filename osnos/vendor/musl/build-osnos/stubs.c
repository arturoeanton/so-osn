/*
 * stubs.c — compiler-rt builtin stubs para musl/libc.so en osnos.
 *
 * Musl referencia internamente helpers de clang/gcc para complex
 * multiplication (__mulxc3, __mulsc3, __muldc3) y otros builtins
 * que normalmente vienen de libgcc.a o compiler_rt. Nuestro build
 * de musl no incluye LIBCC, así que estos quedan como UNDEFINED en
 * libc.so y ld.so se queja al cargar.
 *
 * Stubs: nada de los binaries dinámicos que estamos shipping usa
 * complex math, así que devuelven cero. Si un binario real lo
 * necesita, la solución correcta es linkear compiler-rt.
 */

typedef struct { long double r; long double i; } _Complex_long_double;
typedef struct { double r; double i; }            _Complex_double;
typedef struct { float r; float i; }              _Complex_float;

_Complex_long_double __mulxc3(long double a, long double b,
                              long double c, long double d) {
    (void)a; (void)b; (void)c; (void)d;
    _Complex_long_double r = { 0, 0 };
    return r;
}

_Complex_double __muldc3(double a, double b, double c, double d) {
    (void)a; (void)b; (void)c; (void)d;
    _Complex_double r = { 0, 0 };
    return r;
}

_Complex_float __mulsc3(float a, float b, float c, float d) {
    (void)a; (void)b; (void)c; (void)d;
    _Complex_float r = { 0, 0 };
    return r;
}
