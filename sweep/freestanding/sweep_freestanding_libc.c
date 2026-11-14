/* sweep_freestanding_libc.c - minimal freestanding C environment for Duktape.
 *
 * Compiled with -ffreestanding -nostdlib -fno-builtin, this file defines the
 * standard memory/string/math symbols Duktape references plus the two feature
 * hooks selected by duk_config_freestanding.h.  It performs no allocation and
 * makes no OS calls; the only non-portable operation is __builtin_trap() in the
 * fatal handler (defined behaviour for a freestanding target).
 *
 * The math routines are small, dependency-free approximations.  They are good
 * enough for the smoke test and for ordinary engine values, but they are NOT a
 * correctly-rounded libm.  See README.md for the exact accuracy caveat.
 */
#include "sweep_freestanding_libc.h"

#include <stddef.h>
#include <stdint.h>
#if defined(SWEEP_FS_KAPA)
/* The KAPA build routes the fatal reason to main.log through the restricted
 * KAPA gateway before trapping; the freestanding libc has no KAPA service
 * header of its own. */
#include "../../emergency/ow_gate.h"
#endif

/* Forward declarations for functions used before their definitions. */
double atan2(double y, double x);

/* ------------------------------------------------------------------------- */
/* Memory / string                                                           */
/* ------------------------------------------------------------------------- */

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*) dst;
    const unsigned char* s = (const unsigned char*) src;
    size_t i;
    for (i = 0u; i < n; ++i) d[i] = s[i];
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*) dst;
    const unsigned char* s = (const unsigned char*) src;
    if (d == s || n == 0u) return dst;
    if (d < s) {
        size_t i;
        for (i = 0u; i < n; ++i) d[i] = s[i];
    } else {
        size_t i = n;
        while (i > 0u) { --i; d[i] = s[i]; }
    }
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    unsigned char* d = (unsigned char*) dst;
    size_t i;
    for (i = 0u; i < n; ++i) d[i] = (unsigned char) c;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = (const unsigned char*) a;
    const unsigned char* y = (const unsigned char*) b;
    size_t i;
    for (i = 0u; i < n; ++i) {
        if (x[i] != y[i]) return (x[i] < y[i]) ? -1 : 1;
    }
    return 0;
}

size_t strlen(const char* s) {
    size_t n = 0u;
    while (s[n] != '\0') ++n;
    return n;
}

int strcmp(const char* a, const char* b) {
    size_t i = 0u;
    while (a[i] != '\0' && a[i] == b[i]) ++i;
    return (int) (unsigned char) a[i] - (int) (unsigned char) b[i];
}

int strncmp(const char* a, const char* b, size_t n) {
    size_t i;
    for (i = 0u; i < n; ++i) {
        if (a[i] != b[i] || a[i] == '\0') {
            return (int) (unsigned char) a[i] - (int) (unsigned char) b[i];
        }
    }
    return 0;
}

char* strcpy(char* dst, const char* src) {
    size_t i = 0u;
    do { dst[i] = src[i]; } while (src[i++] != '\0');
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    size_t i;
    for (i = 0u; i < n && src[i] != '\0'; ++i) dst[i] = src[i];
    for (; i < n; ++i) dst[i] = '\0';
    return dst;
}

char* strcat(char* dst, const char* src) {
    size_t d = strlen(dst);
    (void) strcpy(dst + d, src);
    return dst;
}

char* strchr(const char* s, int c) {
    char ch = (char) c;
    for (;; ++s) {
        if (*s == ch) return (char*) s;
        if (*s == '\0') return NULL;
    }
}

char* strrchr(const char* s, int c) {
    const char* last = NULL;
    char ch = (char) c;
    for (;; ++s) {
        if (*s == ch) last = s;
        if (*s == '\0') return (char*) last;
    }
}

char* strstr(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    if (n == 0u) return (char*) hay;
    for (; *hay != '\0'; ++hay) {
        if (*hay == needle[0] && strncmp(hay, needle, n) == 0) {
            return (char*) hay;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* ASCII character classification and case conversion.  These are ASCII-only
 * and deliberately incomplete: the freestanding engine never depends on a
 * locale, and expanding them into full Unicode semantics would be a Unicode
 * redesign, which is out of scope.  Anything outside ASCII is neither a
 * letter nor a digit nor whitespace here. */
/* ------------------------------------------------------------------------- */
int tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

int toupper(int c) {
    return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
}

int isalpha(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

int isdigit(int c) {
    return c >= '0' && c <= '9';
}

int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

int isctrl(int c) {
    return (c >= 0 && c < 0x20) || c == 0x7F;
}

/* ------------------------------------------------------------------------- */
/* Floating point helpers                                                    */
/* ------------------------------------------------------------------------- */

static const union { uint64_t u; double d; } S_FS_INF = { 0x7FF0000000000000ull };
static const union { uint64_t u; double d; } S_FS_NAN = { 0x7FF8000000000000ull };

#define FS_INF (S_FS_INF.d)
#define FS_NAN (S_FS_NAN.d)

static double fs_abs(double x) { return (x < 0.0) ? -x : x; }

static double fs_scale_pow2(double x, int e) {
    union { double d; uint64_t u; } v;
    int biased;
    if (x == 0.0 || e == 0) return x;
    if (e > 1023) return (x < 0.0) ? -FS_INF : FS_INF;
    if (e < -1074) return (x < 0.0) ? -0.0 : 0.0;
    v.d = x;
    biased = (int) ((v.u >> 52) & 0x7FFu);
    if (biased == 0x7FF) return x; /* inf/nan */
    if (biased != 0) {
        biased += e;
        if (biased >= 0x7FF) return (x < 0.0) ? -FS_INF : FS_INF;
        if (biased > 0) {
            v.u = (v.u & ~(0x7FFull << 52)) | ((uint64_t) biased << 52);
            return v.d;
        }
    }
    /* Subnormal / underflow: scale one power of two at a time so the exponent
     * field never needs to wrap.  Bounded by the IEEE-754 exponent range. */
    {
        double r = x;
        int k = e;
        while (k > 0) { r *= 2.0; --k; }
        while (k < 0) { r *= 0.5; ++k; }
        return r;
    }
}

double floor(double x) {
    double t;
    if (x != x || x == FS_INF || x == -FS_INF) return x;
    if (x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;
    t = (double) (int64_t) x;
    if (t > x) t -= 1.0;
    if (t == 0.0 && x < 0.0) return -0.0;
    return t;
}

double ceil(double x) {
    double t;
    if (x != x || x == FS_INF || x == -FS_INF) return x;
    if (x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;
    t = (double) (int64_t) x;
    if (t < x) t += 1.0;
    if (t == 0.0 && x > 0.0) return 0.0;
    return t;
}

double trunc(double x) {
    if (x != x || x == FS_INF || x == -FS_INF) return x;
    if (x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;
    return (double) (int64_t) x;
}

double fmod(double x, double y) {
    double q;
    if (y == 0.0 || x != x || y != y || x == FS_INF || x == -FS_INF) {
        return FS_NAN;
    }
    if (y == FS_INF || y == -FS_INF) return x;
    q = trunc(x / y);
    /* For very large quotients the subtraction loses all precision; reduce by
     * repeated scaling instead.  Duktape does not depend on exact libm fmod
     * for such values. */
    if (q >= 4503599627370496.0 || q <= -4503599627370496.0) {
        double ay = fs_abs(y);
        double ax = fs_abs(x);
        while (ax >= ay) ax -= ay;
        return (x < 0.0) ? -ax : ax;
    }
    return x - q * y;
}

double sqrt(double x) {
    double r, prev;
    int i;
    if (x != x || x < 0.0) return FS_NAN;
    if (x == 0.0 || x == FS_INF) return x;
    r = (x > 1.0) ? x : 1.0;
    for (i = 0; i < 64; ++i) {
        prev = r;
        r = 0.5 * (r + x / r);
        if (r == prev) break;
    }
    return r;
}

double exp(double x) {
    /* e^x = 2^k * e^r, r in [-ln2/2, ln2/2] */
    static const double LN2 = 0.69314718055994530942;
    static const double INV_LN2 = 1.4426950408889634074;
    double kd, r, term, sum;
    int k, i;
    if (x != x) return x;
    if (x == FS_INF) return FS_INF;
    if (x == -FS_INF) return 0.0;
    if (x > 709.782712893384) return FS_INF;
    if (x < -745.1332191019411) return 0.0;
    kd = floor(x * INV_LN2 + 0.5);
    k = (int) kd;
    r = x - kd * LN2;
    term = 1.0;
    sum = 1.0;
    for (i = 1; i < 24; ++i) {
        term *= r / (double) i;
        sum += term;
    }
    return fs_scale_pow2(sum, k);
}

double log(double x) {
    /* ln(x) = k*ln2 + 2*atanh((m-1)/(m+1)), m in [1,2) */
    static const double LN2 = 0.69314718055994530942;
    union { double d; uint64_t u; } v;
    double m, z, z2, term, sum;
    int e, i;
    if (x != x || x < 0.0) return FS_NAN;
    if (x == 0.0) return -FS_INF;
    if (x == FS_INF) return FS_INF;
    v.d = x;
    e = (int) ((v.u >> 52) & 0x7FFu) - 1023;
    if (e == -1023) {
        /* subnormal: normalize */
        v.d = x * 4503599627370496.0; /* 2^52 */
        e = (int) ((v.u >> 52) & 0x7FFu) - 1023 - 52;
    }
    v.u = (v.u & ~(0x7FFull << 52)) | (1023ull << 52); /* m in [1,2) */
    m = v.d;
    if (m > 1.4142135623730951) { m *= 0.5; e += 1; }
    z = (m - 1.0) / (m + 1.0);
    z2 = z * z;
    term = z;
    sum = 0.0;
    for (i = 1; i < 40; i += 2) {
        sum += term / (double) i;
        term *= z2;
    }
    return (double) e * LN2 + 2.0 * sum;
}

double pow(double x, double y) {
    double yi;
    if (y == 0.0) return 1.0;
    if (x == 0.0) {
        if (y < 0.0) return FS_INF;
        return (y == 0.0) ? 1.0 : 0.0;
    }
    if (x == 1.0) return 1.0;
    if (x != x || y != y) return FS_NAN;
    if (x < 0.0) {
        yi = trunc(y);
        if (yi != y) return FS_NAN;
        {
            double r = exp(y * log(-x));
            /* odd integer exponent keeps the sign */
            double half = trunc(yi / 2.0);
            if (half * 2.0 != yi) r = -r;
            return r;
        }
    }
    if (x == FS_INF) return (y > 0.0) ? FS_INF : 0.0;
    if (y == FS_INF) {
        if (x > 1.0) return FS_INF;
        if (x < 1.0) return 0.0;
        return 1.0;
    }
    return exp(y * log(x));
}

double sin(double x) {
    static const double PI2 = 6.28318530717958647692;
    static const double PI = 3.14159265358979323846;
    double r, term, sum;
    int i, sign = 1;
    if (x != x || x == FS_INF || x == -FS_INF) return FS_NAN;
    r = fmod(x, PI2);
    if (r > PI) r -= PI2;
    if (r < -PI) r += PI2;
    if (r < 0.0) { r = -r; sign = -1; }
    if (r > PI / 2.0) r = PI - r;
    term = r;
    sum = r;
    for (i = 1; i < 30; ++i) {
        term *= -(r * r) / ((double) (2 * i) * (double) (2 * i + 1));
        sum += term;
        if (term < 1e-18 && term > -1e-18) break;
    }
    return (sign < 0) ? -sum : sum;
}

double cos(double x) {
    return sin(x + 1.57079632679489661923);
}

double tan(double x) {
    double c = cos(x);
    if (c == 0.0) return FS_NAN;
    return sin(x) / c;
}

double atan(double x) {
    static const double PI = 3.14159265358979323846;
    int neg = 0;
    double r, r2, term, sum;
    int i;
    if (x != x) return x;
    if (x < 0.0) { x = -x; neg = 1; }
    if (x == FS_INF || x > 1.0e300) return neg ? -PI / 2.0 : PI / 2.0;
    if (x == 0.0) return 0.0;
    /* Half-angle reduction: atan(x) = 2*atan(x / (1 + sqrt(1+x^2))). */
    {
        int steps = 0;
        while (x > 0.25) {
            x = x / (1.0 + sqrt(1.0 + x * x));
            ++steps;
        }
        r = x;
        r2 = r * r;
        term = r;
        sum = r;
        for (i = 3; i < 60; i += 2) {
            term *= -r2;
            sum += term / (double) i;
        }
        while (steps-- > 0) sum *= 2.0;
    }
    return neg ? -sum : sum;
}

double asin(double x) {
    if (x != x || x > 1.0 || x < -1.0) return FS_NAN;
    if (x == 1.0) return 1.57079632679489661923;
    if (x == -1.0) return -1.57079632679489661923;
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x) {
    if (x != x || x > 1.0 || x < -1.0) return FS_NAN;
    return 1.57079632679489661923 - asin(x);
}

double atan2(double y, double x) {
    static const double PI = 3.14159265358979323846;
    if (x != x || y != y) return FS_NAN;
    if (x > 0.0) return atan(y / x);
    if (x < 0.0) {
        if (y >= 0.0) return atan(y / x) + PI;
        return atan(y / x) - PI;
    }
    if (y > 0.0) return PI / 2.0;
    if (y < 0.0) return -PI / 2.0;
    return 0.0;
}

/* ------------------------------------------------------------------------- */
/* Minimal printf/scanf surface used by Duktape diagnostics and error text.  */
/* Supports the format subset Duktape actually emits: %s %c %d %i %u %o %x  */
/* %X %p %f and %% with width/precision/length modifiers.                    */
/* ------------------------------------------------------------------------- */

#include <stdarg.h>

static int fs_is_digit(char c) { return c >= '0' && c <= '9'; }

static int fs_parse_uint(const char** sp, int base, unsigned long long* out) {
    const char* s = *sp;
    unsigned long long v = 0ull;
    int any = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (;;) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned long long) base + (unsigned long long) d;
        ++s;
        any = 1;
    }
    if (!any) return 0;
    *sp = s;
    *out = v;
    return 1;
}

int sscanf(const char* str, const char* fmt, ...);

static int fs_vsscanf(const char* str, const char* fmt, va_list ap) {
    const char* s = str;
    int assigned = 0;
    while (*fmt != '\0') {
        if (*fmt == ' ' || *fmt == '\t' || *fmt == '\n') { ++fmt; continue; }
        if (*fmt != '%') {
            if (*s != *fmt) break;
            ++s; ++fmt; continue;
        }
        ++fmt;
        if (*fmt == '%') {
            if (*s != '%') break;
            ++s; ++fmt; continue;
        }
        if (*fmt == 'p') {
            void** out = va_arg(ap, void**);
            unsigned long long v = 0ull;
            if (!fs_parse_uint(&s, 16, &v)) break;
            if (out) *out = (void*) (uintptr_t) v;
            ++assigned; ++fmt; continue;
        }
        if (*fmt == 'x' || *fmt == 'X' || *fmt == 'd' || *fmt == 'i' ||
            *fmt == 'u' || *fmt == 'o') {
            unsigned long long v = 0ull;
            int base = (*fmt == 'x' || *fmt == 'X') ? 16 : ((*fmt == 'o') ? 8 : 10);
            if (!fs_parse_uint(&s, base, &v)) break;
            if (*fmt == 'd' || *fmt == 'i') {
                int* out = va_arg(ap, int*);
                if (out) *out = (int) v;
            } else {
                unsigned int* out = va_arg(ap, unsigned int*);
                if (out) *out = (unsigned int) v;
            }
            ++assigned; ++fmt; continue;
        }
        if (*fmt == 's') {
            char* out = va_arg(ap, char*);
            while (*s == ' ' || *s == '\t' || *s == '\n') ++s;
            while (*s != '\0' && *s != ' ' && *s != '\t' && *s != '\n') {
                if (out) *out++ = *s;
                ++s;
            }
            if (out) *out = '\0';
            ++assigned; ++fmt; continue;
        }
        break;
    }
    return assigned;
}

int sscanf(const char* str, const char* fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = fs_vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}

typedef struct {
    char*  out;
    size_t cap;
    size_t pos;   /* characters that would have been written */
} FS_FMT;

static void fs_put(FS_FMT* f, char c) {
    if (f->out != NULL && f->pos + 1u < f->cap) f->out[f->pos] = c;
    ++f->pos;
}

static void fs_puts(FS_FMT* f, const char* s, size_t n) {
    size_t i;
    for (i = 0u; i < n; ++i) fs_put(f, s[i]);
}

static void fs_pad(FS_FMT* f, int n, char c) {
    while (n-- > 0) fs_put(f, c);
}

static void fs_format_int(FS_FMT* f, unsigned long long v, int neg,
                          int base, int upper, int width, int zero,
                          int left, int plus, int space, int precision) {
    char tmp[32];
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    int total;
    int sign_len = neg ? 1 : (plus ? 1 : (space ? 1 : 0));

    if (v == 0ull) {
        if (precision != 0) tmp[n++] = '0';
    } else {
        while (v > 0ull && n < (int) sizeof(tmp)) {
            tmp[n++] = digits[v % (unsigned long long) base];
            v /= (unsigned long long) base;
        }
    }
    total = n + sign_len;
    if (precision > n) total += precision - n;
    if (!left && !zero && width > total) fs_pad(f, width - total, ' ');
    if (neg) fs_put(f, '-');
    else if (plus) fs_put(f, '+');
    else if (space) fs_put(f, ' ');
    if (precision > n) fs_pad(f, precision - n, '0');
    if (!left && zero && width > total) fs_pad(f, width - total, '0');
    while (n > 0) fs_put(f, tmp[--n]);
    if (left && width > total) fs_pad(f, width - total, ' ');
}

static void fs_format_fixed(FS_FMT* f, double value, int precision,
                            int width, int left, int zero, int plus, int space) {
    char whole[32];
    char frac[32];
    int nw = 0, nf = 0, i, total;
    unsigned long long ip;
    double fracpart;
    int neg = 0;

    if (value != value) { fs_puts(f, "nan", 3u); return; }
    if (precision < 0) precision = 6;
    if (precision > 17) precision = 17;
    if (value < 0.0) { neg = 1; value = -value; }
    if (value > 9.0e18) { fs_puts(f, "inf", 3u); return; }
    ip = (unsigned long long) value;
    fracpart = value - (double) ip;
    if (ip == 0ull) whole[nw++] = '0';
    else while (ip > 0ull && nw < (int) sizeof(whole)) {
        whole[nw++] = (char) ('0' + (int) (ip % 10ull));
        ip /= 10ull;
    }
    for (i = 0; i < precision; ++i) {
        int d;
        fracpart *= 10.0;
        d = (int) fracpart;
        frac[nf] = (char) ('0' + d);
        ++nf;
        fracpart -= (double) d;
    }
    if (precision == 0 && fracpart >= 0.5) {
        /* round the integer part up */
        int carry = 1;
        for (i = 0; i < nw && carry; ++i) {
            int d = whole[i] - '0' + carry;
            whole[i] = (char) ('0' + (d % 10));
            carry = (d >= 10);
        }
        if (carry && nw < (int) sizeof(whole)) whole[nw++] = '1';
    }
    total = nw + (precision > 0 ? 1 + precision : 0) + (neg || plus || space ? 1 : 0);
    if (!left && !zero && width > total) fs_pad(f, width - total, ' ');
    if (neg) fs_put(f, '-');
    else if (plus) fs_put(f, '+');
    else if (space) fs_put(f, ' ');
    if (!left && zero && width > total) fs_pad(f, width - total, '0');
    while (nw > 0) fs_put(f, whole[--nw]);
    if (precision > 0) {
        fs_put(f, '.');
        for (i = 0; i < nf; ++i) fs_put(f, frac[i]);
    }
    if (left && width > total) fs_pad(f, width - total, ' ');
}

static int fs_vformat(char* out, size_t cap, const char* fmt, va_list ap) {
    FS_FMT f;
    f.out = out;
    f.cap = cap;
    f.pos = 0u;

    while (*fmt != '\0') {
        int left = 0, zero = 0, plus = 0, space = 0;
        int width = 0, precision = -1;
        int len_l = 0, len_ll = 0, len_h = 0;

        if (*fmt != '%') { fs_put(&f, *fmt++); continue; }
        ++fmt;

        while (*fmt == '-' || *fmt == '0' || *fmt == '+' || *fmt == ' ') {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else space = 1;
            ++fmt;
        }
        if (*fmt == '*') { width = va_arg(ap, int); ++fmt; }
        else while (fs_is_digit(*fmt)) { width = width * 10 + (*fmt - '0'); ++fmt; }
        if (*fmt == '.') {
            ++fmt;
            precision = 0;
            if (*fmt == '*') { precision = va_arg(ap, int); ++fmt; }
            else while (fs_is_digit(*fmt)) { precision = precision * 10 + (*fmt - '0'); ++fmt; }
        }
        if (*fmt == 'h') { len_h = 1; ++fmt; if (*fmt == 'h') ++fmt; }
        else if (*fmt == 'l') { ++fmt; if (*fmt == 'l') { len_ll = 1; ++fmt; } else len_l = 1; }
        else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') { len_l = 1; ++fmt; }

        switch (*fmt) {
        case '%': fs_put(&f, '%'); ++fmt; break;
        case 'c': {
            char ch = (char) va_arg(ap, int);
            fs_put(&f, ch);
            ++fmt; break;
        }
        case 's': {
            const char* s = va_arg(ap, const char*);
            size_t n;
            if (s == NULL) s = "(null)";
            n = strlen(s);
            if (precision >= 0 && (size_t) precision < n) n = (size_t) precision;
            if (!left && width > (int) n) fs_pad(&f, width - (int) n, ' ');
            fs_puts(&f, s, n);
            if (left && width > (int) n) fs_pad(&f, width - (int) n, ' ');
            ++fmt; break;
        }
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            unsigned long long v;
            int neg = 0;
            int base = (*fmt == 'o') ? 8 : ((*fmt == 'x' || *fmt == 'X') ? 16 : 10);
            if (*fmt == 'd' || *fmt == 'i') {
                long long sv;
                if (len_ll) sv = va_arg(ap, long long);
                else if (len_l) sv = (long long) va_arg(ap, long);
                else sv = (long long) va_arg(ap, int);
                neg = (sv < 0);
                v = neg ? (unsigned long long) (-(sv + 1)) + 1ull
                        : (unsigned long long) sv;
            } else {
                if (len_ll) v = va_arg(ap, unsigned long long);
                else if (len_l) v = (unsigned long long) va_arg(ap, unsigned long);
                else v = (unsigned long long) va_arg(ap, unsigned int);
                if (len_h) v = (unsigned long long) (unsigned short) v;
            }
            fs_format_int(&f, v, neg, base, *fmt == 'X', width, zero, left,
                          plus, space, precision);
            ++fmt; break;
        }
        case 'p': {
            void* p = va_arg(ap, void*);
            fs_puts(&f, "0x", 2u);
            fs_format_int(&f, (unsigned long long) (uintptr_t) p, 0, 16, 0,
                          width, zero, left, 0, 0, precision);
            ++fmt; break;
        }
        case 'f': case 'F': {
            double d = va_arg(ap, double);
            fs_format_fixed(&f, d, precision, width, left, zero, plus, space);
            ++fmt; break;
        }
        case '\0': fs_put(&f, '%'); break;
        default: fs_put(&f, '%'); fs_put(&f, *fmt); ++fmt; break;
        }
    }
    if (f.out != NULL && f.cap > 0u) {
        size_t at = (f.pos < f.cap) ? f.pos : f.cap - 1u;
        f.out[at] = '\0';
    }
    return (int) f.pos;
}

int vsnprintf(char* out, size_t cap, const char* fmt, va_list ap) {
    return fs_vformat(out, cap, fmt, ap);
}

int snprintf(char* out, size_t cap, const char* fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = fs_vformat(out, cap, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char* out, const char* fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = fs_vformat(out, (size_t) -1, fmt, ap);
    va_end(ap);
    return r;
}

/* MinGW headers alias the CRT entry points to these names. */
int __mingw_vsnprintf(char* out, size_t cap, const char* fmt, va_list ap) {
    return fs_vformat(out, cap, fmt, ap);
}

int __mingw_snprintf(char* out, size_t cap, const char* fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = fs_vformat(out, cap, fmt, ap);
    va_end(ap);
    return r;
}

int __mingw_sprintf(char* out, const char* fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = fs_vformat(out, (size_t) -1, fmt, ap);
    va_end(ap);
    return r;
}

int __mingw_sscanf(const char* str, const char* fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = fs_vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------------------------- */
/* Duktape hooks                                                             */
/* ------------------------------------------------------------------------- */

double sweep_fs_date_now_ms(void) {
    /* No wall clock in a freestanding target.  Returning 0 keeps Date and
     * performance.now deterministic; the embedding backend can replace this
     * symbol with a real tick source. */
    return 0.0;
}

void sweep_fs_fatal(void* udata, const char* msg) {
    volatile void* keep;
    (void) msg;
    keep = udata;
    (void) keep;
#if defined(SWEEP_FS_KAPA)
    /* The KAPA build surfaces the fatal reason through the restricted Service
     * Bridge (main.log) instead of trapping mute.  The message is copied to a
     * bounded local buffer before use and the bytes are sent as data. */
    {
        OW_SYS_KDB_BRIDGE_CALL call;
        char buf[96];
        unsigned long i = 0UL;
        const char* m = msg;
        const char* tag = (const char*) "kapa-sweep: duktape fatal: ";

        while (tag[i] != '\0' && i + 1u < sizeof(buf)) {
            buf[i] = tag[i];
            ++i;
        }
        while (m && *m != '\0' && i + 1u < sizeof(buf)) {
            buf[i] = *m;
            ++i;
            ++m;
        }
        buf[i] = '\0';
        call.Magic = OW_SYS_KDB_BRIDGE_MAGIC;
        call.Version = OW_SYS_KDB_BRIDGE_VERSION;
        call.ServiceId = OW_KDB_SERVICE_LOG;
        call.Flags = 0U;
        call.RequestLength = (uint32_t) i;
        call.ResponseCapacity = 0U;
        call.ResponseLength = 0U;
        call.TargetInstanceId = 0U;
        call.RequestAddress = (uint64_t) (uintptr_t) buf;
        call.ResponseAddress = 0U;
        call.Reserved = 0U;
        (void) ow_gate_bridge(&call);
    }
#endif
    __builtin_trap();
}

int sweep_js_engine_timeout_check(void* udata) {
    (void) udata;
    return 0;
}
