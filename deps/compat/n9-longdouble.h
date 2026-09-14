/* n9-longdouble.h -- force-included into every cross build (see deps/toolchain.sh).
 *
 * On this target long double == double, so the Harmattan sysroot's
 * bits/mathdef.h defines __NO_LONG_DOUBLE_MATH, and glibc 2.10's math.h then
 * suppresses the declaration of EVERY "long double" math function.
 *
 * The symbols are all still present in libm as weak aliases of their double
 * counterparts (verified: frexpl and frexp share address 0x411fa620), so the
 * only thing missing is the prototype. gnulib-based configure checks -- GLib's
 * among them -- probe with a declaration test and conclude the function is
 * "missing or broken beyond repair", which hard-errors the build.
 *
 * glibc offers __LDBL_COMPAT to re-enable these declarations, but defining it
 * drags in bits/libio-ldbl.h's __LDBL_REDIR_DECL machinery, which this target
 * never configured -- that path fails to even parse. So declare them directly.
 *
 * Every prototype below was checked against the sysroot's libm exports.
 *
 * This header MUST NOT include any system header. It is force-included ahead of
 * every translation unit, so including <math.h> here would drag in <features.h>
 * before a source file gets to set its own feature-test macros -- which silently
 * changes which variants of functions glibc declares. (Observed: it flipped
 * strerror_r from the GNU char*-returning form to the XSI int-returning one and
 * broke GLib's detection of it.)
 *
 * So the guard uses GCC's own predefined macros instead. __LDBL_MANT_DIG__ ==
 * __DBL_MANT_DIG__ (both 53 here) is precisely the condition under which glibc's
 * bits/mathdef.h defines __NO_LONG_DOUBLE_MATH, so the two agree without our
 * needing to see glibc's headers at all.
 */
#ifndef N9_LONGDOUBLE_H
#define N9_LONGDOUBLE_H

/* -include applies to .S sources too (they run through the preprocessor), and
 * the assembler cannot parse C declarations -- libpng's arm/filter_neon.S is
 * the first of many. __ASSEMBLER__ is set only on that path. */
#ifndef __ASSEMBLER__

#if defined(__LDBL_MANT_DIG__) && defined(__DBL_MANT_DIG__) && \
    __LDBL_MANT_DIG__ == __DBL_MANT_DIG__

#if defined(__cplusplus)
extern "C" {
#endif

/* one long double in, one out */
extern long double acosl(long double);   extern long double asinl(long double);
extern long double atanl(long double);   extern long double cosl(long double);
extern long double sinl(long double);    extern long double tanl(long double);
extern long double acoshl(long double);  extern long double asinhl(long double);
extern long double atanhl(long double);  extern long double coshl(long double);
extern long double sinhl(long double);   extern long double tanhl(long double);
extern long double expl(long double);    extern long double exp2l(long double);
extern long double expm1l(long double);  extern long double logl(long double);
extern long double log10l(long double);  extern long double log1pl(long double);
extern long double log2l(long double);   extern long double logbl(long double);
extern long double cbrtl(long double);   extern long double fabsl(long double);
extern long double sqrtl(long double);   extern long double erfl(long double);
extern long double erfcl(long double);   extern long double lgammal(long double);
extern long double tgammal(long double); extern long double ceill(long double);
extern long double floorl(long double);  extern long double nearbyintl(long double);
extern long double rintl(long double);   extern long double roundl(long double);
extern long double truncl(long double);

/* two long doubles in */
extern long double atan2l(long double, long double);
extern long double hypotl(long double, long double);
extern long double powl(long double, long double);
extern long double fmodl(long double, long double);
extern long double remainderl(long double, long double);
extern long double copysignl(long double, long double);
extern long double nextafterl(long double, long double);
extern long double nexttowardl(long double, long double);
extern long double fdiml(long double, long double);
extern long double fmaxl(long double, long double);
extern long double fminl(long double, long double);
extern long double fmal(long double, long double, long double);

/* mixed / out-parameter / differing return type */
extern long double frexpl(long double, int *);
extern long double ldexpl(long double, int);
extern long double modfl(long double, long double *);
extern long double scalbnl(long double, int);
extern long double scalblnl(long double, long);
extern long double remquol(long double, long double, int *);
extern long double nanl(const char *);
extern int         ilogbl(long double);
extern long        lrintl(long double);
extern long long   llrintl(long double);
extern long        lroundl(long double);
extern long long   llroundl(long double);

#if defined(__cplusplus)
}
#endif

#endif /* long double == double */

#endif /* !__ASSEMBLER__ */
#endif /* N9_LONGDOUBLE_H */
