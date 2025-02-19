/*
 * msvcrt.dll math functions
 *
 * Copyright 2000 Jon Griffiths
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 *
 * For functions copied from musl libc (http://musl.libc.org/):
 * ====================================================
 * Copyright 2005-2020 Rich Felker, et al.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * ====================================================
 */

#include <complex.h>
#include <stdio.h>
#include <fenv.h>
#include <fpieee.h>
#include <limits.h>
#include <locale.h>
#include <math.h>

#include "msvcrt.h"
#include "winternl.h"
#include "unixlib.h"

#include "wine/asm.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(msvcrt);

#undef div
#undef ldiv

#define _DOMAIN         1       /* domain error in argument */
#define _SING           2       /* singularity */
#define _OVERFLOW       3       /* range overflow */
#define _UNDERFLOW      4       /* range underflow */

typedef int (CDECL *MSVCRT_matherr_func)(struct _exception *);

static MSVCRT_matherr_func MSVCRT_default_matherr_func = NULL;

BOOL sse2_supported;
static BOOL sse2_enabled;

static const struct unix_funcs *unix_funcs;

void msvcrt_init_math( void *module )
{
    sse2_supported = IsProcessorFeaturePresent( PF_XMMI64_INSTRUCTIONS_AVAILABLE );
#if _MSVCR_VER <=71
    sse2_enabled = FALSE;
#else
    sse2_enabled = sse2_supported;
#endif
    __wine_init_unix_lib( module, DLL_PROCESS_ATTACH, NULL, &unix_funcs );
}

/* Copied from musl: src/internal/libm.h */
static inline float fp_barrierf(float x)
{
    volatile float y = x;
    return y;
}

static inline double CDECL ret_nan( BOOL update_sw )
{
    double x = 1.0;
    if (!update_sw) return -NAN;
    return (x - x) / (x - x);
}

#define SET_X87_CW(MASK) \
    "subl $4, %esp\n\t" \
    __ASM_CFI(".cfi_adjust_cfa_offset 4\n\t") \
    "fnstcw (%esp)\n\t" \
    "movw (%esp), %ax\n\t" \
    "movw %ax, 2(%esp)\n\t" \
    "testw $" #MASK ", %ax\n\t" \
    "jz 1f\n\t" \
    "andw $~" #MASK ", %ax\n\t" \
    "movw %ax, 2(%esp)\n\t" \
    "fldcw 2(%esp)\n\t" \
    "1:\n\t"

#define RESET_X87_CW \
    "movw (%esp), %ax\n\t" \
    "cmpw %ax, 2(%esp)\n\t" \
    "je 1f\n\t" \
    "fstpl 8(%esp)\n\t" \
    "fldcw (%esp)\n\t" \
    "fldl 8(%esp)\n\t" \
    "fwait\n\t" \
    "1:\n\t" \
    "addl $4, %esp\n\t" \
    __ASM_CFI(".cfi_adjust_cfa_offset -4\n\t")

/*********************************************************************
 *      _matherr (CRTDLL.@)
 */
int CDECL _matherr(struct _exception *e)
{
    return 0;
}


static double math_error(int type, const char *name, double arg1, double arg2, double retval)
{
    struct _exception exception = {type, (char *)name, arg1, arg2, retval};

    TRACE("(%d, %s, %g, %g, %g)\n", type, debugstr_a(name), arg1, arg2, retval);

    if (MSVCRT_default_matherr_func && MSVCRT_default_matherr_func(&exception))
        return exception.retval;

    switch (type)
    {
    case 0:
        /* don't set errno */
        break;
    case _DOMAIN:
        *_errno() = EDOM;
        break;
    case _SING:
    case _OVERFLOW:
        *_errno() = ERANGE;
        break;
    case _UNDERFLOW:
        /* don't set errno */
        break;
    default:
        ERR("Unhandled math error!\n");
    }

    return exception.retval;
}

/*********************************************************************
 *      __setusermatherr (MSVCRT.@)
 */
void CDECL __setusermatherr(MSVCRT_matherr_func func)
{
    MSVCRT_default_matherr_func = func;
    TRACE("new matherr handler %p\n", func);
}

/*********************************************************************
 *      _set_SSE2_enable (MSVCRT.@)
 */
int CDECL _set_SSE2_enable(int flag)
{
    sse2_enabled = flag && sse2_supported;
    return sse2_enabled;
}

#if defined(_WIN64)
# if _MSVCR_VER>=140
/*********************************************************************
 *      _get_FMA3_enable (UCRTBASE.@)
 */
int CDECL _get_FMA3_enable(void)
{
    FIXME("() stub\n");
    return 0;
}
# endif

# if _MSVCR_VER>=120
/*********************************************************************
 *      _set_FMA3_enable (MSVCR120.@)
 */
int CDECL _set_FMA3_enable(int flag)
{
    FIXME("(%x) stub\n", flag);
    return 0;
}
# endif
#endif

#if !defined(__i386__) || _MSVCR_VER>=120

/*********************************************************************
 *      _chgsignf (MSVCRT.@)
 */
float CDECL _chgsignf( float num )
{
    union { float f; UINT32 i; } u = { num };
    u.i ^= 0x80000000;
    return u.f;
}

/*********************************************************************
 *      _copysignf (MSVCRT.@)
 *
 * Copied from musl: src/math/copysignf.c
 */
float CDECL _copysignf( float x, float y )
{
    union { float f; UINT32 i; } ux = { x }, uy = { y };
    ux.i &= 0x7fffffff;
    ux.i |= uy.i & 0x80000000;
    return ux.f;
}

/*********************************************************************
 *      _nextafterf (MSVCRT.@)
 */
float CDECL _nextafterf( float num, float next )
{
    if (!isfinite(num) || !isfinite(next)) *_errno() = EDOM;
    return unix_funcs->nextafterf( num, next );
}

/*********************************************************************
 *      _logbf (MSVCRT.@)
 */
float CDECL _logbf( float num )
{
    float ret = unix_funcs->logbf(num);
    if (isnan(num)) return math_error(_DOMAIN, "_logbf", num, 0, ret);
    if (!num) return math_error(_SING, "_logbf", num, 0, ret);
    return ret;
}

#endif

#ifndef __i386__

/*********************************************************************
 *      _fpclassf (MSVCRT.@)
 */
int CDECL _fpclassf( float num )
{
    union { float f; UINT32 i; } u = { num };
    int e = u.i >> 23 & 0xff;
    int s = u.i >> 31;

    switch (e)
    {
    case 0:
        if (u.i << 1) return s ? _FPCLASS_ND : _FPCLASS_PD;
        return s ? _FPCLASS_NZ : _FPCLASS_PZ;
    case 0xff:
        if (u.i << 9) return ((u.i >> 22) & 1) ? _FPCLASS_QNAN : _FPCLASS_SNAN;
        return s ? _FPCLASS_NINF : _FPCLASS_PINF;
    default:
        return s ? _FPCLASS_NN : _FPCLASS_PN;
    }
}

/*********************************************************************
 *      _finitef (MSVCRT.@)
 */
int CDECL _finitef( float num )
{
    union { float f; UINT32 i; } u = { num };
    return (u.i & 0x7fffffff) < 0x7f800000;
}

/*********************************************************************
 *      _isnanf (MSVCRT.@)
 */
int CDECL _isnanf( float num )
{
    union { float f; UINT32 i; } u = { num };
    return (u.i & 0x7fffffff) > 0x7f800000;
}

static float asinf_R(float z)
{
    /* coefficients for R(x^2) */
    static const float p1 = 1.66666672e-01,
                 p2 = -5.11644611e-02,
                 p3 = -1.21124933e-02,
                 p4 = -3.58742251e-03,
                 q1 = -7.56982703e-01;

    float p, q;
    p = z * (p1 + z * (p2 + z * (p3 + z * p4)));
    q = 1.0f + z * q1;
    return p / q;
}

/*********************************************************************
 *      acosf (MSVCRT.@)
 *
 * Copied from musl: src/math/acosf.c
 */
float CDECL acosf( float x )
{
    static const double pio2_lo = 6.12323399573676603587e-17;

    float z, w, s, c, df;
    unsigned int hx, ix;

    hx = *(unsigned int*)&x;
    ix = hx & 0x7fffffff;
    /* |x| >= 1 or nan */
    if (ix >= 0x3f800000) {
        if (ix == 0x3f800000) {
            if (hx >> 31)
                return M_PI;
            return 0;
        }
        if (isnan(x)) return x;
        return math_error(_DOMAIN, "acosf", x, 0, 0 / (x - x));
    }
    /* |x| < 0.5 */
    if (ix < 0x3f000000) {
        if (ix <= 0x32800000) /* |x| < 2**-26 */
            return M_PI_2;
        return M_PI_2 - (x - (pio2_lo - x * asinf_R(x * x)));
    }
    /* x < -0.5 */
    if (hx >> 31) {
        z = (1 + x) * 0.5f;
        s = sqrtf(z);
        return M_PI - 2 * (s + ((double)s * asinf_R(z)));
    }
    /* x > 0.5 */
    z = (1 - x) * 0.5f;
    s = sqrtf(z);
    hx = *(unsigned int*)&s & 0xffff0000;
    df = *(float*)&hx;
    c = (z - df * df) / (s + df);
    w = asinf_R(z) * s + c;
    return 2 * (df + w);
}

/*********************************************************************
 *      asinf (MSVCRT.@)
 *
 * Copied from musl: src/math/asinf.c
 */
float CDECL asinf( float x )
{
    static const double pio2 = 1.570796326794896558e+00;
    static const float pio4_hi = 0.785398125648;
    static const float pio2_lo = 7.54978941586e-08;

    float s, z, f, c;
    unsigned int hx, ix;

    hx = *(unsigned int*)&x;
    ix = hx & 0x7fffffff;
    if (ix >= 0x3f800000) {  /* |x| >= 1 */
        if (ix == 0x3f800000)  /* |x| == 1 */
            return x * pio2 + 7.5231638453e-37;  /* asin(+-1) = +-pi/2 with inexact */
        if (isnan(x)) return x;
        return math_error(_DOMAIN, "asinf", x, 0, 0 / (x - x));
    }
    if (ix < 0x3f000000) {  /* |x| < 0.5 */
        /* if 0x1p-126 <= |x| < 0x1p-12, avoid raising underflow */
        if (ix < 0x39800000 && ix >= 0x00800000)
            return x;
        return x + x * asinf_R(x * x);
    }
    /* 1 > |x| >= 0.5 */
    z = (1 - fabsf(x)) * 0.5f;
    s = sqrtf(z);
    /* f+c = sqrt(z) */
    *(unsigned int*)&f = *(unsigned int*)&s & 0xffff0000;
    c = (z - f * f) / (s + f);
    x = pio4_hi - (2 * s * asinf_R(z) - (pio2_lo - 2 * c) - (pio4_hi - 2 * f));
    if (hx >> 31)
        return -x;
    return x;
}

/*********************************************************************
 *      atanf (MSVCRT.@)
 *
 * Copied from musl: src/math/atanf.c
 */
float CDECL atanf( float x )
{
    static const float atanhi[] = {
        4.6364760399e-01,
        7.8539812565e-01,
        9.8279368877e-01,
        1.5707962513e+00,
    };
    static const float atanlo[] = {
        5.0121582440e-09,
        3.7748947079e-08,
        3.4473217170e-08,
        7.5497894159e-08,
    };
    static const float aT[] = {
        3.3333328366e-01,
        -1.9999158382e-01,
        1.4253635705e-01,
        -1.0648017377e-01,
        6.1687607318e-02,
    };

    float w, s1, s2, z;
    unsigned int ix, sign;
    int id;

#if _MSVCR_VER == 0
    if (isnan(x)) return math_error(_DOMAIN, "atanf", x, 0, x);
#endif

    ix = *(unsigned int*)&x;
    sign = ix >> 31;
    ix &= 0x7fffffff;
    if (ix >= 0x4c800000) {  /* if |x| >= 2**26 */
        if (isnan(x))
            return x;
        z = atanhi[3] + 7.5231638453e-37;
        return sign ? -z : z;
    }
    if (ix < 0x3ee00000) {   /* |x| < 0.4375 */
        if (ix < 0x39800000) {  /* |x| < 2**-12 */
            if (ix < 0x00800000)
                /* raise underflow for subnormal x */
                fp_barrierf(x*x);
            return x;
        }
        id = -1;
    } else {
        x = fabsf(x);
        if (ix < 0x3f980000) {  /* |x| < 1.1875 */
            if (ix < 0x3f300000) {  /*  7/16 <= |x| < 11/16 */
                id = 0;
                x = (2.0f * x - 1.0f) / (2.0f + x);
            } else {                /* 11/16 <= |x| < 19/16 */
                id = 1;
                x = (x - 1.0f) / (x + 1.0f);
            }
        } else {
            if (ix < 0x401c0000) {  /* |x| < 2.4375 */
                id = 2;
                x = (x - 1.5f) / (1.0f + 1.5f * x);
            } else {                /* 2.4375 <= |x| < 2**26 */
                id = 3;
                x = -1.0f / x;
            }
        }
    }
    /* end of argument reduction */
    z = x * x;
    w = z * z;
    /* break sum from i=0 to 10 aT[i]z**(i+1) into odd and even poly */
    s1 = z * (aT[0] + w * (aT[2] + w * aT[4]));
    s2 = w * (aT[1] + w * aT[3]);
    if (id < 0)
        return x - x * (s1 + s2);
    z = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
    return sign ? -z : z;
}

/*********************************************************************
 *              atan2f (MSVCRT.@)
 *
 * Copied from musl: src/math/atan2f.c
 */
float CDECL atan2f( float y, float x )
{
    static const float pi     = 3.1415927410e+00,
                 pi_lo  = -8.7422776573e-08;

    float z;
    unsigned int m, ix, iy;

    if (isnan(x) || isnan(y))
        return x + y;
    ix = *(unsigned int*)&x;
    iy = *(unsigned int*)&y;
    if (ix == 0x3f800000)  /* x=1.0 */
        return atanf(y);
    m = ((iy >> 31) & 1) | ((ix >> 30) & 2);  /* 2*sign(x)+sign(y) */
    ix &= 0x7fffffff;
    iy &= 0x7fffffff;

    /* when y = 0 */
    if (iy == 0) {
        switch (m) {
        case 0:
        case 1: return y;   /* atan(+-0,+anything)=+-0 */
        case 2: return pi;  /* atan(+0,-anything) = pi */
        case 3: return -pi; /* atan(-0,-anything) =-pi */
        }
    }
    /* when x = 0 */
    if (ix == 0)
        return m & 1 ? -pi / 2 : pi / 2;
    /* when x is INF */
    if (ix == 0x7f800000) {
        if (iy == 0x7f800000) {
            switch (m) {
            case 0: return pi / 4;      /* atan(+INF,+INF) */
            case 1: return -pi / 4;     /* atan(-INF,+INF) */
            case 2: return 3 * pi / 4;  /*atan(+INF,-INF)*/
            case 3: return -3 * pi / 4; /*atan(-INF,-INF)*/
            }
        } else {
            switch (m) {
            case 0: return 0.0f;    /* atan(+...,+INF) */
            case 1: return -0.0f;   /* atan(-...,+INF) */
            case 2: return pi;      /* atan(+...,-INF) */
            case 3: return -pi;     /* atan(-...,-INF) */
            }
        }
    }
    /* |y/x| > 0x1p26 */
    if (ix + (26 << 23) < iy || iy == 0x7f800000)
        return m & 1 ? -pi / 2 : pi / 2;

    /* z = atan(|y/x|) with correct underflow */
    if ((m & 2) && iy + (26 << 23) < ix)  /*|y/x| < 0x1p-26, x < 0 */
        z = 0.0;
    else
        z = atanf(fabsf(y / x));
    switch (m) {
    case 0: return z;                /* atan(+,+) */
    case 1: return -z;               /* atan(-,+) */
    case 2: return pi - (z - pi_lo); /* atan(+,-) */
    default: /* case 3 */
        return (z - pi_lo) - pi;     /* atan(-,-) */
    }
}

/*********************************************************************
 *      cosf (MSVCRT.@)
 */
float CDECL cosf( float x )
{
  float ret = unix_funcs->cosf( x );
  if (!isfinite(x)) return math_error(_DOMAIN, "cosf", x, 0, ret);
  return ret;
}

/*********************************************************************
 *      coshf (MSVCRT.@)
 */
float CDECL coshf( float x )
{
  float ret = unix_funcs->coshf( x );
  if (isnan(x)) return math_error(_DOMAIN, "coshf", x, 0, ret);
  return ret;
}

/*********************************************************************
 *      expf (MSVCRT.@)
 */
float CDECL expf( float x )
{
  float ret = unix_funcs->expf( x );
  if (isnan(x)) return math_error(_DOMAIN, "expf", x, 0, ret);
  if (isfinite(x) && !ret) return math_error(_UNDERFLOW, "expf", x, 0, ret);
  if (isfinite(x) && !isfinite(ret)) return math_error(_OVERFLOW, "expf", x, 0, ret);
  return ret;
}

/*********************************************************************
 *      fmodf (MSVCRT.@)
 */
float CDECL fmodf( float x, float y )
{
  float ret = unix_funcs->fmodf( x, y );
  if (!isfinite(x) || !isfinite(y)) return math_error(_DOMAIN, "fmodf", x, 0, ret);
  return ret;
}

/*********************************************************************
 *      logf (MSVCRT.@)
 */
float CDECL logf( float x )
{
  float ret = unix_funcs->logf( x );
  if (x < 0.0) return math_error(_DOMAIN, "logf", x, 0, ret);
  if (x == 0.0) return math_error(_SING, "logf", x, 0, ret);
  return ret;
}

/*********************************************************************
 *      log10f (MSVCRT.@)
 */
float CDECL log10f( float x )
{
  float ret = unix_funcs->log10f( x );
  if (x < 0.0) return math_error(_DOMAIN, "log10f", x, 0, ret);
  if (x == 0.0) return math_error(_SING, "log10f", x, 0, ret);
  return ret;
}

/*********************************************************************
 *      powf (MSVCRT.@)
 */
float CDECL powf( float x, float y )
{
  float z = unix_funcs->powf(x,y);
  if (x < 0 && y != floorf(y)) return math_error(_DOMAIN, "powf", x, y, z);
  if (!x && isfinite(y) && y < 0) return math_error(_SING, "powf", x, y, z);
  if (isfinite(x) && isfinite(y) && !isfinite(z)) return math_error(_OVERFLOW, "powf", x, y, z);
  if (x && isfinite(x) && isfinite(y) && !z) return math_error(_UNDERFLOW, "powf", x, y, z);
  return z;
}

/*********************************************************************
 *      sinf (MSVCRT.@)
 */
float CDECL sinf( float x )
{
  float ret = unix_funcs->sinf( x );
  if (!isfinite(x)) return math_error(_DOMAIN, "sinf", x, 0, ret);
  return ret;
}

/*********************************************************************
 *      sinhf (MSVCRT.@)
 */
float CDECL sinhf( float x )
{
  float ret = unix_funcs->sinhf( x );
  if (isnan(x)) return math_error(_DOMAIN, "sinhf", x, 0, ret);
  return ret;
}

static BOOL sqrtf_validate( float *x )
{
    short c = _fdclass(*x);

    if (c == FP_ZERO) return FALSE;
    if (c == FP_NAN) return FALSE;
    if (signbit(*x))
    {
        *x = math_error(_DOMAIN, "sqrtf", *x, 0, ret_nan(TRUE));
        return FALSE;
    }
    if (c == FP_INFINITE) return FALSE;
    return TRUE;
}

#if defined(__x86_64__) || defined(__i386__)
float CDECL sse2_sqrtf(float);
__ASM_GLOBAL_FUNC( sse2_sqrtf,
        "sqrtss %xmm0, %xmm0\n\t"
        "ret" )
#endif

/*********************************************************************
 *      sqrtf (MSVCRT.@)
 *
 * Copied from musl: src/math/sqrtf.c
 */
float CDECL sqrtf( float x )
{
#ifdef __x86_64__
    if (!sqrtf_validate(&x))
        return x;

    return sse2_sqrtf(x);
#else
    static const float tiny = 1.0e-30;

    float z;
    int ix,s,q,m,t,i;
    unsigned int r;

    ix = *(int*)&x;

    if (!sqrtf_validate(&x))
        return x;

    /* normalize x */
    m = ix >> 23;
    if (m == 0) {  /* subnormal x */
        for (i = 0; (ix & 0x00800000) == 0; i++)
            ix <<= 1;
        m -= i - 1;
    }
    m -= 127;  /* unbias exponent */
    ix = (ix & 0x007fffff) | 0x00800000;
    if (m & 1)  /* odd m, double x to make it even */
        ix += ix;
    m >>= 1;  /* m = [m/2] */

    /* generate sqrt(x) bit by bit */
    ix += ix;
    q = s = 0;       /* q = sqrt(x) */
    r = 0x01000000;  /* r = moving bit from right to left */

    while (r != 0) {
        t = s + r;
        if (t <= ix) {
            s = t + r;
            ix -= t;
            q += r;
        }
        ix += ix;
        r >>= 1;
    }

    /* use floating add to find out rounding direction */
    if (ix != 0) {
        z = 1.0f - tiny; /* raise inexact flag */
        if (z >= 1.0f) {
            z = 1.0f + tiny;
            if (z > 1.0f)
                q += 2;
            else
                q += q & 1;
        }
    }
    ix = (q >> 1) + 0x3f000000;
    r = ix + ((unsigned int)m << 23);
    z = *(float*)&r;
    return z;
#endif
}

/*********************************************************************
 *      tanf (MSVCRT.@)
 */
float CDECL tanf( float x )
{
  float ret = unix_funcs->tanf(x);
  if (!isfinite(x)) return math_error(_DOMAIN, "tanf", x, 0, ret);
  return ret;
}

/*********************************************************************
 *      tanhf (MSVCRT.@)
 */
float CDECL tanhf( float x )
{
  float ret = unix_funcs->tanhf(x);
  if (!isfinite(x)) return math_error(_DOMAIN, "tanhf", x, 0, ret);
  return ret;
}

/*********************************************************************
 *      ceilf (MSVCRT.@)
 */
float CDECL ceilf( float x )
{
  return unix_funcs->ceilf(x);
}

/*********************************************************************
 *      floorf (MSVCRT.@)
 */
float CDECL floorf( float x )
{
  return unix_funcs->floorf(x);
}

/*********************************************************************
 *      frexpf (MSVCRT.@)
 */
float CDECL frexpf( float x, int *exp )
{
  return unix_funcs->frexpf( x, exp );
}

/*********************************************************************
 *      modff (MSVCRT.@)
 */
float CDECL modff( float x, float *iptr )
{
  return unix_funcs->modff( x, iptr );
}

#endif

#if !defined(__i386__) && !defined(__x86_64__) && (_MSVCR_VER == 0 || _MSVCR_VER >= 110)

/*********************************************************************
 *      fabsf (MSVCRT.@)
 *
 * Copied from musl: src/math/fabsf.c
 */
float CDECL fabsf( float x )
{
    union { float f; UINT32 i; } u = { x };
    u.i &= 0x7fffffff;
    return u.f;
}

#endif

/*********************************************************************
 *		acos (MSVCRT.@)
 *
 * Copied from musl: src/math/acos.c
 */
static double acos_R(double z)
{
    static const double pS0 =  1.66666666666666657415e-01,
                 pS1 = -3.25565818622400915405e-01,
                 pS2 =  2.01212532134862925881e-01,
                 pS3 = -4.00555345006794114027e-02,
                 pS4 =  7.91534994289814532176e-04,
                 pS5 =  3.47933107596021167570e-05,
                 qS1 = -2.40339491173441421878e+00,
                 qS2 =  2.02094576023350569471e+00,
                 qS3 = -6.88283971605453293030e-01,
                 qS4 =  7.70381505559019352791e-02;

    double p, q;
    p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
    q = 1.0 + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
    return p/q;
}

double CDECL acos( double x )
{
    static const double pio2_hi = 1.57079632679489655800e+00,
                 pio2_lo = 6.12323399573676603587e-17;

    double z, w, s, c, df;
    unsigned int hx, ix;
    ULONGLONG llx;

    hx = *(ULONGLONG*)&x >> 32;
    ix = hx & 0x7fffffff;
    /* |x| >= 1 or nan */
    if (ix >= 0x3ff00000) {
        unsigned int lx;

        lx = *(ULONGLONG*)&x;
        if (((ix - 0x3ff00000) | lx) == 0) {
            /* acos(1)=0, acos(-1)=pi */
            if (hx >> 31)
                return 2 * pio2_hi + 7.5231638452626401e-37;
            return 0;
        }
        if (isnan(x)) return x;
        return math_error(_DOMAIN, "acos", x, 0, 0 / (x - x));
    }
    /* |x| < 0.5 */
    if (ix < 0x3fe00000) {
        if (ix <= 0x3c600000)  /* |x| < 2**-57 */
            return pio2_hi + 7.5231638452626401e-37;
        return pio2_hi - (x - (pio2_lo - x * acos_R(x * x)));
    }
    /* x < -0.5 */
    if (hx >> 31) {
        z = (1.0 + x) * 0.5;
        s = sqrt(z);
        w = acos_R(z) * s - pio2_lo;
        return 2 * (pio2_hi - (s + w));
    }
    /* x > 0.5 */
    z = (1.0 - x) * 0.5;
    s = sqrt(z);
    df = s;
    llx = (*(ULONGLONG*)&df >> 32) << 32;
    df = *(double*)&llx;
    c = (z - df * df) / (s + df);
    w = acos_R(z) * s + c;
    return 2 * (df + w);
}

/*********************************************************************
 *		asin (MSVCRT.@)
 *
 * Copied from musl: src/math/asin.c
 */
static double asin_R(double z)
{
    /* coefficients for R(x^2) */
    static const double pS0 =  1.66666666666666657415e-01,
                 pS1 = -3.25565818622400915405e-01,
                 pS2 =  2.01212532134862925881e-01,
                 pS3 = -4.00555345006794114027e-02,
                 pS4 =  7.91534994289814532176e-04,
                 pS5 =  3.47933107596021167570e-05,
                 qS1 = -2.40339491173441421878e+00,
                 qS2 =  2.02094576023350569471e+00,
                 qS3 = -6.88283971605453293030e-01,
                 qS4 =  7.70381505559019352791e-02;

    double p, q;
    p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
    q = 1.0 + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
    return p / q;
}

#ifdef __i386__
double CDECL x87_asin(double);
__ASM_GLOBAL_FUNC( x87_asin,
        "fldl 4(%esp)\n\t"
        SET_X87_CW(~0x37f)
        "fld %st\n\t"
        "fld1\n\t"
        "fsubp\n\t"
        "fld1\n\t"
        "fadd %st(2)\n\t"
        "fmulp\n\t"
        "fsqrt\n\t"
        "fpatan\n\t"
        RESET_X87_CW
        "ret" )
#endif

double CDECL asin( double x )
{
    static const double pio2_hi = 1.57079632679489655800e+00,
                 pio2_lo = 6.12323399573676603587e-17;

    double z, r, s;
    unsigned int hx, ix;
    ULONGLONG llx;
#ifdef __i386__
    unsigned int x87_cw, sse2_cw;
#endif

    hx = *(ULONGLONG*)&x >> 32;
    ix = hx & 0x7fffffff;
    /* |x| >= 1 or nan */
    if (ix >= 0x3ff00000) {
        unsigned int lx;
        lx = *(ULONGLONG*)&x;
        if (((ix - 0x3ff00000) | lx) == 0)
            /* asin(1) = +-pi/2 with inexact */
            return x * pio2_hi + 7.5231638452626401e-37;
        if (isnan(x))
        {
#ifdef __i386__
            return math_error(_DOMAIN, "asin", x, 0, x);
#else
            return x;
#endif
        }
        return math_error(_DOMAIN, "asin", x, 0, 0 / (x - x));
    }

#ifdef __i386__
    __control87_2(0, 0, &x87_cw, &sse2_cw);
    if (!sse2_enabled || (x87_cw & _MCW_EM) != _MCW_EM
            || (sse2_cw & (_MCW_EM | _MCW_RC)) != _MCW_EM)
        return x87_asin(x);
#endif

    /* |x| < 0.5 */
    if (ix < 0x3fe00000) {
        /* if 0x1p-1022 <= |x| < 0x1p-26, avoid raising underflow */
        if (ix < 0x3e500000 && ix >= 0x00100000)
            return x;
        return x + x * asin_R(x * x);
    }
    /* 1 > |x| >= 0.5 */
    z = (1 - fabs(x)) * 0.5;
    s = sqrt(z);
    r = asin_R(z);
    if (ix >= 0x3fef3333) {  /* if |x| > 0.975 */
        x = pio2_hi - (2 * (s + s * r) - pio2_lo);
    } else {
        double f, c;
        /* f+c = sqrt(z) */
        f = s;
        llx = (*(ULONGLONG*)&f >> 32) << 32;
        f = *(double*)&llx;
        c = (z - f * f) / (s + f);
        x = 0.5 * pio2_hi - (2 * s * r - (pio2_lo - 2 * c) - (0.5 * pio2_hi - 2 * f));
    }
    if (hx >> 31)
        return -x;
    return x;
}

/*********************************************************************
 *		atan (MSVCRT.@)
 *
 * Copied from musl: src/math/atan.c
 */
double CDECL atan( double x )
{
    static const double atanhi[] = {
        4.63647609000806093515e-01,
        7.85398163397448278999e-01,
        9.82793723247329054082e-01,
        1.57079632679489655800e+00,
    };
    static const double atanlo[] = {
        2.26987774529616870924e-17,
        3.06161699786838301793e-17,
        1.39033110312309984516e-17,
        6.12323399573676603587e-17,
    };
    static const double aT[] = {
        3.33333333333329318027e-01,
        -1.99999999998764832476e-01,
        1.42857142725034663711e-01,
        -1.11111104054623557880e-01,
        9.09088713343650656196e-02,
        -7.69187620504482999495e-02,
        6.66107313738753120669e-02,
        -5.83357013379057348645e-02,
        4.97687799461593236017e-02,
        -3.65315727442169155270e-02,
        1.62858201153657823623e-02,
    };

    double w, s1, s2, z;
    unsigned int ix, sign;
    int id;

#if _MSVCR_VER == 0
    if (isnan(x)) return math_error(_DOMAIN, "atan", x, 0, x);
#endif

    ix = *(ULONGLONG*)&x >> 32;
    sign = ix >> 31;
    ix &= 0x7fffffff;
    if (ix >= 0x44100000) {   /* if |x| >= 2^66 */
        if (isnan(x))
            return x;
        z = atanhi[3] + 7.5231638452626401e-37;
        return sign ? -z : z;
    }
    if (ix < 0x3fdc0000) {    /* |x| < 0.4375 */
        if (ix < 0x3e400000) {  /* |x| < 2^-27 */
            if (ix < 0x00100000)
                /* raise underflow for subnormal x */
                fp_barrierf((float)x);
            return x;
        }
        id = -1;
    } else {
        x = fabs(x);
        if (ix < 0x3ff30000) {  /* |x| < 1.1875 */
            if (ix < 0x3fe60000) {  /*  7/16 <= |x| < 11/16 */
                id = 0;
                x = (2.0 * x - 1.0) / (2.0 + x);
            } else {                /* 11/16 <= |x| < 19/16 */
                id = 1;
                x = (x - 1.0) / (x + 1.0);
            }
        } else {
            if (ix < 0x40038000) {  /* |x| < 2.4375 */
                id = 2;
                x = (x - 1.5) / (1.0 + 1.5 * x);
            } else {                /* 2.4375 <= |x| < 2^66 */
                id = 3;
                x = -1.0 / x;
            }
        }
    }
    /* end of argument reduction */
    z = x * x;
    w = z * z;
    /* break sum from i=0 to 10 aT[i]z**(i+1) into odd and even poly */
    s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    if (id < 0)
        return x - x * (s1 + s2);
    z = atanhi[id] - (x * (s1 + s2) - atanlo[id] - x);
    return sign ? -z : z;
}

/*********************************************************************
 *		atan2 (MSVCRT.@)
 *
 * Copied from musl: src/math/atan2.c
 */
double CDECL atan2( double y, double x )
{
    static const double pi     = 3.1415926535897931160E+00,
                 pi_lo  = 1.2246467991473531772E-16;

    double z;
    unsigned int m, lx, ly, ix, iy;

    if (isnan(x) || isnan(y))
        return x+y;
    ix = *(ULONGLONG*)&x >> 32;
    lx = *(ULONGLONG*)&x;
    iy = *(ULONGLONG*)&y >> 32;
    ly = *(ULONGLONG*)&y;
    if (((ix - 0x3ff00000) | lx) == 0)  /* x = 1.0 */
        return atan(y);
    m = ((iy >> 31) & 1) | ((ix >> 30) & 2);  /* 2*sign(x)+sign(y) */
    ix = ix & 0x7fffffff;
    iy = iy & 0x7fffffff;

    /* when y = 0 */
    if ((iy | ly) == 0) {
        switch(m) {
        case 0:
        case 1: return y;   /* atan(+-0,+anything)=+-0 */
        case 2: return pi;  /* atan(+0,-anything) = pi */
        case 3: return -pi; /* atan(-0,-anything) =-pi */
        }
    }
    /* when x = 0 */
    if ((ix | lx) == 0)
        return m & 1 ? -pi / 2 : pi / 2;
    /* when x is INF */
    if (ix == 0x7ff00000) {
        if (iy == 0x7ff00000) {
            switch(m) {
            case 0: return pi / 4;      /* atan(+INF,+INF) */
            case 1: return -pi / 4;     /* atan(-INF,+INF) */
            case 2: return 3 * pi / 4;  /* atan(+INF,-INF) */
            case 3: return -3 * pi / 4; /* atan(-INF,-INF) */
            }
        } else {
            switch(m) {
            case 0: return 0.0;  /* atan(+...,+INF) */
            case 1: return -0.0; /* atan(-...,+INF) */
            case 2: return pi;   /* atan(+...,-INF) */
            case 3: return -pi;  /* atan(-...,-INF) */
            }
        }
    }
    /* |y/x| > 0x1p64 */
    if (ix + (64 << 20) < iy || iy == 0x7ff00000)
        return m & 1 ? -pi / 2 : pi / 2;

    /* z = atan(|y/x|) without spurious underflow */
    if ((m & 2) && iy + (64 << 20) < ix)  /* |y/x| < 0x1p-64, x<0 */
        z = 0;
    else
        z = atan(fabs(y / x));
    switch (m) {
    case 0: return z;                /* atan(+,+) */
    case 1: return -z;               /* atan(-,+) */
    case 2: return pi - (z - pi_lo); /* atan(+,-) */
    default: /* case 3 */
        return (z - pi_lo) - pi;     /* atan(-,-) */
    }
}

/*********************************************************************
 *		cos (MSVCRT.@)
 */
double CDECL cos( double x )
{
  double ret = unix_funcs->cos( x );
  if (!isfinite(x)) return math_error(_DOMAIN, "cos", x, 0, ret);
  return ret;
}

/*********************************************************************
 *		cosh (MSVCRT.@)
 */
double CDECL cosh( double x )
{
  double ret = unix_funcs->cosh( x );
  if (isnan(x)) return math_error(_DOMAIN, "cosh", x, 0, ret);
  return ret;
}

/*********************************************************************
 *		exp (MSVCRT.@)
 */
double CDECL exp( double x )
{
  double ret = unix_funcs->exp( x );
  if (isnan(x)) return math_error(_DOMAIN, "exp", x, 0, ret);
  if (isfinite(x) && !ret) return math_error(_UNDERFLOW, "exp", x, 0, ret);
  if (isfinite(x) && !isfinite(ret)) return math_error(_OVERFLOW, "exp", x, 0, ret);
  return ret;
}

/*********************************************************************
 *		fmod (MSVCRT.@)
 */
double CDECL fmod( double x, double y )
{
  double ret = unix_funcs->fmod( x, y );
  if (!isfinite(x) || !isfinite(y)) return math_error(_DOMAIN, "fmod", x, y, ret);
  return ret;
}

/*********************************************************************
 *		log (MSVCRT.@)
 */
double CDECL log( double x )
{
  double ret = unix_funcs->log( x );
  if (x < 0.0) return math_error(_DOMAIN, "log", x, 0, ret);
  if (x == 0.0) return math_error(_SING, "log", x, 0, ret);
  return ret;
}

/*********************************************************************
 *		log10 (MSVCRT.@)
 */
double CDECL log10( double x )
{
  double ret = unix_funcs->log10( x );
  if (x < 0.0) return math_error(_DOMAIN, "log10", x, 0, ret);
  if (x == 0.0) return math_error(_SING, "log10", x, 0, ret);
  return ret;
}

/*********************************************************************
 *		pow (MSVCRT.@)
 */
double CDECL pow( double x, double y )
{
  double z = unix_funcs->pow(x,y);
  if (x < 0 && y != floor(y))
    return math_error(_DOMAIN, "pow", x, y, z);
  if (!x && isfinite(y) && y < 0)
    return math_error(_SING, "pow", x, y, z);
  if (isfinite(x) && isfinite(y) && !isfinite(z))
    return math_error(_OVERFLOW, "pow", x, y, z);
  if (x && isfinite(x) && isfinite(y) && !z)
    return math_error(_UNDERFLOW, "pow", x, y, z);
  return z;
}

/*********************************************************************
 *		sin (MSVCRT.@)
 */
double CDECL sin( double x )
{
  double ret = unix_funcs->sin( x );
  if (!isfinite(x)) return math_error(_DOMAIN, "sin", x, 0, ret);
  return ret;
}

/*********************************************************************
 *		sinh (MSVCRT.@)
 */
double CDECL sinh( double x )
{
  double ret = unix_funcs->sinh( x );
  if (isnan(x)) return math_error(_DOMAIN, "sinh", x, 0, ret);
  return ret;
}

static BOOL sqrt_validate( double *x, BOOL update_sw )
{
    short c = _dclass(*x);

    if (c == FP_ZERO) return FALSE;
    if (c == FP_NAN)
    {
#ifdef __i386__
        if (update_sw)
            *x = math_error(_DOMAIN, "sqrt", *x, 0, *x);
#else
        /* set signaling bit */
        *(ULONGLONG*)x |= 0x8000000000000ULL;
#endif
        return FALSE;
    }
    if (signbit(*x))
    {
        *x = math_error(_DOMAIN, "sqrt", *x, 0, ret_nan(update_sw));
        return FALSE;
    }
    if (c == FP_INFINITE) return FALSE;
    return TRUE;
}

#if defined(__x86_64__) || defined(__i386__)
double CDECL sse2_sqrt(double);
__ASM_GLOBAL_FUNC( sse2_sqrt,
        "sqrtsd %xmm0, %xmm0\n\t"
        "ret" )
#endif

#ifdef __i386__
double CDECL x87_sqrt(double);
__ASM_GLOBAL_FUNC( x87_sqrt,
        "fldl 4(%esp)\n\t"
        SET_X87_CW(0xc00)
        "fsqrt\n\t"
        RESET_X87_CW
        "ret" )
#endif

/*********************************************************************
 *		sqrt (MSVCRT.@)
 *
 * Copied from musl: src/math/sqrt.c
 */
double CDECL sqrt( double x )
{
#ifdef __x86_64__
    if (!sqrt_validate(&x, TRUE))
        return x;

    return sse2_sqrt(x);
#elif defined( __i386__ )
    if (!sqrt_validate(&x, TRUE))
        return x;

    return x87_sqrt(x);
#else
    static const double tiny = 1.0e-300;

    double z;
    int sign = 0x80000000;
    int ix0,s0,q,m,t,i;
    unsigned int r,t1,s1,ix1,q1;
    ULONGLONG ix;

    if (!sqrt_validate(&x, TRUE))
        return x;

    ix = *(ULONGLONG*)&x;
    ix0 = ix >> 32;
    ix1 = ix;

    /* normalize x */
    m = ix0 >> 20;
    if (m == 0) {  /* subnormal x */
        while (ix0 == 0) {
            m -= 21;
            ix0 |= (ix1 >> 11);
            ix1 <<= 21;
        }
        for (i=0; (ix0 & 0x00100000) == 0; i++)
            ix0 <<= 1;
        m -= i - 1;
        ix0 |= ix1 >> (32 - i);
        ix1 <<= i;
    }
    m -= 1023;    /* unbias exponent */
    ix0 = (ix0 & 0x000fffff) | 0x00100000;
    if (m & 1) {  /* odd m, double x to make it even */
        ix0 += ix0 + ((ix1 & sign) >> 31);
        ix1 += ix1;
    }
    m >>= 1;      /* m = [m/2] */

    /* generate sqrt(x) bit by bit */
    ix0 += ix0 + ((ix1 & sign) >> 31);
    ix1 += ix1;
    q = q1 = s0 = s1 = 0;  /* [q,q1] = sqrt(x) */
    r = 0x00200000;        /* r = moving bit from right to left */

    while (r != 0) {
        t = s0 + r;
        if (t <= ix0) {
            s0   = t + r;
            ix0 -= t;
            q   += r;
        }
        ix0 += ix0 + ((ix1 & sign) >> 31);
        ix1 += ix1;
        r >>= 1;
    }

    r = sign;
    while (r != 0) {
        t1 = s1 + r;
        t  = s0;
        if (t < ix0 || (t == ix0 && t1 <= ix1)) {
            s1 = t1 + r;
            if ((t1&sign) == sign && (s1 & sign) == 0)
                s0++;
            ix0 -= t;
            if (ix1 < t1)
                ix0--;
            ix1 -= t1;
            q1 += r;
        }
        ix0 += ix0 + ((ix1 & sign) >> 31);
        ix1 += ix1;
        r >>= 1;
    }

    /* use floating add to find out rounding direction */
    if ((ix0 | ix1) != 0) {
        z = 1.0 - tiny; /* raise inexact flag */
        if (z >= 1.0) {
            z = 1.0 + tiny;
            if (q1 == (unsigned int)0xffffffff) {
                q1 = 0;
                q++;
            } else if (z > 1.0) {
                if (q1 == (unsigned int)0xfffffffe)
                    q++;
                q1 += 2;
            } else
                q1 += q1 & 1;
        }
    }
    ix0 = (q >> 1) + 0x3fe00000;
    ix1 = q1 >> 1;
    if (q & 1)
        ix1 |= sign;
    ix = ix0 + ((unsigned int)m << 20);
    ix <<= 32;
    ix |= ix1;
    return *(double*)&ix;
#endif
}

/*********************************************************************
 *		tan (MSVCRT.@)
 */
double CDECL tan( double x )
{
  double ret = unix_funcs->tan(x);
  if (!isfinite(x)) return math_error(_DOMAIN, "tan", x, 0, ret);
  return ret;
}

/*********************************************************************
 *		tanh (MSVCRT.@)
 */
double CDECL tanh( double x )
{
  double ret = unix_funcs->tanh(x);
  if (isnan(x)) return math_error(_DOMAIN, "tanh", x, 0, ret);
  return ret;
}


#if (defined(__GNUC__) || defined(__clang__)) && defined(__i386__)

#define CREATE_FPU_FUNC1(name, call) \
    __ASM_GLOBAL_FUNC(name, \
            "pushl   %ebp\n\t" \
            __ASM_CFI(".cfi_adjust_cfa_offset 4\n\t") \
            __ASM_CFI(".cfi_rel_offset %ebp,0\n\t") \
            "movl    %esp, %ebp\n\t" \
            __ASM_CFI(".cfi_def_cfa_register %ebp\n\t") \
            "subl    $68, %esp\n\t" /* sizeof(double)*8 + sizeof(int) */ \
            "fstpl   (%esp)\n\t"    /* store function argument */ \
            "fwait\n\t" \
            "movl    $1, %ecx\n\t"  /* empty FPU stack */ \
            "1:\n\t" \
            "fxam\n\t" \
            "fstsw   %ax\n\t" \
            "and     $0x4500, %ax\n\t" \
            "cmp     $0x4100, %ax\n\t" \
            "je      2f\n\t" \
            "fstpl    (%esp,%ecx,8)\n\t" \
            "fwait\n\t" \
            "incl    %ecx\n\t" \
            "jmp     1b\n\t" \
            "2:\n\t" \
            "movl    %ecx, -4(%ebp)\n\t" \
            "call    " __ASM_NAME( #call ) "\n\t" \
            "movl    -4(%ebp), %ecx\n\t" \
            "fstpl   (%esp)\n\t"    /* save result */ \
            "3:\n\t"                /* restore FPU stack */ \
            "decl    %ecx\n\t" \
            "fldl    (%esp,%ecx,8)\n\t" \
            "cmpl    $0, %ecx\n\t" \
            "jne     3b\n\t" \
            "leave\n\t" \
            __ASM_CFI(".cfi_def_cfa %esp,4\n\t") \
            __ASM_CFI(".cfi_same_value %ebp\n\t") \
            "ret")

#define CREATE_FPU_FUNC2(name, call) \
    __ASM_GLOBAL_FUNC(name, \
            "pushl   %ebp\n\t" \
            __ASM_CFI(".cfi_adjust_cfa_offset 4\n\t") \
            __ASM_CFI(".cfi_rel_offset %ebp,0\n\t") \
            "movl    %esp, %ebp\n\t" \
            __ASM_CFI(".cfi_def_cfa_register %ebp\n\t") \
            "subl    $68, %esp\n\t" /* sizeof(double)*8 + sizeof(int) */ \
            "fstpl   8(%esp)\n\t"   /* store function argument */ \
            "fwait\n\t" \
            "fstpl   (%esp)\n\t" \
            "fwait\n\t" \
            "movl    $2, %ecx\n\t"  /* empty FPU stack */ \
            "1:\n\t" \
            "fxam\n\t" \
            "fstsw   %ax\n\t" \
            "and     $0x4500, %ax\n\t" \
            "cmp     $0x4100, %ax\n\t" \
            "je      2f\n\t" \
            "fstpl    (%esp,%ecx,8)\n\t" \
            "fwait\n\t" \
            "incl    %ecx\n\t" \
            "jmp     1b\n\t" \
            "2:\n\t" \
            "movl    %ecx, -4(%ebp)\n\t" \
            "call    " __ASM_NAME( #call ) "\n\t" \
            "movl    -4(%ebp), %ecx\n\t" \
            "fstpl   8(%esp)\n\t"   /* save result */ \
            "3:\n\t"                /* restore FPU stack */ \
            "decl    %ecx\n\t" \
            "fldl    (%esp,%ecx,8)\n\t" \
            "cmpl    $1, %ecx\n\t" \
            "jne     3b\n\t" \
            "leave\n\t" \
            __ASM_CFI(".cfi_def_cfa %esp,4\n\t") \
            __ASM_CFI(".cfi_same_value %ebp\n\t") \
            "ret")

CREATE_FPU_FUNC1(_CIacos, acos)
CREATE_FPU_FUNC1(_CIasin, asin)
CREATE_FPU_FUNC1(_CIatan, atan)
CREATE_FPU_FUNC2(_CIatan2, atan2)
CREATE_FPU_FUNC1(_CIcos, cos)
CREATE_FPU_FUNC1(_CIcosh, cosh)
CREATE_FPU_FUNC1(_CIexp, exp)
CREATE_FPU_FUNC2(_CIfmod, fmod)
CREATE_FPU_FUNC1(_CIlog, log)
CREATE_FPU_FUNC1(_CIlog10, log10)
CREATE_FPU_FUNC2(_CIpow, pow)
CREATE_FPU_FUNC1(_CIsin, sin)
CREATE_FPU_FUNC1(_CIsinh, sinh)
CREATE_FPU_FUNC1(_CIsqrt, sqrt)
CREATE_FPU_FUNC1(_CItan, tan)
CREATE_FPU_FUNC1(_CItanh, tanh)

__ASM_GLOBAL_FUNC(_ftol,
        "pushl   %ebp\n\t"
        __ASM_CFI(".cfi_adjust_cfa_offset 4\n\t")
        __ASM_CFI(".cfi_rel_offset %ebp,0\n\t")
        "movl    %esp, %ebp\n\t"
        __ASM_CFI(".cfi_def_cfa_register %ebp\n\t")
        "subl    $12, %esp\n\t"     /* sizeof(LONGLONG) + 2*sizeof(WORD) */
        "fnstcw  (%esp)\n\t"
        "mov     (%esp), %ax\n\t"
        "or      $0xc00, %ax\n\t"
        "mov     %ax, 2(%esp)\n\t"
        "fldcw   2(%esp)\n\t"
        "fistpq  4(%esp)\n\t"
        "fldcw   (%esp)\n\t"
        "movl    4(%esp), %eax\n\t"
        "movl    8(%esp), %edx\n\t"
        "leave\n\t"
        __ASM_CFI(".cfi_def_cfa %esp,4\n\t")
        __ASM_CFI(".cfi_same_value %ebp\n\t")
        "ret")

#endif /* (defined(__GNUC__) || defined(__clang__)) && defined(__i386__) */

/*********************************************************************
 *		_fpclass (MSVCRT.@)
 */
int CDECL _fpclass(double num)
{
    union { double f; UINT64 i; } u = { num };
    int e = u.i >> 52 & 0x7ff;
    int s = u.i >> 63;

    switch (e)
    {
    case 0:
        if (u.i << 1) return s ? _FPCLASS_ND : _FPCLASS_PD;
        return s ? _FPCLASS_NZ : _FPCLASS_PZ;
    case 0x7ff:
        if (u.i << 12) return ((u.i >> 51) & 1) ? _FPCLASS_QNAN : _FPCLASS_SNAN;
        return s ? _FPCLASS_NINF : _FPCLASS_PINF;
    default:
        return s ? _FPCLASS_NN : _FPCLASS_PN;
    }
}

/*********************************************************************
 *		_rotl (MSVCRT.@)
 */
unsigned int CDECL MSVCRT__rotl(unsigned int num, int shift)
{
  shift &= 31;
  return (num << shift) | (num >> (32-shift));
}

/*********************************************************************
 *		_lrotl (MSVCRT.@)
 */
__msvcrt_ulong CDECL MSVCRT__lrotl(__msvcrt_ulong num, int shift)
{
  shift &= 0x1f;
  return (num << shift) | (num >> (32-shift));
}

/*********************************************************************
 *		_lrotr (MSVCRT.@)
 */
__msvcrt_ulong CDECL MSVCRT__lrotr(__msvcrt_ulong num, int shift)
{
  shift &= 0x1f;
  return (num >> shift) | (num << (32-shift));
}

/*********************************************************************
 *		_rotr (MSVCRT.@)
 */
unsigned int CDECL MSVCRT__rotr(unsigned int num, int shift)
{
    shift &= 0x1f;
    return (num >> shift) | (num << (32-shift));
}

/*********************************************************************
 *		_rotl64 (MSVCRT.@)
 */
unsigned __int64 CDECL MSVCRT__rotl64(unsigned __int64 num, int shift)
{
  shift &= 63;
  return (num << shift) | (num >> (64-shift));
}

/*********************************************************************
 *		_rotr64 (MSVCRT.@)
 */
unsigned __int64 CDECL MSVCRT__rotr64(unsigned __int64 num, int shift)
{
    shift &= 63;
    return (num >> shift) | (num << (64-shift));
}

/*********************************************************************
 *		abs (MSVCRT.@)
 */
int CDECL abs( int n )
{
    return n >= 0 ? n : -n;
}

/*********************************************************************
 *		labs (MSVCRT.@)
 */
__msvcrt_long CDECL labs( __msvcrt_long n )
{
    return n >= 0 ? n : -n;
}

#if _MSVCR_VER>=100
/*********************************************************************
 *		llabs (MSVCR100.@)
 */
__int64 CDECL llabs( __int64 n )
{
    return n >= 0 ? n : -n;
}
#endif

#if _MSVCR_VER>=120
/*********************************************************************
 *		imaxabs (MSVCR120.@)
 */
intmax_t CDECL imaxabs( intmax_t n )
{
    return n >= 0 ? n : -n;
}
#endif

/*********************************************************************
 *		_abs64 (MSVCRT.@)
 */
__int64 CDECL _abs64( __int64 n )
{
    return n >= 0 ? n : -n;
}

/*********************************************************************
 *		_logb (MSVCRT.@)
 */
double CDECL _logb(double num)
{
  double ret = unix_funcs->logb(num);
  if (isnan(num)) return math_error(_DOMAIN, "_logb", num, 0, ret);
  if (!num) return math_error(_SING, "_logb", num, 0, ret);
  return ret;
}

/*********************************************************************
 *		_hypot (MSVCRT.@)
 */
double CDECL _hypot(double x, double y)
{
  /* FIXME: errno handling */
  return unix_funcs->hypot( x, y );
}

/*********************************************************************
 *      _hypotf (MSVCRT.@)
 */
float CDECL _hypotf(float x, float y)
{
  /* FIXME: errno handling */
  return unix_funcs->hypotf( x, y );
}

/*********************************************************************
 *		ceil (MSVCRT.@)
 */
double CDECL ceil( double x )
{
  return unix_funcs->ceil(x);
}

/*********************************************************************
 *		floor (MSVCRT.@)
 */
double CDECL floor( double x )
{
  return unix_funcs->floor(x);
}

/*********************************************************************
 *      fma (MSVCRT.@)
 */
double CDECL fma( double x, double y, double z )
{
  double w = unix_funcs->fma(x, y, z);
  if ((isinf(x) && y == 0) || (x == 0 && isinf(y))) *_errno() = EDOM;
  else if (isinf(x) && isinf(z) && x != z) *_errno() = EDOM;
  else if (isinf(y) && isinf(z) && y != z) *_errno() = EDOM;
  return w;
}

/*********************************************************************
 *      fmaf (MSVCRT.@)
 */
float CDECL fmaf( float x, float y, float z )
{
  float w = unix_funcs->fmaf(x, y, z);
  if ((isinf(x) && y == 0) || (x == 0 && isinf(y))) *_errno() = EDOM;
  else if (isinf(x) && isinf(z) && x != z) *_errno() = EDOM;
  else if (isinf(y) && isinf(z) && y != z) *_errno() = EDOM;
  return w;
}

/*********************************************************************
 *		fabs (MSVCRT.@)
 *
 * Copied from musl: src/math/fabsf.c
 */
double CDECL fabs( double x )
{
    union { double f; UINT64 i; } u = { x };
    u.i &= ~0ull >> 1;
    return u.f;
}

/*********************************************************************
 *		frexp (MSVCRT.@)
 */
double CDECL frexp( double x, int *exp )
{
  return unix_funcs->frexp( x, exp );
}

/*********************************************************************
 *		modf (MSVCRT.@)
 */
double CDECL modf( double x, double *iptr )
{
  return unix_funcs->modf( x, iptr );
}

/**********************************************************************
 *		_statusfp2 (MSVCRT.@)
 *
 * Not exported by native msvcrt, added in msvcr80.
 */
#if defined(__i386__) || defined(__x86_64__)
void CDECL _statusfp2( unsigned int *x86_sw, unsigned int *sse2_sw )
{
#if defined(__GNUC__) || defined(__clang__)
    unsigned int flags;
    unsigned long fpword;

    if (x86_sw)
    {
        __asm__ __volatile__( "fstsw %0" : "=m" (fpword) );
        flags = 0;
        if (fpword & 0x1)  flags |= _SW_INVALID;
        if (fpword & 0x2)  flags |= _SW_DENORMAL;
        if (fpword & 0x4)  flags |= _SW_ZERODIVIDE;
        if (fpword & 0x8)  flags |= _SW_OVERFLOW;
        if (fpword & 0x10) flags |= _SW_UNDERFLOW;
        if (fpword & 0x20) flags |= _SW_INEXACT;
        *x86_sw = flags;
    }

    if (!sse2_sw) return;

    if (sse2_supported)
    {
        __asm__ __volatile__( "stmxcsr %0" : "=m" (fpword) );
        flags = 0;
        if (fpword & 0x1)  flags |= _SW_INVALID;
        if (fpword & 0x2)  flags |= _SW_DENORMAL;
        if (fpword & 0x4)  flags |= _SW_ZERODIVIDE;
        if (fpword & 0x8)  flags |= _SW_OVERFLOW;
        if (fpword & 0x10) flags |= _SW_UNDERFLOW;
        if (fpword & 0x20) flags |= _SW_INEXACT;
        *sse2_sw = flags;
    }
    else *sse2_sw = 0;
#else
    FIXME( "not implemented\n" );
#endif
}
#endif

/**********************************************************************
 *		_statusfp (MSVCRT.@)
 */
unsigned int CDECL _statusfp(void)
{
    unsigned int flags = 0;
#if defined(__i386__) || defined(__x86_64__)
    unsigned int x86_sw, sse2_sw;

    _statusfp2( &x86_sw, &sse2_sw );
    /* FIXME: there's no definition for ambiguous status, just return all status bits for now */
    flags = x86_sw | sse2_sw;
#elif defined(__aarch64__)
    ULONG_PTR fpsr;

    __asm__ __volatile__( "mrs %0, fpsr" : "=r" (fpsr) );
    if (fpsr & 0x1)  flags |= _SW_INVALID;
    if (fpsr & 0x2)  flags |= _SW_ZERODIVIDE;
    if (fpsr & 0x4)  flags |= _SW_OVERFLOW;
    if (fpsr & 0x8)  flags |= _SW_UNDERFLOW;
    if (fpsr & 0x10) flags |= _SW_INEXACT;
    if (fpsr & 0x80) flags |= _SW_DENORMAL;
#else
    FIXME( "not implemented\n" );
#endif
    return flags;
}

/*********************************************************************
 *		_clearfp (MSVCRT.@)
 */
unsigned int CDECL _clearfp(void)
{
    unsigned int flags = 0;
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(__x86_64__))
    unsigned long fpword;

    __asm__ __volatile__( "fnstsw %0; fnclex" : "=m" (fpword) );
    if (fpword & 0x1)  flags |= _SW_INVALID;
    if (fpword & 0x2)  flags |= _SW_DENORMAL;
    if (fpword & 0x4)  flags |= _SW_ZERODIVIDE;
    if (fpword & 0x8)  flags |= _SW_OVERFLOW;
    if (fpword & 0x10) flags |= _SW_UNDERFLOW;
    if (fpword & 0x20) flags |= _SW_INEXACT;

    if (sse2_supported)
    {
        __asm__ __volatile__( "stmxcsr %0" : "=m" (fpword) );
        if (fpword & 0x1)  flags |= _SW_INVALID;
        if (fpword & 0x2)  flags |= _SW_DENORMAL;
        if (fpword & 0x4)  flags |= _SW_ZERODIVIDE;
        if (fpword & 0x8)  flags |= _SW_OVERFLOW;
        if (fpword & 0x10) flags |= _SW_UNDERFLOW;
        if (fpword & 0x20) flags |= _SW_INEXACT;
        fpword &= ~0x3f;
        __asm__ __volatile__( "ldmxcsr %0" : : "m" (fpword) );
    }
#elif defined(__aarch64__)
    ULONG_PTR fpsr;

    __asm__ __volatile__( "mrs %0, fpsr" : "=r" (fpsr) );
    if (fpsr & 0x1)  flags |= _SW_INVALID;
    if (fpsr & 0x2)  flags |= _SW_ZERODIVIDE;
    if (fpsr & 0x4)  flags |= _SW_OVERFLOW;
    if (fpsr & 0x8)  flags |= _SW_UNDERFLOW;
    if (fpsr & 0x10) flags |= _SW_INEXACT;
    if (fpsr & 0x80) flags |= _SW_DENORMAL;
    fpsr &= ~0x9f;
    __asm__ __volatile__( "msr fpsr, %0" :: "r" (fpsr) );
#else
    FIXME( "not implemented\n" );
#endif
    return flags;
}

/*********************************************************************
 *		__fpecode (MSVCRT.@)
 */
int * CDECL __fpecode(void)
{
    return &msvcrt_get_thread_data()->fpecode;
}

/*********************************************************************
 *		ldexp (MSVCRT.@)
 */
double CDECL ldexp(double num, int exp)
{
  double z = unix_funcs->ldexp(num,exp);

  if (isfinite(num) && !isfinite(z))
    return math_error(_OVERFLOW, "ldexp", num, exp, z);
  if (num && isfinite(num) && !z)
    return math_error(_UNDERFLOW, "ldexp", num, exp, z);
  if (z == 0 && signbit(z))
    z = 0.0; /* Convert -0 -> +0 */
  return z;
}

/*********************************************************************
 *		_cabs (MSVCRT.@)
 */
double CDECL _cabs(struct _complex num)
{
  return sqrt(num.x * num.x + num.y * num.y);
}

/*********************************************************************
 *		_chgsign (MSVCRT.@)
 */
double CDECL _chgsign(double num)
{
    union { double f; UINT64 i; } u = { num };
    u.i ^= 1ull << 63;
    return u.f;
}

/*********************************************************************
 *		__control87_2 (MSVCR80.@)
 *
 * Not exported by native msvcrt, added in msvcr80.
 */
#ifdef __i386__
int CDECL __control87_2( unsigned int newval, unsigned int mask,
                         unsigned int *x86_cw, unsigned int *sse2_cw )
{
#if defined(__GNUC__) || defined(__clang__)
    unsigned long fpword;
    unsigned int flags;
    unsigned int old_flags;

    if (x86_cw)
    {
        __asm__ __volatile__( "fstcw %0" : "=m" (fpword) );

        /* Convert into mask constants */
        flags = 0;
        if (fpword & 0x1)  flags |= _EM_INVALID;
        if (fpword & 0x2)  flags |= _EM_DENORMAL;
        if (fpword & 0x4)  flags |= _EM_ZERODIVIDE;
        if (fpword & 0x8)  flags |= _EM_OVERFLOW;
        if (fpword & 0x10) flags |= _EM_UNDERFLOW;
        if (fpword & 0x20) flags |= _EM_INEXACT;
        switch (fpword & 0xc00)
        {
        case 0xc00: flags |= _RC_UP|_RC_DOWN; break;
        case 0x800: flags |= _RC_UP; break;
        case 0x400: flags |= _RC_DOWN; break;
        }
        switch (fpword & 0x300)
        {
        case 0x0:   flags |= _PC_24; break;
        case 0x200: flags |= _PC_53; break;
        case 0x300: flags |= _PC_64; break;
        }
        if (fpword & 0x1000) flags |= _IC_AFFINE;

        TRACE( "x86 flags=%08x newval=%08x mask=%08x\n", flags, newval, mask );
        if (mask)
        {
            flags = (flags & ~mask) | (newval & mask);

            /* Convert (masked) value back to fp word */
            fpword = 0;
            if (flags & _EM_INVALID)    fpword |= 0x1;
            if (flags & _EM_DENORMAL)   fpword |= 0x2;
            if (flags & _EM_ZERODIVIDE) fpword |= 0x4;
            if (flags & _EM_OVERFLOW)   fpword |= 0x8;
            if (flags & _EM_UNDERFLOW)  fpword |= 0x10;
            if (flags & _EM_INEXACT)    fpword |= 0x20;
            switch (flags & _MCW_RC)
            {
            case _RC_UP|_RC_DOWN:   fpword |= 0xc00; break;
            case _RC_UP:            fpword |= 0x800; break;
            case _RC_DOWN:          fpword |= 0x400; break;
            }
            switch (flags & _MCW_PC)
            {
            case _PC_64: fpword |= 0x300; break;
            case _PC_53: fpword |= 0x200; break;
            case _PC_24: fpword |= 0x0; break;
            }
            if (flags & _IC_AFFINE) fpword |= 0x1000;

            __asm__ __volatile__( "fldcw %0" : : "m" (fpword) );
        }
        *x86_cw = flags;
    }

    if (!sse2_cw) return 1;

    if (sse2_supported)
    {
        __asm__ __volatile__( "stmxcsr %0" : "=m" (fpword) );

        /* Convert into mask constants */
        flags = 0;
        if (fpword & 0x80)   flags |= _EM_INVALID;
        if (fpword & 0x100)  flags |= _EM_DENORMAL;
        if (fpword & 0x200)  flags |= _EM_ZERODIVIDE;
        if (fpword & 0x400)  flags |= _EM_OVERFLOW;
        if (fpword & 0x800)  flags |= _EM_UNDERFLOW;
        if (fpword & 0x1000) flags |= _EM_INEXACT;
        switch (fpword & 0x6000)
        {
        case 0x6000: flags |= _RC_UP|_RC_DOWN; break;
        case 0x4000: flags |= _RC_UP; break;
        case 0x2000: flags |= _RC_DOWN; break;
        }
        switch (fpword & 0x8040)
        {
        case 0x0040: flags |= _DN_FLUSH_OPERANDS_SAVE_RESULTS; break;
        case 0x8000: flags |= _DN_SAVE_OPERANDS_FLUSH_RESULTS; break;
        case 0x8040: flags |= _DN_FLUSH; break;
        }

        TRACE( "sse2 flags=%08x newval=%08x mask=%08x\n", flags, newval, mask );
        if (mask)
        {
            old_flags = flags;
            mask &= _MCW_EM | _MCW_RC | _MCW_DN;
            flags = (flags & ~mask) | (newval & mask);

            if (flags != old_flags)
            {
                /* Convert (masked) value back to fp word */
                fpword = 0;
                if (flags & _EM_INVALID)    fpword |= 0x80;
                if (flags & _EM_DENORMAL)   fpword |= 0x100;
                if (flags & _EM_ZERODIVIDE) fpword |= 0x200;
                if (flags & _EM_OVERFLOW)   fpword |= 0x400;
                if (flags & _EM_UNDERFLOW)  fpword |= 0x800;
                if (flags & _EM_INEXACT)    fpword |= 0x1000;
                switch (flags & _MCW_RC)
                {
                case _RC_UP|_RC_DOWN:   fpword |= 0x6000; break;
                case _RC_UP:            fpword |= 0x4000; break;
                case _RC_DOWN:          fpword |= 0x2000; break;
                }
                switch (flags & _MCW_DN)
                {
                case _DN_FLUSH_OPERANDS_SAVE_RESULTS: fpword |= 0x0040; break;
                case _DN_SAVE_OPERANDS_FLUSH_RESULTS: fpword |= 0x8000; break;
                case _DN_FLUSH:                       fpword |= 0x8040; break;
                }
                __asm__ __volatile__( "ldmxcsr %0" : : "m" (fpword) );
            }
        }
        *sse2_cw = flags;
    }
    else *sse2_cw = 0;

    return 1;
#else
    FIXME( "not implemented\n" );
    return 0;
#endif
}
#endif

/*********************************************************************
 *		_control87 (MSVCRT.@)
 */
unsigned int CDECL _control87(unsigned int newval, unsigned int mask)
{
    unsigned int flags = 0;
#ifdef __i386__
    unsigned int sse2_cw;

    __control87_2( newval, mask, &flags, &sse2_cw );

    if ((flags ^ sse2_cw) & (_MCW_EM | _MCW_RC)) flags |= _EM_AMBIGUOUS;
    flags |= sse2_cw;
#elif defined(__x86_64__)
    unsigned long fpword;
    unsigned int old_flags;

    __asm__ __volatile__( "stmxcsr %0" : "=m" (fpword) );
    if (fpword & 0x80)   flags |= _EM_INVALID;
    if (fpword & 0x100)  flags |= _EM_DENORMAL;
    if (fpword & 0x200)  flags |= _EM_ZERODIVIDE;
    if (fpword & 0x400)  flags |= _EM_OVERFLOW;
    if (fpword & 0x800)  flags |= _EM_UNDERFLOW;
    if (fpword & 0x1000) flags |= _EM_INEXACT;
    switch (fpword & 0x6000)
    {
    case 0x6000: flags |= _RC_CHOP; break;
    case 0x4000: flags |= _RC_UP; break;
    case 0x2000: flags |= _RC_DOWN; break;
    }
    switch (fpword & 0x8040)
    {
    case 0x0040: flags |= _DN_FLUSH_OPERANDS_SAVE_RESULTS; break;
    case 0x8000: flags |= _DN_SAVE_OPERANDS_FLUSH_RESULTS; break;
    case 0x8040: flags |= _DN_FLUSH; break;
    }
    old_flags = flags;
    mask &= _MCW_EM | _MCW_RC | _MCW_DN;
    flags = (flags & ~mask) | (newval & mask);
    if (flags != old_flags)
    {
        fpword = 0;
        if (flags & _EM_INVALID)    fpword |= 0x80;
        if (flags & _EM_DENORMAL)   fpword |= 0x100;
        if (flags & _EM_ZERODIVIDE) fpword |= 0x200;
        if (flags & _EM_OVERFLOW)   fpword |= 0x400;
        if (flags & _EM_UNDERFLOW)  fpword |= 0x800;
        if (flags & _EM_INEXACT)    fpword |= 0x1000;
        switch (flags & _MCW_RC)
        {
        case _RC_CHOP: fpword |= 0x6000; break;
        case _RC_UP:   fpword |= 0x4000; break;
        case _RC_DOWN: fpword |= 0x2000; break;
        }
        switch (flags & _MCW_DN)
        {
        case _DN_FLUSH_OPERANDS_SAVE_RESULTS: fpword |= 0x0040; break;
        case _DN_SAVE_OPERANDS_FLUSH_RESULTS: fpword |= 0x8000; break;
        case _DN_FLUSH:                       fpword |= 0x8040; break;
        }
        __asm__ __volatile__( "ldmxcsr %0" :: "m" (fpword) );
    }
#elif defined(__aarch64__)
    ULONG_PTR fpcr;

    __asm__ __volatile__( "mrs %0, fpcr" : "=r" (fpcr) );
    if (!(fpcr & 0x100))  flags |= _EM_INVALID;
    if (!(fpcr & 0x200))  flags |= _EM_ZERODIVIDE;
    if (!(fpcr & 0x400))  flags |= _EM_OVERFLOW;
    if (!(fpcr & 0x800))  flags |= _EM_UNDERFLOW;
    if (!(fpcr & 0x1000)) flags |= _EM_INEXACT;
    if (!(fpcr & 0x8000)) flags |= _EM_DENORMAL;
    switch (fpcr & 0xc00000)
    {
    case 0x400000: flags |= _RC_UP; break;
    case 0x800000: flags |= _RC_DOWN; break;
    case 0xc00000: flags |= _RC_CHOP; break;
    }
    flags = (flags & ~mask) | (newval & mask);
    fpcr &= ~0xc09f00ul;
    if (!(flags & _EM_INVALID)) fpcr |= 0x100;
    if (!(flags & _EM_ZERODIVIDE)) fpcr |= 0x200;
    if (!(flags & _EM_OVERFLOW)) fpcr |= 0x400;
    if (!(flags & _EM_UNDERFLOW)) fpcr |= 0x800;
    if (!(flags & _EM_INEXACT)) fpcr |= 0x1000;
    if (!(flags & _EM_DENORMAL)) fpcr |= 0x8000;
    switch (flags & _MCW_RC)
    {
    case _RC_CHOP: fpcr |= 0xc00000; break;
    case _RC_UP:   fpcr |= 0x400000; break;
    case _RC_DOWN: fpcr |= 0x800000; break;
    }
    __asm__ __volatile__( "msr fpcr, %0" :: "r" (fpcr) );
#else
    FIXME( "not implemented\n" );
#endif
    return flags;
}

/*********************************************************************
 *		_controlfp (MSVCRT.@)
 */
unsigned int CDECL _controlfp(unsigned int newval, unsigned int mask)
{
  return _control87( newval, mask & ~_EM_DENORMAL );
}

/*********************************************************************
 *		_set_controlfp (MSVCRT.@)
 */
void CDECL _set_controlfp( unsigned int newval, unsigned int mask )
{
    _controlfp( newval, mask );
}

/*********************************************************************
 *              _controlfp_s (MSVCRT.@)
 */
int CDECL _controlfp_s(unsigned int *cur, unsigned int newval, unsigned int mask)
{
    static const unsigned int all_flags = (_MCW_EM | _MCW_IC | _MCW_RC |
                                           _MCW_PC | _MCW_DN);
    unsigned int val;

    if (!MSVCRT_CHECK_PMT( !(newval & mask & ~all_flags) ))
    {
        if (cur) *cur = _controlfp( 0, 0 );  /* retrieve it anyway */
        return EINVAL;
    }
    val = _controlfp( newval, mask );
    if (cur) *cur = val;
    return 0;
}

#if _MSVCR_VER>=120
/*********************************************************************
 *		fegetenv (MSVCR120.@)
 */
int CDECL fegetenv(fenv_t *env)
{
    env->_Fe_ctl = _controlfp(0, 0) & (_EM_INEXACT | _EM_UNDERFLOW |
            _EM_OVERFLOW | _EM_ZERODIVIDE | _EM_INVALID | _RC_CHOP);
    env->_Fe_stat = _statusfp();
    return 0;
}

/*********************************************************************
 *      fetestexcept (MSVCR120.@)
 */
int CDECL fetestexcept(int flags)
{
    return _statusfp() & flags;
}

/*********************************************************************
 *      fesetexceptflag (MSVCR120.@)
 */
int CDECL fesetexceptflag(const fexcept_t *status, int excepts)
{
    fenv_t env;

    if(!excepts)
        return 0;

    fegetenv(&env);
    excepts &= FE_ALL_EXCEPT;
    env._Fe_stat &= ~excepts;
    env._Fe_stat |= (*status & excepts);
    return fesetenv(&env);
}

/*********************************************************************
 *      feclearexcept (MSVCR120.@)
 */
int CDECL feclearexcept(int flags)
{
    fenv_t env;

    fegetenv(&env);
    env._Fe_stat &= ~(flags & FE_ALL_EXCEPT);
    return fesetenv(&env);
}

/*********************************************************************
 *      fegetexceptflag (MSVCR120.@)
 */
int CDECL fegetexceptflag(fexcept_t *status, int excepts)
{
    *status = _statusfp() & excepts;
    return 0;
}
#endif

#if _MSVCR_VER>=140
/*********************************************************************
 *		__fpe_flt_rounds (UCRTBASE.@)
 */
int CDECL __fpe_flt_rounds(void)
{
    unsigned int fpc = _controlfp(0, 0) & _RC_CHOP;

    TRACE("()\n");

    switch(fpc) {
        case _RC_CHOP: return 0;
        case _RC_NEAR: return 1;
        case _RC_UP: return 2;
        default: return 3;
    }
}
#endif

#if _MSVCR_VER>=120

/*********************************************************************
 *		fegetround (MSVCR120.@)
 */
int CDECL fegetround(void)
{
    return _controlfp(0, 0) & _RC_CHOP;
}

/*********************************************************************
 *		fesetround (MSVCR120.@)
 */
int CDECL fesetround(int round_mode)
{
    if (round_mode & (~_RC_CHOP))
        return 1;
    _controlfp(round_mode, _RC_CHOP);
    return 0;
}

#endif /* _MSVCR_VER>=120 */

/*********************************************************************
 *		_copysign (MSVCRT.@)
 *
 * Copied from musl: src/math/copysign.c
 */
double CDECL _copysign( double x, double y )
{
    union { double f; UINT64 i; } ux = { x }, uy = { y };
    ux.i &= ~0ull >> 1;
    ux.i |= uy.i & 1ull << 63;
    return ux.f;
}

/*********************************************************************
 *		_finite (MSVCRT.@)
 */
int CDECL _finite(double num)
{
    union { double f; UINT64 i; } u = { num };
    return (u.i & ~0ull >> 1) < 0x7ffull << 52;
}

/*********************************************************************
 *		_fpreset (MSVCRT.@)
 */
void CDECL _fpreset(void)
{
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(__x86_64__))
    const unsigned int x86_cw = 0x27f;
    __asm__ __volatile__( "fninit; fldcw %0" : : "m" (x86_cw) );
    if (sse2_supported)
    {
        const unsigned long sse2_cw = 0x1f80;
        __asm__ __volatile__( "ldmxcsr %0" : : "m" (sse2_cw) );
    }
#else
    FIXME( "not implemented\n" );
#endif
}

#if _MSVCR_VER>=120
/*********************************************************************
 *              fesetenv (MSVCR120.@)
 */
int CDECL fesetenv(const fenv_t *env)
{
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__i386__) || defined(__x86_64__))
    struct {
        WORD control_word;
        WORD unused1;
        WORD status_word;
        WORD unused2;
        WORD tag_word;
        WORD unused3;
        DWORD instruction_pointer;
        WORD code_segment;
        WORD unused4;
        DWORD operand_addr;
        WORD data_segment;
        WORD unused5;
    } fenv;

    TRACE( "(%p)\n", env );

    if (!env->_Fe_ctl && !env->_Fe_stat) {
        _fpreset();
        return 0;
    }

    __asm__ __volatile__( "fnstenv %0" : "=m" (fenv) );

    fenv.control_word &= ~0xc3d;
    if (env->_Fe_ctl & _EM_INVALID) fenv.control_word |= 0x1;
    if (env->_Fe_ctl & _EM_ZERODIVIDE) fenv.control_word |= 0x4;
    if (env->_Fe_ctl & _EM_OVERFLOW) fenv.control_word |= 0x8;
    if (env->_Fe_ctl & _EM_UNDERFLOW) fenv.control_word |= 0x10;
    if (env->_Fe_ctl & _EM_INEXACT) fenv.control_word |= 0x20;
    switch (env->_Fe_ctl & _MCW_RC)
    {
        case _RC_UP|_RC_DOWN:   fenv.control_word |= 0xc00; break;
        case _RC_UP:            fenv.control_word |= 0x800; break;
        case _RC_DOWN:          fenv.control_word |= 0x400; break;
    }

    fenv.status_word &= ~0x3d;
    if (env->_Fe_stat & FE_INVALID) fenv.status_word |= 0x1;
    if (env->_Fe_stat & FE_DIVBYZERO) fenv.status_word |= 0x4;
    if (env->_Fe_stat & FE_OVERFLOW) fenv.status_word |= 0x8;
    if (env->_Fe_stat & FE_UNDERFLOW) fenv.status_word |= 0x10;
    if (env->_Fe_stat & FE_INEXACT) fenv.status_word |= 0x20;

    __asm__ __volatile__( "fldenv %0" : : "m" (fenv) : "st", "st(1)",
            "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)" );

    if (sse2_supported)
    {
        DWORD fpword;
        __asm__ __volatile__( "stmxcsr %0" : "=m" (fpword) );
        fpword &= ~0x7e80;
        if (env->_Fe_ctl & _EM_INVALID) fpword |= 0x80;
        if (env->_Fe_ctl & _EM_ZERODIVIDE) fpword |= 0x200;
        if (env->_Fe_ctl & _EM_OVERFLOW) fpword |= 0x400;
        if (env->_Fe_ctl & _EM_UNDERFLOW) fpword |= 0x800;
        if (env->_Fe_ctl & _EM_INEXACT) fpword |= 0x1000;
        switch (env->_Fe_ctl & _MCW_RC)
        {
            case _RC_CHOP: fpword |= 0x6000; break;
            case _RC_UP:   fpword |= 0x4000; break;
            case _RC_DOWN: fpword |= 0x2000; break;
        }
        __asm__ __volatile__( "ldmxcsr %0" : : "m" (fpword) );
    }

    return 0;
#else
    FIXME( "not implemented\n" );
#endif
    return 1;
}
#endif

/*********************************************************************
 *		_isnan (MSVCRT.@)
 */
int CDECL _isnan(double num)
{
    union { double f; UINT64 i; } u = { num };
    return (u.i & ~0ull >> 1) > 0x7ffull << 52;
}

static double pzero(double x)
{
    static const double pR8[6] = { /* for x in [inf, 8]=1/[0,0.125] */
        0.00000000000000000000e+00,
        -7.03124999999900357484e-02,
        -8.08167041275349795626e+00,
        -2.57063105679704847262e+02,
        -2.48521641009428822144e+03,
        -5.25304380490729545272e+03,
    }, pS8[5] = {
        1.16534364619668181717e+02,
        3.83374475364121826715e+03,
        4.05978572648472545552e+04,
        1.16752972564375915681e+05,
        4.76277284146730962675e+04,
    }, pR5[6] = { /* for x in [8,4.5454]=1/[0.125,0.22001] */
        -1.14125464691894502584e-11,
        -7.03124940873599280078e-02,
        -4.15961064470587782438e+00,
        -6.76747652265167261021e+01,
        -3.31231299649172967747e+02,
        -3.46433388365604912451e+02,
    }, pS5[5] = {
        6.07539382692300335975e+01,
        1.05125230595704579173e+03,
        5.97897094333855784498e+03,
        9.62544514357774460223e+03,
        2.40605815922939109441e+03,
    }, pR3[6] = {/* for x in [4.547,2.8571]=1/[0.2199,0.35001] */
        -2.54704601771951915620e-09,
        -7.03119616381481654654e-02,
        -2.40903221549529611423e+00,
        -2.19659774734883086467e+01,
        -5.80791704701737572236e+01,
        -3.14479470594888503854e+01,
    }, pS3[5] = {
        3.58560338055209726349e+01,
        3.61513983050303863820e+02,
        1.19360783792111533330e+03,
        1.12799679856907414432e+03,
        1.73580930813335754692e+02,
    }, pR2[6] = {/* for x in [2.8570,2]=1/[0.3499,0.5] */
        -8.87534333032526411254e-08,
        -7.03030995483624743247e-02,
        -1.45073846780952986357e+00,
        -7.63569613823527770791e+00,
        -1.11931668860356747786e+01,
        -3.23364579351335335033e+00,
    }, pS2[5] = {
        2.22202997532088808441e+01,
        1.36206794218215208048e+02,
        2.70470278658083486789e+02,
        1.53875394208320329881e+02,
        1.46576176948256193810e+01,
    };

    const double *p, *q;
    double z, r, s;
    uint32_t ix;

    ix = *(ULONGLONG*)&x >> 32;
    ix &= 0x7fffffff;
    if (ix >= 0x40200000) {
        p = pR8;
        q = pS8;
    } else if (ix >= 0x40122E8B) {
        p = pR5;
        q = pS5;
    } else if (ix >= 0x4006DB6D) {
        p = pR3;
        q = pS3;
    } else /*ix >= 0x40000000*/ {
        p = pR2;
        q = pS2;
    }

    z = 1.0 / (x * x);
    r = p[0] + z * (p[1] + z * (p[2] + z * (p[3] + z * (p[4] + z * p[5]))));
    s = 1.0 + z * (q[0] + z * (q[1] + z * (q[2] + z * (q[3] + z * q[4]))));
    return 1.0 + r / s;
}

static double qzero(double x)
{
    static const double qR8[6] = { /* for x in [inf, 8]=1/[0,0.125] */
        0.00000000000000000000e+00,
        7.32421874999935051953e-02,
        1.17682064682252693899e+01,
        5.57673380256401856059e+02,
        8.85919720756468632317e+03,
        3.70146267776887834771e+04,
    }, qS8[6] = {
        1.63776026895689824414e+02,
        8.09834494656449805916e+03,
        1.42538291419120476348e+05,
        8.03309257119514397345e+05,
        8.40501579819060512818e+05,
        -3.43899293537866615225e+05,
    }, qR5[6] = { /* for x in [8,4.5454]=1/[0.125,0.22001] */
        1.84085963594515531381e-11,
        7.32421766612684765896e-02,
        5.83563508962056953777e+00,
        1.35111577286449829671e+02,
        1.02724376596164097464e+03,
        1.98997785864605384631e+03,
    }, qS5[6] = {
        8.27766102236537761883e+01,
        2.07781416421392987104e+03,
        1.88472887785718085070e+04,
        5.67511122894947329769e+04,
        3.59767538425114471465e+04,
        -5.35434275601944773371e+03,
    }, qR3[6] = {/* for x in [4.547,2.8571]=1/[0.2199,0.35001] */
        4.37741014089738620906e-09,
        7.32411180042911447163e-02,
        3.34423137516170720929e+00,
        4.26218440745412650017e+01,
        1.70808091340565596283e+02,
        1.66733948696651168575e+02,
    }, qS3[6] = {
        4.87588729724587182091e+01,
        7.09689221056606015736e+02,
        3.70414822620111362994e+03,
        6.46042516752568917582e+03,
        2.51633368920368957333e+03,
        -1.49247451836156386662e+02,
    }, qR2[6] = {/* for x in [2.8570,2]=1/[0.3499,0.5] */
        1.50444444886983272379e-07,
        7.32234265963079278272e-02,
        1.99819174093815998816e+00,
        1.44956029347885735348e+01,
        3.16662317504781540833e+01,
        1.62527075710929267416e+01,
    }, qS2[6] = {
        3.03655848355219184498e+01,
        2.69348118608049844624e+02,
        8.44783757595320139444e+02,
        8.82935845112488550512e+02,
        2.12666388511798828631e+02,
        -5.31095493882666946917e+00,
    };

    const double *p, *q;
    double s, r, z;
    unsigned int ix;

    ix = *(ULONGLONG*)&x >> 32;
    ix &= 0x7fffffff;
    if (ix >= 0x40200000) {
        p = qR8;
        q = qS8;
    } else if (ix >= 0x40122E8B) {
        p = qR5;
        q = qS5;
    } else if (ix >= 0x4006DB6D) {
        p = qR3;
        q = qS3;
    } else /*ix >= 0x40000000*/ {
        p = qR2;
        q = qS2;
    }

    z = 1.0 / (x * x);
    r = p[0] + z * (p[1] + z * (p[2] + z * (p[3] + z * (p[4] + z * p[5]))));
    s = 1.0 + z * (q[0] + z * (q[1] + z * (q[2] + z * (q[3] + z * (q[4] + z * q[5])))));
    return (-0.125 + r / s) / x;
}

/* j0 and y0 approximation for |x|>=2 */
static double j0_y0_approx(unsigned int ix, double x, BOOL y0)
{
    static const double invsqrtpi = 5.64189583547756279280e-01;

    double s, c, ss, cc, z;

    s = sin(x);
    c = cos(x);
    if (y0) c = -c;
    cc = s + c;
    /* avoid overflow in 2*x, big ulp error when x>=0x1p1023 */
    if (ix < 0x7fe00000) {
        ss = s - c;
        z = -cos(2 * x);
        if (s * c < 0) cc = z / ss;
        else ss = z / cc;
        if (ix < 0x48000000) {
            if (y0) ss = -ss;
            cc = pzero(x) * cc - qzero(x) * ss;
        }
    }
    return invsqrtpi * cc / sqrt(x);
}

/*********************************************************************
 *		_j0 (MSVCRT.@)
 *
 * Copied from musl: src/math/j0.c
 */
double CDECL _j0(double x)
{
    static const double R02 =  1.56249999999999947958e-02,
            R03 = -1.89979294238854721751e-04,
            R04 =  1.82954049532700665670e-06,
            R05 = -4.61832688532103189199e-09,
            S01 =  1.56191029464890010492e-02,
            S02 =  1.16926784663337450260e-04,
            S03 =  5.13546550207318111446e-07,
            S04 =  1.16614003333790000205e-09;

    double z, r, s;
    unsigned int ix;

    ix = *(ULONGLONG*)&x >> 32;
    ix &= 0x7fffffff;

    /* j0(+-inf)=0, j0(nan)=nan */
    if (ix >= 0x7ff00000)
        return math_error(_DOMAIN, "_j0", x, 0, 1 / (x * x));
    x = fabs(x);

    if (ix >= 0x40000000) {  /* |x| >= 2 */
        /* large ulp error near zeros: 2.4, 5.52, 8.6537,.. */
        return j0_y0_approx(ix, x, FALSE);
    }

    if (ix >= 0x3f200000) {  /* |x| >= 2**-13 */
        /* up to 4ulp error close to 2 */
        z = x * x;
        r = z * (R02 + z * (R03 + z * (R04 + z * R05)));
        s = 1 + z * (S01 + z * (S02 + z * (S03 + z * S04)));
        return (1 + x / 2) * (1 - x / 2) + z * (r / s);
    }

    /* 1 - x*x/4 */
    /* prevent underflow */
    /* inexact should be raised when x!=0, this is not done correctly */
    if (ix >= 0x38000000)  /* |x| >= 2**-127 */
        x = 0.25 * x * x;
    return 1 - x;
}

static double pone(double x)
{
    static const double pr8[6] = { /* for x in [inf, 8]=1/[0,0.125] */
        0.00000000000000000000e+00,
        1.17187499999988647970e-01,
        1.32394806593073575129e+01,
        4.12051854307378562225e+02,
        3.87474538913960532227e+03,
        7.91447954031891731574e+03,
    }, ps8[5] = {
        1.14207370375678408436e+02,
        3.65093083420853463394e+03,
        3.69562060269033463555e+04,
        9.76027935934950801311e+04,
        3.08042720627888811578e+04,
    }, pr5[6] = { /* for x in [8,4.5454]=1/[0.125,0.22001] */
        1.31990519556243522749e-11,
        1.17187493190614097638e-01,
        6.80275127868432871736e+00,
        1.08308182990189109773e+02,
        5.17636139533199752805e+02,
        5.28715201363337541807e+02,
    }, ps5[5] = {
        5.92805987221131331921e+01,
        9.91401418733614377743e+02,
        5.35326695291487976647e+03,
        7.84469031749551231769e+03,
        1.50404688810361062679e+03,
    }, pr3[6] = {
        3.02503916137373618024e-09,
        1.17186865567253592491e-01,
        3.93297750033315640650e+00,
        3.51194035591636932736e+01,
        9.10550110750781271918e+01,
        4.85590685197364919645e+01,
    }, ps3[5] = {
        3.47913095001251519989e+01,
        3.36762458747825746741e+02,
        1.04687139975775130551e+03,
        8.90811346398256432622e+02,
        1.03787932439639277504e+02,
    }, pr2[6] = { /* for x in [2.8570,2]=1/[0.3499,0.5] */
        1.07710830106873743082e-07,
        1.17176219462683348094e-01,
        2.36851496667608785174e+00,
        1.22426109148261232917e+01,
        1.76939711271687727390e+01,
        5.07352312588818499250e+00,
    }, ps2[5] = {
        2.14364859363821409488e+01,
        1.25290227168402751090e+02,
        2.32276469057162813669e+02,
        1.17679373287147100768e+02,
        8.36463893371618283368e+00,
    };

    const double *p, *q;
    double z, r, s;
    unsigned int ix;

    ix = *(ULONGLONG*)&x >> 32;
    ix &= 0x7fffffff;
    if (ix >= 0x40200000) {
        p = pr8;
        q = ps8;
    } else if (ix >= 0x40122E8B) {
        p = pr5;
        q = ps5;
    } else if (ix >= 0x4006DB6D) {
        p = pr3;
        q = ps3;
    } else /*ix >= 0x40000000*/ {
        p = pr2;
        q = ps2;
    }
    z = 1.0 / (x * x);
    r = p[0] + z * (p[1] + z * (p[2] + z * (p[3] + z * (p[4] + z * p[5]))));
    s = 1.0 + z * (q[0] + z * (q[1] + z * (q[2] + z * (q[3] + z * q[4]))));
    return 1.0 + r / s;
}

static double qone(double x)
{
    static const double qr8[6] = { /* for x in [inf, 8]=1/[0,0.125] */
        0.00000000000000000000e+00,
        -1.02539062499992714161e-01,
        -1.62717534544589987888e+01,
        -7.59601722513950107896e+02,
        -1.18498066702429587167e+04,
        -4.84385124285750353010e+04,
    }, qs8[6] = {
        1.61395369700722909556e+02,
        7.82538599923348465381e+03,
        1.33875336287249578163e+05,
        7.19657723683240939863e+05,
        6.66601232617776375264e+05,
        -2.94490264303834643215e+05,
    }, qr5[6] = { /* for x in [8,4.5454]=1/[0.125,0.22001] */
        -2.08979931141764104297e-11,
        -1.02539050241375426231e-01,
        -8.05644828123936029840e+00,
        -1.83669607474888380239e+02,
        -1.37319376065508163265e+03,
        -2.61244440453215656817e+03,
    }, qs5[6] = {
        8.12765501384335777857e+01,
        1.99179873460485964642e+03,
        1.74684851924908907677e+04,
        4.98514270910352279316e+04,
        2.79480751638918118260e+04,
        -4.71918354795128470869e+03,
    }, qr3[6] = {
        -5.07831226461766561369e-09,
        -1.02537829820837089745e-01,
        -4.61011581139473403113e+00,
        -5.78472216562783643212e+01,
        -2.28244540737631695038e+02,
        -2.19210128478909325622e+02,
    }, qs3[6] = {
        4.76651550323729509273e+01,
        6.73865112676699709482e+02,
        3.38015286679526343505e+03,
        5.54772909720722782367e+03,
        1.90311919338810798763e+03,
        -1.35201191444307340817e+02,
    }, qr2[6] = { /* for x in [2.8570,2]=1/[0.3499,0.5] */
        -1.78381727510958865572e-07,
        -1.02517042607985553460e-01,
        -2.75220568278187460720e+00,
        -1.96636162643703720221e+01,
        -4.23253133372830490089e+01,
        -2.13719211703704061733e+01,
    }, qs2[6] = {
        2.95333629060523854548e+01,
        2.52981549982190529136e+02,
        7.57502834868645436472e+02,
        7.39393205320467245656e+02,
        1.55949003336666123687e+02,
        -4.95949898822628210127e+00,
    };

    const double *p, *q;
    double s, r, z;
    unsigned int ix;

    ix = *(ULONGLONG*)&x >> 32;
    ix &= 0x7fffffff;
    if (ix >= 0x40200000) {
        p = qr8;
        q = qs8;
    } else if (ix >= 0x40122E8B) {
        p = qr5;
        q = qs5;
    } else if (ix >= 0x4006DB6D) {
        p = qr3;
        q = qs3;
    } else /*ix >= 0x40000000*/ {
        p = qr2;
        q = qs2;
    }
    z = 1.0 / (x * x);
    r = p[0] + z * (p[1] + z * (p[2] + z * (p[3] + z * (p[4] + z * p[5]))));
    s = 1.0 + z * (q[0] + z * (q[1] + z * (q[2] + z * (q[3] + z * (q[4] + z * q[5])))));
    return (0.375 + r / s) / x;
}

static double j1_y1_approx(unsigned int ix, double x, BOOL y1, int sign)
{
    static const double invsqrtpi = 5.64189583547756279280e-01;

    double z, s, c, ss, cc;

    s = sin(x);
    if (y1) s = -s;
    c = cos(x);
    cc = s - c;
    if (ix < 0x7fe00000) {
        ss = -s - c;
        z = cos(2 * x);
        if (s * c > 0) cc = z / ss;
        else ss = z / cc;
        if (ix < 0x48000000) {
            if (y1)
                ss = -ss;
            cc = pone(x) * cc - qone(x) * ss;
        }
    }
    if (sign)
        cc = -cc;
    return invsqrtpi * cc / sqrt(x);
}

/*********************************************************************
 *		_j1 (MSVCRT.@)
 *
 * Copied from musl: src/math/j1.c
 */
double CDECL _j1(double x)
{
    static const double r00 = -6.25000000000000000000e-02,
        r01 =  1.40705666955189706048e-03,
        r02 = -1.59955631084035597520e-05,
        r03 =  4.96727999609584448412e-08,
        s01 =  1.91537599538363460805e-02,
        s02 =  1.85946785588630915560e-04,
        s03 =  1.17718464042623683263e-06,
        s04 =  5.04636257076217042715e-09,
        s05 =  1.23542274426137913908e-11;

    double z, r, s;
    unsigned int ix;
    int sign;

    ix = *(ULONGLONG*)&x >> 32;
    sign = ix >> 31;
    ix &= 0x7fffffff;
    if (ix >= 0x7ff00000)
        return math_error(isnan(x) ? 0 : _DOMAIN, "_j1", x, 0, 1 / (x * x));
    if (ix >= 0x40000000)  /* |x| >= 2 */
        return j1_y1_approx(ix, fabs(x), FALSE, sign);
    if (ix >= 0x38000000) {  /* |x| >= 2**-127 */
        z = x * x;
        r = z * (r00 + z * (r01 + z * (r02 + z * r03)));
        s = 1 + z * (s01 + z * (s02 + z * (s03 + z * (s04 + z * s05))));
        z = r / s;
    } else {
        /* avoid underflow, raise inexact if x!=0 */
        z = x;
    }
    return (0.5 + z) * x;
}

/*********************************************************************
 *		_jn (MSVCRT.@)
 *
 * Copied from musl: src/math/jn.c
 */
double CDECL _jn(int n, double x)
{
    static const double invsqrtpi = 5.64189583547756279280e-01;

    unsigned int ix, lx;
    int nm1, i, sign;
    double a, b, temp;

    ix = *(ULONGLONG*)&x >> 32;
    lx = *(ULONGLONG*)&x;
    sign = ix >> 31;
    ix &= 0x7fffffff;

    if ((ix | (lx | -lx) >> 31) > 0x7ff00000) /* nan */
        return x;

    if (n == 0)
        return _j0(x);
    if (n < 0) {
        nm1 = -(n + 1);
        x = -x;
        sign ^= 1;
    } else {
        nm1 = n-1;
    }
    if (nm1 == 0)
        return j1(x);

    sign &= n;  /* even n: 0, odd n: signbit(x) */
    x = fabs(x);
    if ((ix | lx) == 0 || ix == 0x7ff00000)  /* if x is 0 or inf */
        b = 0.0;
    else if (nm1 < x) {
        if (ix >= 0x52d00000) { /* x > 2**302 */
            switch(nm1 & 3) {
            case 0:
                temp = -cos(x) + sin(x);
                break;
            case 1:
                temp = -cos(x) - sin(x);
                break;
            case 2:
                temp =  cos(x) - sin(x);
                break;
            default:
                temp =  cos(x) + sin(x);
                break;
            }
            b = invsqrtpi * temp / sqrt(x);
        } else {
            a = _j0(x);
            b = _j1(x);
            for (i = 0; i < nm1; ) {
                i++;
                temp = b;
                b = b * (2.0 * i / x) - a; /* avoid underflow */
                a = temp;
            }
        }
    } else {
        if (ix < 0x3e100000) { /* x < 2**-29 */
            if (nm1 > 32)  /* underflow */
                b = 0.0;
            else {
                temp = x * 0.5;
                b = temp;
                a = 1.0;
                for (i = 2; i <= nm1 + 1; i++) {
                    a *= (double)i; /* a = n! */
                    b *= temp;      /* b = (x/2)^n */
                }
                b = b / a;
            }
        } else {
            double t, q0, q1, w, h, z, tmp, nf;
            int k;

            nf = nm1 + 1.0;
            w = 2 * nf / x;
            h = 2 / x;
            z = w + h;
            q0 = w;
            q1 = w * z - 1.0;
            k = 1;
            while (q1 < 1.0e9) {
                k += 1;
                z += h;
                tmp = z * q1 - q0;
                q0 = q1;
                q1 = tmp;
            }
            for (t = 0.0, i = k; i >= 0; i--)
                t = 1 / (2 * (i + nf) / x - t);
            a = t;
            b = 1.0;
            tmp = nf * log(fabs(w));
            if (tmp < 7.09782712893383973096e+02) {
                for (i = nm1; i > 0; i--) {
                    temp = b;
                    b = b * (2.0 * i) / x - a;
                    a = temp;
                }
            } else {
                for (i = nm1; i > 0; i--) {
                    temp = b;
                    b = b * (2.0 * i) / x - a;
                    a = temp;
                    /* scale b to avoid spurious overflow */
                    if (b > 0x1p500) {
                        a /= b;
                        t /= b;
                        b  = 1.0;
                    }
                }
            }
            z = j0(x);
            w = j1(x);
            if (fabs(z) >= fabs(w))
                b = t * z / b;
            else
                b = t * w / a;
        }
    }
    return sign ? -b : b;
}

/*********************************************************************
 *		_y0 (MSVCRT.@)
 */
double CDECL _y0(double x)
{
    static const double tpi = 6.36619772367581382433e-01,
        u00  = -7.38042951086872317523e-02,
        u01  =  1.76666452509181115538e-01,
        u02  = -1.38185671945596898896e-02,
        u03  =  3.47453432093683650238e-04,
        u04  = -3.81407053724364161125e-06,
        u05  =  1.95590137035022920206e-08,
        u06  = -3.98205194132103398453e-11,
        v01  =  1.27304834834123699328e-02,
        v02  =  7.60068627350353253702e-05,
        v03  =  2.59150851840457805467e-07,
        v04  =  4.41110311332675467403e-10;

    double z, u, v;
    unsigned int ix, lx;

    ix = *(ULONGLONG*)&x >> 32;
    lx = *(ULONGLONG*)&x;

    /* y0(nan)=nan, y0(<0)=nan, y0(0)=-inf, y0(inf)=0 */
    if ((ix << 1 | lx) == 0)
        return math_error(_OVERFLOW, "_y0", x, 0, -INFINITY);
    if (isnan(x))
        return x;
    if (ix >> 31)
        return math_error(_DOMAIN, "_y0", x, 0, 0 / (x - x));
    if (ix >= 0x7ff00000)
        return 1 / x;

    if (ix >= 0x40000000) {  /* x >= 2 */
        /* large ulp errors near zeros: 3.958, 7.086,.. */
        return j0_y0_approx(ix, x, TRUE);
    }

    if (ix >= 0x3e400000) {  /* x >= 2**-27 */
        /* large ulp error near the first zero, x ~= 0.89 */
        z = x * x;
        u = u00 + z * (u01 + z * (u02 + z * (u03 + z * (u04 + z * (u05 + z * u06)))));
        v = 1.0 + z * (v01 + z * (v02 + z * (v03 + z * v04)));
        return u / v + tpi * (j0(x) * log(x));
    }
    return u00 + tpi * log(x);
}

/*********************************************************************
 *		_y1 (MSVCRT.@)
 */
double CDECL _y1(double x)
{
    static const double tpi = 6.36619772367581382433e-01,
        u00 =  -1.96057090646238940668e-01,
        u01 = 5.04438716639811282616e-02,
        u02 = -1.91256895875763547298e-03,
        u03 = 2.35252600561610495928e-05,
        u04 = -9.19099158039878874504e-08,
        v00 = 1.99167318236649903973e-02,
        v01 = 2.02552581025135171496e-04,
        v02 = 1.35608801097516229404e-06,
        v03 = 6.22741452364621501295e-09,
        v04 = 1.66559246207992079114e-11;

    double z, u, v;
    unsigned int ix, lx;

    ix = *(ULONGLONG*)&x >> 32;
    lx = *(ULONGLONG*)&x;

    /* y1(nan)=nan, y1(<0)=nan, y1(0)=-inf, y1(inf)=0 */
    if ((ix << 1 | lx) == 0)
        return math_error(_OVERFLOW, "_y1", x, 0, -INFINITY);
    if (isnan(x))
        return x;
    if (ix >> 31)
        return math_error(_DOMAIN, "_y1", x, 0, 0 / (x - x));
    if (ix >= 0x7ff00000)
        return 1 / x;

    if (ix >= 0x40000000)  /* x >= 2 */
        return j1_y1_approx(ix, x, TRUE, 0);
    if (ix < 0x3c900000)  /* x < 2**-54 */
        return -tpi / x;
    z = x * x;
    u = u00 + z * (u01 + z * (u02 + z * (u03 + z * u04)));
    v = 1 + z * (v00 + z * (v01 + z * (v02 + z * (v03 + z * v04))));
    return x * (u / v) + tpi * (j1(x) * log(x) - 1 / x);
}

/*********************************************************************
 *		_yn (MSVCRT.@)
 *
 * Copied from musl: src/math/jn.c
 */
double CDECL _yn(int n, double x)
{
    static const double invsqrtpi = 5.64189583547756279280e-01;

    unsigned int ix, lx, ib;
    int nm1, sign, i;
    double a, b, temp;

    ix = *(ULONGLONG*)&x >> 32;
    lx = *(ULONGLONG*)&x;
    sign = ix >> 31;
    ix &= 0x7fffffff;

    if ((ix | (lx | -lx) >> 31) > 0x7ff00000) /* nan */
        return x;
    if (sign && (ix | lx) != 0) /* x < 0 */
        return math_error(_DOMAIN, "_y1", x, 0, 0 / (x - x));
    if (ix == 0x7ff00000)
        return 0.0;

    if (n == 0)
        return y0(x);
    if (n < 0) {
        nm1 = -(n + 1);
        sign = n & 1;
    } else {
        nm1 = n - 1;
        sign = 0;
    }
    if (nm1 == 0)
        return sign ? -y1(x) : y1(x);

    if (ix >= 0x52d00000) { /* x > 2**302 */
        switch(nm1 & 3) {
        case 0:
            temp = -sin(x) - cos(x);
            break;
        case 1:
            temp = -sin(x) + cos(x);
            break;
        case 2:
            temp = sin(x) + cos(x);
            break;
        default:
            temp = sin(x) - cos(x);
            break;
        }
        b = invsqrtpi * temp / sqrt(x);
    } else {
        a = y0(x);
        b = y1(x);
        /* quit if b is -inf */
        ib = *(ULONGLONG*)&b >> 32;
        for (i = 0; i < nm1 && ib != 0xfff00000;) {
            i++;
            temp = b;
            b = (2.0 * i / x) * b - a;
            ib = *(ULONGLONG*)&b >> 32;
            a = temp;
        }
    }
    return sign ? -b : b;
}

#if _MSVCR_VER>=120

/*********************************************************************
 *		_nearbyint (MSVCR120.@)
 */
double CDECL nearbyint(double num)
{
    return unix_funcs->nearbyint( num );
}

/*********************************************************************
 *		_nearbyintf (MSVCR120.@)
 */
float CDECL nearbyintf(float num)
{
    return unix_funcs->nearbyintf( num );
}

/*********************************************************************
 *              nexttoward (MSVCR120.@)
 */
double CDECL MSVCRT_nexttoward(double num, double next)
{
    double ret = unix_funcs->nexttoward(num, next);
    if (!(_fpclass(ret) & (_FPCLASS_PN | _FPCLASS_NN
            | _FPCLASS_SNAN | _FPCLASS_QNAN)) && !isinf(num))
    {
        *_errno() = ERANGE;
    }
    return ret;
}

/*********************************************************************
 *              nexttowardf (MSVCR120.@)
 */
float CDECL MSVCRT_nexttowardf(float num, double next)
{
    float ret = unix_funcs->nexttowardf( num, next );
    if (!(_fpclass(ret) & (_FPCLASS_PN | _FPCLASS_NN
            | _FPCLASS_SNAN | _FPCLASS_QNAN)) && !isinf(num))
    {
        *_errno() = ERANGE;
    }
    return ret;
}

#endif /* _MSVCR_VER>=120 */

/*********************************************************************
 *		_nextafter (MSVCRT.@)
 */
double CDECL _nextafter(double num, double next)
{
  double retval;
  if (!isfinite(num) || !isfinite(next)) *_errno() = EDOM;
  retval = unix_funcs->nextafter(num,next);
  return retval;
}

/*********************************************************************
 *		_ecvt (MSVCRT.@)
 */
char * CDECL _ecvt( double number, int ndigits, int *decpt, int *sign )
{
    int prec, len;
    thread_data_t *data = msvcrt_get_thread_data();
    /* FIXME: check better for overflow (native supports over 300 chars) */
    ndigits = min( ndigits, 80 - 8); /* 8 : space for sign, dec point, "e",
                                      * 4 for exponent and one for
                                      * terminating '\0' */
    if (!data->efcvt_buffer)
        data->efcvt_buffer = malloc( 80 ); /* ought to be enough */

    /* handle cases with zero ndigits or less */
    prec = ndigits;
    if( prec < 1) prec = 2;
    len = _snprintf(data->efcvt_buffer, 80, "%.*le", prec - 1, number);

    if (data->efcvt_buffer[0] == '-') {
        memmove( data->efcvt_buffer, data->efcvt_buffer + 1, len-- );
        *sign = 1;
    } else *sign = 0;

    /* take the decimal "point away */
    if( prec != 1)
        memmove( data->efcvt_buffer + 1, data->efcvt_buffer + 2, len - 1 );
    /* take the exponential "e" out */
    data->efcvt_buffer[ prec] = '\0';
    /* read the exponent */
    sscanf( data->efcvt_buffer + prec + 1, "%d", decpt);
    (*decpt)++;
    /* adjust for some border cases */
    if( data->efcvt_buffer[0] == '0')/* value is zero */
        *decpt = 0;
    /* handle cases with zero ndigits or less */
    if( ndigits < 1){
        if( data->efcvt_buffer[ 0] >= '5')
            (*decpt)++;
        data->efcvt_buffer[ 0] = '\0';
    }
    TRACE("out=\"%s\"\n",data->efcvt_buffer);
    return data->efcvt_buffer;
}

/*********************************************************************
 *		_ecvt_s (MSVCRT.@)
 */
int CDECL _ecvt_s( char *buffer, size_t length, double number, int ndigits, int *decpt, int *sign )
{
    int prec, len;
    char *result;

    if (!MSVCRT_CHECK_PMT(buffer != NULL)) return EINVAL;
    if (!MSVCRT_CHECK_PMT(decpt != NULL)) return EINVAL;
    if (!MSVCRT_CHECK_PMT(sign != NULL)) return EINVAL;
    if (!MSVCRT_CHECK_PMT_ERR( length > 2, ERANGE )) return ERANGE;
    if (!MSVCRT_CHECK_PMT_ERR(ndigits < (int)length - 1, ERANGE )) return ERANGE;

    /* handle cases with zero ndigits or less */
    prec = ndigits;
    if( prec < 1) prec = 2;
    result = malloc(prec + 8);

    len = _snprintf(result, prec + 8, "%.*le", prec - 1, number);
    if (result[0] == '-') {
        memmove( result, result + 1, len-- );
        *sign = 1;
    } else *sign = 0;

    /* take the decimal "point away */
    if( prec != 1)
        memmove( result + 1, result + 2, len - 1 );
    /* take the exponential "e" out */
    result[ prec] = '\0';
    /* read the exponent */
    sscanf( result + prec + 1, "%d", decpt);
    (*decpt)++;
    /* adjust for some border cases */
    if( result[0] == '0')/* value is zero */
        *decpt = 0;
    /* handle cases with zero ndigits or less */
    if( ndigits < 1){
        if( result[ 0] >= '5')
            (*decpt)++;
        result[ 0] = '\0';
    }
    memcpy( buffer, result, max(ndigits + 1, 1) );
    free( result );
    return 0;
}

/***********************************************************************
 *		_fcvt  (MSVCRT.@)
 */
char * CDECL _fcvt( double number, int ndigits, int *decpt, int *sign )
{
    thread_data_t *data = msvcrt_get_thread_data();
    int stop, dec1, dec2;
    char *ptr1, *ptr2, *first;
    char buf[80]; /* ought to be enough */
    char decimal_separator = get_locinfo()->lconv->decimal_point[0];

    if (!data->efcvt_buffer)
        data->efcvt_buffer = malloc( 80 ); /* ought to be enough */

    stop = _snprintf(buf, 80, "%.*f", ndigits < 0 ? 0 : ndigits, number);
    ptr1 = buf;
    ptr2 = data->efcvt_buffer;
    first = NULL;
    dec1 = 0;
    dec2 = 0;

    if (*ptr1 == '-') {
        *sign = 1;
        ptr1++;
    } else *sign = 0;

    /* For numbers below the requested resolution, work out where
       the decimal point will be rather than finding it in the string */
    if (number < 1.0 && number > 0.0) {
	dec2 = log10(number + 1e-10);
	if (-dec2 <= ndigits) dec2 = 0;
    }

    /* If requested digits is zero or less, we will need to truncate
     * the returned string */
    if (ndigits < 1) {
	stop += ndigits;
    }

    while (*ptr1 == '0') ptr1++; /* Skip leading zeroes */
    while (*ptr1 != '\0' && *ptr1 != decimal_separator) {
	if (!first) first = ptr2;
	if ((ptr1 - buf) < stop) {
	    *ptr2++ = *ptr1++;
	} else {
	    ptr1++;
	}
	dec1++;
    }

    if (ndigits > 0) {
	ptr1++;
	if (!first) {
	    while (*ptr1 == '0') { /* Process leading zeroes */
		*ptr2++ = *ptr1++;
		dec1--;
	    }
	}
	while (*ptr1 != '\0') {
	    if (!first) first = ptr2;
	    *ptr2++ = *ptr1++;
	}
    }

    *ptr2 = '\0';

    /* We never found a non-zero digit, then our number is either
     * smaller than the requested precision, or 0.0 */
    if (!first) {
	if (number > 0.0) {
	    first = ptr2;
	} else {
	    first = data->efcvt_buffer;
	    dec1 = 0;
	}
    }

    *decpt = dec2 ? dec2 : dec1;
    return first;
}

/***********************************************************************
 *		_fcvt_s  (MSVCRT.@)
 */
int CDECL _fcvt_s(char* outbuffer, size_t size, double number, int ndigits, int *decpt, int *sign)
{
    int stop, dec1, dec2;
    char *ptr1, *ptr2, *first;
    char buf[80]; /* ought to be enough */
    char decimal_separator = get_locinfo()->lconv->decimal_point[0];

    if (!outbuffer || !decpt || !sign || size == 0)
    {
        *_errno() = EINVAL;
        return EINVAL;
    }

    stop = _snprintf(buf, 80, "%.*f", ndigits < 0 ? 0 : ndigits, number);
    ptr1 = buf;
    ptr2 = outbuffer;
    first = NULL;
    dec1 = 0;
    dec2 = 0;

    if (*ptr1 == '-') {
        *sign = 1;
        ptr1++;
    } else *sign = 0;

    /* For numbers below the requested resolution, work out where
       the decimal point will be rather than finding it in the string */
    if (number < 1.0 && number > 0.0) {
	dec2 = log10(number + 1e-10);
	if (-dec2 <= ndigits) dec2 = 0;
    }

    /* If requested digits is zero or less, we will need to truncate
     * the returned string */
    if (ndigits < 1) {
	stop += ndigits;
    }

    while (*ptr1 == '0') ptr1++; /* Skip leading zeroes */
    while (*ptr1 != '\0' && *ptr1 != decimal_separator) {
	if (!first) first = ptr2;
	if ((ptr1 - buf) < stop) {
	    if (size > 1) {
                *ptr2++ = *ptr1++;
                size--;
            }
	} else {
	    ptr1++;
	}
	dec1++;
    }

    if (ndigits > 0) {
	ptr1++;
	if (!first) {
	    while (*ptr1 == '0') { /* Process leading zeroes */
                if (number == 0.0 && size > 1) {
                    *ptr2++ = '0';
                    size--;
                }
                ptr1++;
		dec1--;
	    }
	}
	while (*ptr1 != '\0') {
	    if (!first) first = ptr2;
	    if (size > 1) {
                *ptr2++ = *ptr1++;
                size--;
            }
	}
    }

    *ptr2 = '\0';

    /* We never found a non-zero digit, then our number is either
     * smaller than the requested precision, or 0.0 */
    if (!first && (number <= 0.0))
        dec1 = 0;

    *decpt = dec2 ? dec2 : dec1;
    return 0;
}

/***********************************************************************
 *		_gcvt  (MSVCRT.@)
 */
char * CDECL _gcvt( double number, int ndigit, char *buff )
{
    if(!buff) {
        *_errno() = EINVAL;
        return NULL;
    }

    if(ndigit < 0) {
        *_errno() = ERANGE;
        return NULL;
    }

    sprintf(buff, "%.*g", ndigit, number);
    return buff;
}

/***********************************************************************
 *              _gcvt_s  (MSVCRT.@)
 */
int CDECL _gcvt_s(char *buff, size_t size, double number, int digits)
{
    int len;

    if(!buff) {
        *_errno() = EINVAL;
        return EINVAL;
    }

    if( digits<0 || digits>=size) {
        if(size)
            buff[0] = '\0';

        *_errno() = ERANGE;
        return ERANGE;
    }

    len = _scprintf("%.*g", digits, number);
    if(len > size) {
        buff[0] = '\0';
        *_errno() = ERANGE;
        return ERANGE;
    }

    sprintf(buff, "%.*g", digits, number);
    return 0;
}

#include <stdlib.h> /* div_t, ldiv_t */

/*********************************************************************
 *		div (MSVCRT.@)
 * VERSION
 *	[i386] Windows binary compatible - returns the struct in eax/edx.
 */
#ifdef __i386__
unsigned __int64 CDECL div(int num, int denom)
{
    union {
        div_t div;
        unsigned __int64 uint64;
    } ret;

    ret.div.quot = num / denom;
    ret.div.rem = num % denom;
    return ret.uint64;
}
#else
/*********************************************************************
 *		div (MSVCRT.@)
 * VERSION
 *	[!i386] Non-x86 can't run win32 apps so we don't need binary compatibility
 */
div_t CDECL div(int num, int denom)
{
    div_t ret;

    ret.quot = num / denom;
    ret.rem = num % denom;
    return ret;
}
#endif /* ifdef __i386__ */


/*********************************************************************
 *		ldiv (MSVCRT.@)
 * VERSION
 * 	[i386] Windows binary compatible - returns the struct in eax/edx.
 */
#ifdef __i386__
unsigned __int64 CDECL ldiv(__msvcrt_long num, __msvcrt_long denom)
{
    union {
        ldiv_t ldiv;
        unsigned __int64 uint64;
    } ret;

    ret.ldiv.quot = num / denom;
    ret.ldiv.rem = num % denom;
    return ret.uint64;
}
#else
/*********************************************************************
 *		ldiv (MSVCRT.@)
 * VERSION
 *	[!i386] Non-x86 can't run win32 apps so we don't need binary compatibility
 */
ldiv_t CDECL ldiv(__msvcrt_long num, __msvcrt_long denom)
{
    ldiv_t ret;

    ret.quot = num / denom;
    ret.rem = num % denom;
    return ret;
}
#endif /* ifdef __i386__ */

#if _MSVCR_VER>=100
/*********************************************************************
 *		lldiv (MSVCR100.@)
 */
lldiv_t CDECL lldiv(__int64 num, __int64 denom)
{
  lldiv_t ret;

  ret.quot = num / denom;
  ret.rem = num % denom;

  return ret;
}
#endif

#ifdef __i386__

/*********************************************************************
 *		_adjust_fdiv (MSVCRT.@)
 * Used by the MSVC compiler to work around the Pentium FDIV bug.
 */
int MSVCRT__adjust_fdiv = 0;

/***********************************************************************
 *		_adj_fdiv_m16i (MSVCRT.@)
 *
 * NOTE
 *    I _think_ this function is intended to work around the Pentium
 *    fdiv bug.
 */
void __stdcall _adj_fdiv_m16i( short arg )
{
  TRACE("(): stub\n");
}

/***********************************************************************
 *		_adj_fdiv_m32 (MSVCRT.@)
 *
 * NOTE
 *    I _think_ this function is intended to work around the Pentium
 *    fdiv bug.
 */
void __stdcall _adj_fdiv_m32( unsigned int arg )
{
  TRACE("(): stub\n");
}

/***********************************************************************
 *		_adj_fdiv_m32i (MSVCRT.@)
 *
 * NOTE
 *    I _think_ this function is intended to work around the Pentium
 *    fdiv bug.
 */
void __stdcall _adj_fdiv_m32i( int arg )
{
  TRACE("(): stub\n");
}

/***********************************************************************
 *		_adj_fdiv_m64 (MSVCRT.@)
 *
 * NOTE
 *    I _think_ this function is intended to work around the Pentium
 *    fdiv bug.
 */
void __stdcall _adj_fdiv_m64( unsigned __int64 arg )
{
  TRACE("(): stub\n");
}

/***********************************************************************
 *		_adj_fdiv_r (MSVCRT.@)
 * FIXME
 *    This function is likely to have the wrong number of arguments.
 *
 * NOTE
 *    I _think_ this function is intended to work around the Pentium
 *    fdiv bug.
 */
void _adj_fdiv_r(void)
{
  TRACE("(): stub\n");
}

/***********************************************************************
 *		_adj_fdivr_m16i (MSVCRT.@)
 *
 * NOTE
 *    I _think_ this function is intended to work around the Pentium
 *    fdiv bug.
 */
void __stdcall _adj_fdivr_m16i( short arg )
{
  TRACE("(): stub\n");
}

/***********************************************************************
 *		_adj_fdivr_m32 (MSVCRT.@)
 *
 * NOTE
 *    I _think_ this function is intended to work around the Pentium
 *    fdiv bug.
 */
void __stdcall _adj_fdivr_m32( unsigned int arg )
{
  TRACE("(): stub\n");
}

/***********************************************************************
 *		_adj_fdivr_m32i (MSVCRT.@)
 *
 * NOTE
 *    I _think_ this function is intended to work around the Pentium
 *    fdiv bug.
 */
void __stdcall _adj_fdivr_m32i( int arg )
{
  TRACE("(): stub\n");
}

/***********************************************************************
 *		_adj_fdivr_m64 (MSVCRT.@)
 *
 * NOTE
 *    I _think_ this function is intended to work around the Pentium
 *    fdiv bug.
 */
void __stdcall _adj_fdivr_m64( unsigned __int64 arg )
{
  TRACE("(): stub\n");
}

/***********************************************************************
 *		_adj_fpatan (MSVCRT.@)
 * FIXME
 *    This function is likely to have the wrong number of arguments.
 *
 * NOTE
 *    I _think_ this function is intended to work around the Pentium
 *    fdiv bug.
 */
void _adj_fpatan(void)
{
  TRACE("(): stub\n");
}

/***********************************************************************
 *		_adj_fprem (MSVCRT.@)
 * FIXME
 *    This function is likely to have the wrong number of arguments.
 *
 * NOTE
 *    I _think_ this function is intended to work around the Pentium
 *    fdiv bug.
 */
void _adj_fprem(void)
{
  TRACE("(): stub\n");
}

/***********************************************************************
 *		_adj_fprem1 (MSVCRT.@)
 * FIXME
 *    This function is likely to have the wrong number of arguments.
 *
 * NOTE
 *    I _think_ this function is intended to work around the Pentium
 *    fdiv bug.
 */
void _adj_fprem1(void)
{
  TRACE("(): stub\n");
}

/***********************************************************************
 *		_adj_fptan (MSVCRT.@)
 * FIXME
 *    This function is likely to have the wrong number of arguments.
 *
 * NOTE
 *    I _think_ this function is intended to work around the Pentium
 *    fdiv bug.
 */
void _adj_fptan(void)
{
  TRACE("(): stub\n");
}

/***********************************************************************
 *		_safe_fdiv (MSVCRT.@)
 * FIXME
 *    This function is likely to have the wrong number of arguments.
 *
 * NOTE
 *    I _think_ this function is intended to work around the Pentium
 *    fdiv bug.
 */
void _safe_fdiv(void)
{
  TRACE("(): stub\n");
}

/***********************************************************************
 *		_safe_fdivr (MSVCRT.@)
 * FIXME
 *    This function is likely to have the wrong number of arguments.
 *
 * NOTE
 *    I _think_ this function is intended to work around the Pentium
 *    fdiv bug.
 */
void _safe_fdivr(void)
{
  TRACE("(): stub\n");
}

/***********************************************************************
 *		_safe_fprem (MSVCRT.@)
 * FIXME
 *    This function is likely to have the wrong number of arguments.
 *
 * NOTE
 *    I _think_ this function is intended to work around the Pentium
 *    fdiv bug.
 */
void _safe_fprem(void)
{
  TRACE("(): stub\n");
}

/***********************************************************************
 *		_safe_fprem1 (MSVCRT.@)
 *
 * FIXME
 *    This function is likely to have the wrong number of arguments.
 *
 * NOTE
 *    I _think_ this function is intended to work around the Pentium
 *    fdiv bug.
 */
void _safe_fprem1(void)
{
  TRACE("(): stub\n");
}

/***********************************************************************
 *		__libm_sse2_acos   (MSVCRT.@)
 */
void __cdecl __libm_sse2_acos(void)
{
    double d;
    __asm__ __volatile__( "movq %%xmm0,%0" : "=m" (d) );
    d = acos( d );
    __asm__ __volatile__( "movq %0,%%xmm0" : : "m" (d) );
}

/***********************************************************************
 *		__libm_sse2_acosf   (MSVCRT.@)
 */
void __cdecl __libm_sse2_acosf(void)
{
    float f;
    __asm__ __volatile__( "movd %%xmm0,%0" : "=g" (f) );
    f = acosf( f );
    __asm__ __volatile__( "movd %0,%%xmm0" : : "g" (f) );
}

/***********************************************************************
 *		__libm_sse2_asin   (MSVCRT.@)
 */
void __cdecl __libm_sse2_asin(void)
{
    double d;
    __asm__ __volatile__( "movq %%xmm0,%0" : "=m" (d) );
    d = asin( d );
    __asm__ __volatile__( "movq %0,%%xmm0" : : "m" (d) );
}

/***********************************************************************
 *		__libm_sse2_asinf   (MSVCRT.@)
 */
void __cdecl __libm_sse2_asinf(void)
{
    float f;
    __asm__ __volatile__( "movd %%xmm0,%0" : "=g" (f) );
    f = asinf( f );
    __asm__ __volatile__( "movd %0,%%xmm0" : : "g" (f) );
}

/***********************************************************************
 *		__libm_sse2_atan   (MSVCRT.@)
 */
void __cdecl __libm_sse2_atan(void)
{
    double d;
    __asm__ __volatile__( "movq %%xmm0,%0" : "=m" (d) );
    d = atan( d );
    __asm__ __volatile__( "movq %0,%%xmm0" : : "m" (d) );
}

/***********************************************************************
 *		__libm_sse2_atan2   (MSVCRT.@)
 */
void __cdecl __libm_sse2_atan2(void)
{
    double d1, d2;
    __asm__ __volatile__( "movq %%xmm0,%0; movq %%xmm1,%1 " : "=m" (d1), "=m" (d2) );
    d1 = atan2( d1, d2 );
    __asm__ __volatile__( "movq %0,%%xmm0" : : "m" (d1) );
}

/***********************************************************************
 *		__libm_sse2_atanf   (MSVCRT.@)
 */
void __cdecl __libm_sse2_atanf(void)
{
    float f;
    __asm__ __volatile__( "movd %%xmm0,%0" : "=g" (f) );
    f = atanf( f );
    __asm__ __volatile__( "movd %0,%%xmm0" : : "g" (f) );
}

/***********************************************************************
 *		__libm_sse2_cos   (MSVCRT.@)
 */
void __cdecl __libm_sse2_cos(void)
{
    double d;
    __asm__ __volatile__( "movq %%xmm0,%0" : "=m" (d) );
    d = cos( d );
    __asm__ __volatile__( "movq %0,%%xmm0" : : "m" (d) );
}

/***********************************************************************
 *		__libm_sse2_cosf   (MSVCRT.@)
 */
void __cdecl __libm_sse2_cosf(void)
{
    float f;
    __asm__ __volatile__( "movd %%xmm0,%0" : "=g" (f) );
    f = cosf( f );
    __asm__ __volatile__( "movd %0,%%xmm0" : : "g" (f) );
}

/***********************************************************************
 *		__libm_sse2_exp   (MSVCRT.@)
 */
void __cdecl __libm_sse2_exp(void)
{
    double d;
    __asm__ __volatile__( "movq %%xmm0,%0" : "=m" (d) );
    d = exp( d );
    __asm__ __volatile__( "movq %0,%%xmm0" : : "m" (d) );
}

/***********************************************************************
 *		__libm_sse2_expf   (MSVCRT.@)
 */
void __cdecl __libm_sse2_expf(void)
{
    float f;
    __asm__ __volatile__( "movd %%xmm0,%0" : "=g" (f) );
    f = expf( f );
    __asm__ __volatile__( "movd %0,%%xmm0" : : "g" (f) );
}

/***********************************************************************
 *		__libm_sse2_log   (MSVCRT.@)
 */
void __cdecl __libm_sse2_log(void)
{
    double d;
    __asm__ __volatile__( "movq %%xmm0,%0" : "=m" (d) );
    d = log( d );
    __asm__ __volatile__( "movq %0,%%xmm0" : : "m" (d) );
}

/***********************************************************************
 *		__libm_sse2_log10   (MSVCRT.@)
 */
void __cdecl __libm_sse2_log10(void)
{
    double d;
    __asm__ __volatile__( "movq %%xmm0,%0" : "=m" (d) );
    d = log10( d );
    __asm__ __volatile__( "movq %0,%%xmm0" : : "m" (d) );
}

/***********************************************************************
 *		__libm_sse2_log10f   (MSVCRT.@)
 */
void __cdecl __libm_sse2_log10f(void)
{
    float f;
    __asm__ __volatile__( "movd %%xmm0,%0" : "=g" (f) );
    f = log10f( f );
    __asm__ __volatile__( "movd %0,%%xmm0" : : "g" (f) );
}

/***********************************************************************
 *		__libm_sse2_logf   (MSVCRT.@)
 */
void __cdecl __libm_sse2_logf(void)
{
    float f;
    __asm__ __volatile__( "movd %%xmm0,%0" : "=g" (f) );
    f = logf( f );
    __asm__ __volatile__( "movd %0,%%xmm0" : : "g" (f) );
}

/***********************************************************************
 *		__libm_sse2_pow   (MSVCRT.@)
 */
void __cdecl __libm_sse2_pow(void)
{
    double d1, d2;
    __asm__ __volatile__( "movq %%xmm0,%0; movq %%xmm1,%1 " : "=m" (d1), "=m" (d2) );
    d1 = pow( d1, d2 );
    __asm__ __volatile__( "movq %0,%%xmm0" : : "m" (d1) );
}

/***********************************************************************
 *		__libm_sse2_powf   (MSVCRT.@)
 */
void __cdecl __libm_sse2_powf(void)
{
    float f1, f2;
    __asm__ __volatile__( "movd %%xmm0,%0; movd %%xmm1,%1" : "=g" (f1), "=g" (f2) );
    f1 = powf( f1, f2 );
    __asm__ __volatile__( "movd %0,%%xmm0" : : "g" (f1) );
}

/***********************************************************************
 *		__libm_sse2_sin   (MSVCRT.@)
 */
void __cdecl __libm_sse2_sin(void)
{
    double d;
    __asm__ __volatile__( "movq %%xmm0,%0" : "=m" (d) );
    d = sin( d );
    __asm__ __volatile__( "movq %0,%%xmm0" : : "m" (d) );
}

/***********************************************************************
 *		__libm_sse2_sinf   (MSVCRT.@)
 */
void __cdecl __libm_sse2_sinf(void)
{
    float f;
    __asm__ __volatile__( "movd %%xmm0,%0" : "=g" (f) );
    f = sinf( f );
    __asm__ __volatile__( "movd %0,%%xmm0" : : "g" (f) );
}

/***********************************************************************
 *		__libm_sse2_tan   (MSVCRT.@)
 */
void __cdecl __libm_sse2_tan(void)
{
    double d;
    __asm__ __volatile__( "movq %%xmm0,%0" : "=m" (d) );
    d = tan( d );
    __asm__ __volatile__( "movq %0,%%xmm0" : : "m" (d) );
}

/***********************************************************************
 *		__libm_sse2_tanf   (MSVCRT.@)
 */
void __cdecl __libm_sse2_tanf(void)
{
    float f;
    __asm__ __volatile__( "movd %%xmm0,%0" : "=g" (f) );
    f = tanf( f );
    __asm__ __volatile__( "movd %0,%%xmm0" : : "g" (f) );
}

/***********************************************************************
 *		__libm_sse2_sqrt_precise   (MSVCR110.@)
 */
void __cdecl __libm_sse2_sqrt_precise(void)
{
    unsigned int cw;
    double d;

    __asm__ __volatile__( "movq %%xmm0,%0" : "=m" (d) );
    __control87_2(0, 0, NULL, &cw);
    if (cw & _MCW_RC)
    {
        d = sqrt(d);
        __asm__ __volatile__( "movq %0,%%xmm0" : : "m" (d) );
        return;
    }

    if (!sqrt_validate(&d, FALSE))
    {
        __asm__ __volatile__( "movq %0,%%xmm0" : : "m" (d) );
        return;
    }
    __asm__ __volatile__( "call " __ASM_NAME( "sse2_sqrt" ) );
}
#endif  /* __i386__ */

/*********************************************************************
 *      cbrt (MSVCR120.@)
 */
double CDECL cbrt(double x)
{
    return unix_funcs->cbrt( x );
}

/*********************************************************************
 *      cbrtf (MSVCR120.@)
 */
float CDECL cbrtf(float x)
{
    return unix_funcs->cbrtf( x );
}

/*********************************************************************
 *      exp2 (MSVCR120.@)
 */
double CDECL exp2(double x)
{
    double ret = unix_funcs->exp2( x );
    if (isfinite(x) && !isfinite(ret)) *_errno() = ERANGE;
    return ret;
}

/*********************************************************************
 *      exp2f (MSVCR120.@)
 */
float CDECL exp2f(float x)
{
    float ret = unix_funcs->exp2f( x );
    if (isfinite(x) && !isfinite(ret)) *_errno() = ERANGE;
    return ret;
}

/*********************************************************************
 *      expm1 (MSVCR120.@)
 */
double CDECL expm1(double x)
{
    double ret = unix_funcs->expm1( x );
    if (isfinite(x) && !isfinite(ret)) *_errno() = ERANGE;
    return ret;
}

/*********************************************************************
 *      expm1f (MSVCR120.@)
 */
float CDECL expm1f(float x)
{
    float ret = unix_funcs->expm1f( x );
    if (isfinite(x) && !isfinite(ret)) *_errno() = ERANGE;
    return ret;
}

/*********************************************************************
 *      log1p (MSVCR120.@)
 */
double CDECL log1p(double x)
{
    if (x < -1) *_errno() = EDOM;
    else if (x == -1) *_errno() = ERANGE;
    return unix_funcs->log1p( x );
}

/*********************************************************************
 *      log1pf (MSVCR120.@)
 */
float CDECL log1pf(float x)
{
    if (x < -1) *_errno() = EDOM;
    else if (x == -1) *_errno() = ERANGE;
    return unix_funcs->log1pf( x );
}

/*********************************************************************
 *      log2 (MSVCR120.@)
 */
double CDECL log2(double x)
{
    if (x < 0) *_errno() = EDOM;
    else if (x == 0) *_errno() = ERANGE;
    return unix_funcs->log2( x );
}

/*********************************************************************
 *      log2f (MSVCR120.@)
 */
float CDECL log2f(float x)
{
    if (x < 0) *_errno() = EDOM;
    else if (x == 0) *_errno() = ERANGE;
    return unix_funcs->log2f( x );
}

/*********************************************************************
 *      rint (MSVCR120.@)
 */
double CDECL rint(double x)
{
    return unix_funcs->rint(x);
}

/*********************************************************************
 *      rintf (MSVCR120.@)
 */
float CDECL rintf(float x)
{
    return unix_funcs->rintf(x);
}

/*********************************************************************
 *      lrint (MSVCR120.@)
 */
__msvcrt_long CDECL lrint(double x)
{
    return unix_funcs->lrint( x );
}

/*********************************************************************
 *      lrintf (MSVCR120.@)
 */
__msvcrt_long CDECL lrintf(float x)
{
    return unix_funcs->lrintf( x );
}

/*********************************************************************
 *      llrint (MSVCR120.@)
 */
__int64 CDECL llrint(double x)
{
    return unix_funcs->llrint( x );
}

/*********************************************************************
 *      llrintf (MSVCR120.@)
 */
__int64 CDECL llrintf(float x)
{
    return unix_funcs->llrintf( x );
}

/*********************************************************************
 *      _fdclass (MSVCR120.@)
 *
 * Copied from musl: src/math/__fpclassifyf.c
 */
short CDECL _fdclass(float x)
{
    union { float f; UINT32 i; } u = { x };
    int e = u.i >> 23 & 0xff;

    if (!e) return u.i << 1 ? FP_SUBNORMAL : FP_ZERO;
    if (e == 0xff) return u.i << 9 ? FP_NAN : FP_INFINITE;
    return FP_NORMAL;
}

/*********************************************************************
 *      _dclass (MSVCR120.@)
 *
 * Copied from musl: src/math/__fpclassify.c
 */
short CDECL _dclass(double x)
{
    union { double f; UINT64 i; } u = { x };
    int e = u.i >> 52 & 0x7ff;

    if (!e) return u.i << 1 ? FP_SUBNORMAL : FP_ZERO;
    if (e == 0x7ff) return (u.i << 12) ? FP_NAN : FP_INFINITE;
    return FP_NORMAL;
}

#if _MSVCR_VER>=120

/*********************************************************************
 *      round (MSVCR120.@)
 *
 * Based on musl implementation: src/math/round.c
 */
double CDECL round(double x)
{
    ULONGLONG llx = *(ULONGLONG*)&x, tmp;
    int e = (llx >> 52 & 0x7ff) - 0x3ff;

    if (e >= 52)
        return x;
    if (e < -1)
        return 0 * x;
    else if (e == -1)
        return signbit(x) ? -1 : 1;

    tmp = 0x000fffffffffffffULL >> e;
    if (!(llx & tmp))
        return x;
    llx += 0x0008000000000000ULL >> e;
    llx &= ~tmp;
    return *(double*)&llx;
}

/*********************************************************************
 *      roundf (MSVCR120.@)
 *
 * Copied from musl: src/math/roundf.c
 */
float CDECL roundf(float x)
{
    static const float toint = 1 / FLT_EPSILON;

    unsigned int ix = *(unsigned int*)&x;
    int e = ix >> 23 & 0xff;
    float y;

    if (e >= 0x7f + 23)
        return x;
    if (ix >> 31)
        x = -x;
    if (e < 0x7f - 1)
        return 0 * *(float*)&ix;
    y = fp_barrierf(x + toint) - toint - x;
    if (y > 0.5f)
        y = y + x - 1;
    else if (y <= -0.5f)
        y = y + x + 1;
    else
        y = y + x;
    if (ix >> 31)
        y = -y;
    return y;
}

/*********************************************************************
 *      lround (MSVCR120.@)
 *
 * Copied from musl: src/math/lround.c
 */
__msvcrt_long CDECL lround(double x)
{
    double d = round(x);
    if (d != (double)(__msvcrt_long)d) {
        *_errno() = EDOM;
        return 0;
    }
    return d;
}

/*********************************************************************
 *      lroundf (MSVCR120.@)
 *
 * Copied from musl: src/math/lroundf.c
 */
__msvcrt_long CDECL lroundf(float x)
{
    float f = roundf(x);
    if (f != (float)(__msvcrt_long)f) {
        *_errno() = EDOM;
        return 0;
    }
    return f;
}

/*********************************************************************
 *      llround (MSVCR120.@)
 *
 * Copied from musl: src/math/llround.c
 */
__int64 CDECL llround(double x)
{
    double d = round(x);
    if (d != (double)(__int64)d) {
        *_errno() = EDOM;
        return 0;
    }
    return d;
}

/*********************************************************************
 *      llroundf (MSVCR120.@)
 *
 * Copied from musl: src/math/llroundf.c
 */
__int64 CDECL llroundf(float x)
{
    float f = roundf(x);
    if (f != (float)(__int64)f) {
        *_errno() = EDOM;
        return 0;
    }
    return f;
}

/*********************************************************************
 *      trunc (MSVCR120.@)
 */
double CDECL trunc(double x)
{
    return unix_funcs->trunc(x);
}

/*********************************************************************
 *      truncf (MSVCR120.@)
 */
float CDECL truncf(float x)
{
    return unix_funcs->truncf(x);
}

/*********************************************************************
 *      _dtest (MSVCR120.@)
 */
short CDECL _dtest(double *x)
{
    return _dclass(*x);
}

/*********************************************************************
 *      _fdtest (MSVCR120.@)
 */
short CDECL _fdtest(float *x)
{
    return _fdclass(*x);
}

/*********************************************************************
 *      erf (MSVCR120.@)
 */
double CDECL erf(double x)
{
    return unix_funcs->erf( x );
}

/*********************************************************************
 *      erff (MSVCR120.@)
 */
float CDECL erff(float x)
{
    return unix_funcs->erff( x );
}

/*********************************************************************
 *      erfc (MSVCR120.@)
 */
double CDECL erfc(double x)
{
    return unix_funcs->erfc( x );
}

/*********************************************************************
 *      erfcf (MSVCR120.@)
 */
float CDECL erfcf(float x)
{
    return unix_funcs->erfcf( x );
}

/*********************************************************************
 *      fmaxf (MSVCR120.@)
 */
float CDECL fmaxf(float x, float y)
{
    if(isnan(x))
        return y;
    if(isnan(y))
        return x;
    if(x==0 && y==0)
        return signbit(x) ? y : x;
    return x<y ? y : x;
}

/*********************************************************************
 *      fmax (MSVCR120.@)
 */
double CDECL fmax(double x, double y)
{
    if(isnan(x))
        return y;
    if(isnan(y))
        return x;
    if(x==0 && y==0)
        return signbit(x) ? y : x;
    return x<y ? y : x;
}

/*********************************************************************
 *      fdimf (MSVCR120.@)
 */
float CDECL fdimf(float x, float y)
{
    if(isnan(x))
        return x;
    if(isnan(y))
        return y;
    return x>y ? x-y : 0;
}

/*********************************************************************
 *      fdim (MSVCR120.@)
 */
double CDECL fdim(double x, double y)
{
    if(isnan(x))
        return x;
    if(isnan(y))
        return y;
    return x>y ? x-y : 0;
}

/*********************************************************************
 *      _fdsign (MSVCR120.@)
 */
int CDECL _fdsign(float x)
{
    union { float f; UINT32 i; } u = { x };
    return (u.i >> 16) & 0x8000;
}

/*********************************************************************
 *      _dsign (MSVCR120.@)
 */
int CDECL _dsign(double x)
{
    union { double f; UINT64 i; } u = { x };
    return (u.i >> 48) & 0x8000;
}


/*********************************************************************
 *      _dpcomp (MSVCR120.@)
 */
int CDECL _dpcomp(double x, double y)
{
    if(isnan(x) || isnan(y))
        return 0;

    if(x == y) return 2;
    return x < y ? 1 : 4;
}

/*********************************************************************
 *      _fdpcomp (MSVCR120.@)
 */
int CDECL _fdpcomp(float x, float y)
{
    return _dpcomp(x, y);
}

/*********************************************************************
 *      fminf (MSVCR120.@)
 */
float CDECL fminf(float x, float y)
{
    if(isnan(x))
        return y;
    if(isnan(y))
        return x;
    if(x==0 && y==0)
        return signbit(x) ? x : y;
    return x<y ? x : y;
}

/*********************************************************************
 *      fmin (MSVCR120.@)
 */
double CDECL fmin(double x, double y)
{
    if(isnan(x))
        return y;
    if(isnan(y))
        return x;
    if(x==0 && y==0)
        return signbit(x) ? x : y;
    return x<y ? x : y;
}

/*********************************************************************
 *      asinh (MSVCR120.@)
 */
double CDECL asinh(double x)
{
    return unix_funcs->asinh( x );
}

/*********************************************************************
 *      asinhf (MSVCR120.@)
 */
float CDECL asinhf(float x)
{
    return unix_funcs->asinhf( x );
}

/*********************************************************************
 *      acosh (MSVCR120.@)
 */
double CDECL acosh(double x)
{
    if (x < 1)
    {
        fenv_t env;

        *_errno() = EDOM;
        fegetenv(&env);
        env._Fe_stat |= FE_INVALID;
        fesetenv(&env);
        return NAN;
    }
    return unix_funcs->acosh( x );
}

/*********************************************************************
 *      acoshf (MSVCR120.@)
 */
float CDECL acoshf(float x)
{
    if (x < 1)
    {
        fenv_t env;

        *_errno() = EDOM;
        fegetenv(&env);
        env._Fe_stat |= FE_INVALID;
        fesetenv(&env);
        return NAN;
    }
    return unix_funcs->acoshf( x );
}

/*********************************************************************
 *      atanh (MSVCR120.@)
 */
double CDECL atanh(double x)
{
    double ret;

    if (x > 1 || x < -1) {
        fenv_t env;

        *_errno() = EDOM;

        /* on Linux atanh returns -NAN in this case */
        fegetenv(&env);
        env._Fe_stat |= FE_INVALID;
        fesetenv(&env);
        return NAN;
    }
    ret = unix_funcs->atanh( x );

    if (!isfinite(ret)) *_errno() = ERANGE;
    return ret;
}

/*********************************************************************
 *      atanhf (MSVCR120.@)
 */
float CDECL atanhf(float x)
{
    float ret;

    if (x > 1 || x < -1) {
        fenv_t env;

        *_errno() = EDOM;

        fegetenv(&env);
        env._Fe_stat |= FE_INVALID;
        fesetenv(&env);
        return NAN;
    }

    ret = unix_funcs->atanh( x );

    if (!isfinite(ret)) *_errno() = ERANGE;
    return ret;
}

#endif /* _MSVCR_VER>=120 */

/*********************************************************************
 *      _scalb  (MSVCRT.@)
 *      scalbn  (MSVCR120.@)
 *      scalbln (MSVCR120.@)
 */
double CDECL _scalb(double num, __msvcrt_long power)
{
  return ldexp(num, power);
}

/*********************************************************************
 *      _scalbf  (MSVCRT.@)
 *      scalbnf  (MSVCR120.@)
 *      scalblnf (MSVCR120.@)
 */
float CDECL _scalbf(float num, __msvcrt_long power)
{
  return ldexp(num, power);
}

#if _MSVCR_VER>=120

/*********************************************************************
 *      remainder (MSVCR120.@)
 */
double CDECL remainder(double x, double y)
{
    /* this matches 64-bit Windows.  32-bit Windows is slightly different */
    if(!isfinite(x)) *_errno() = EDOM;
    if(isnan(y) || y==0.0) *_errno() = EDOM;
    return unix_funcs->remainder( x, y );
}

/*********************************************************************
 *      remainderf (MSVCR120.@)
 */
float CDECL remainderf(float x, float y)
{
    /* this matches 64-bit Windows.  32-bit Windows is slightly different */
    if(!isfinite(x)) *_errno() = EDOM;
    if(isnan(y) || y==0.0f) *_errno() = EDOM;
    return unix_funcs->remainderf( x, y );
}

/*********************************************************************
 *      remquo (MSVCR120.@)
 */
double CDECL remquo(double x, double y, int *quo)
{
    if(!isfinite(x)) *_errno() = EDOM;
    if(isnan(y) || y==0.0) *_errno() = EDOM;
    return unix_funcs->remquo( x, y, quo );
}

/*********************************************************************
 *      remquof (MSVCR120.@)
 */
float CDECL remquof(float x, float y, int *quo)
{
    if(!isfinite(x)) *_errno() = EDOM;
    if(isnan(y) || y==0.0f) *_errno() = EDOM;
    return unix_funcs->remquof( x, y, quo );
}

/*********************************************************************
 *      lgamma (MSVCR120.@)
 */
double CDECL lgamma(double x)
{
    return unix_funcs->lgamma( x );
}

/*********************************************************************
 *      lgammaf (MSVCR120.@)
 */
float CDECL lgammaf(float x)
{
    return unix_funcs->lgammaf( x );
}

/*********************************************************************
 *      tgamma (MSVCR120.@)
 */
double CDECL tgamma(double x)
{
    return unix_funcs->tgamma( x );
}

/*********************************************************************
 *      tgammaf (MSVCR120.@)
 */
float CDECL tgammaf(float x)
{
    return unix_funcs->tgammaf( x );
}

/*********************************************************************
 *      nan (MSVCR120.@)
 */
double CDECL nan(const char *tagp)
{
    /* Windows ignores input (MSDN) */
    return NAN;
}

/*********************************************************************
 *      nanf (MSVCR120.@)
 */
float CDECL nanf(const char *tagp)
{
    return NAN;
}

/*********************************************************************
 *      _except1 (MSVCR120.@)
 *  TODO:
 *   - find meaning of ignored cw and operation bits
 *   - unk parameter
 */
double CDECL _except1(DWORD fpe, _FP_OPERATION_CODE op, double arg, double res, DWORD cw, void *unk)
{
    ULONG_PTR exception_arg;
    DWORD exception = 0;
    fenv_t env;
    DWORD fpword = 0;
    WORD operation;

    TRACE("(%x %x %lf %lf %x %p)\n", fpe, op, arg, res, cw, unk);

#ifdef _WIN64
    cw = ((cw >> 7) & 0x3f) | ((cw >> 3) & 0xc00);
#endif
    operation = op << 5;
    exception_arg = (ULONG_PTR)&operation;

    fegetenv(&env);

    if (fpe & 0x1) { /* overflow */
        if ((fpe == 0x1 && (cw & 0x8)) || (fpe==0x11 && (cw & 0x28))) {
            /* 32-bit version also sets SW_INEXACT here */
            env._Fe_stat |= FE_OVERFLOW;
            if (fpe & 0x10) env._Fe_stat |= FE_INEXACT;
            res = signbit(res) ? -INFINITY : INFINITY;
        } else {
            exception = EXCEPTION_FLT_OVERFLOW;
        }
    } else if (fpe & 0x2) { /* underflow */
        if ((fpe == 0x2 && (cw & 0x10)) || (fpe==0x12 && (cw & 0x30))) {
            env._Fe_stat |= FE_UNDERFLOW;
            if (fpe & 0x10) env._Fe_stat |= FE_INEXACT;
            res = signbit(res) ? -0.0 : 0.0;
        } else {
            exception = EXCEPTION_FLT_UNDERFLOW;
        }
    } else if (fpe & 0x4) { /* zerodivide */
        if ((fpe == 0x4 && (cw & 0x4)) || (fpe==0x14 && (cw & 0x24))) {
            env._Fe_stat |= FE_DIVBYZERO;
            if (fpe & 0x10) env._Fe_stat |= FE_INEXACT;
        } else {
            exception = EXCEPTION_FLT_DIVIDE_BY_ZERO;
        }
    } else if (fpe & 0x8) { /* invalid */
        if (fpe == 0x8 && (cw & 0x1)) {
            env._Fe_stat |= FE_INVALID;
        } else {
            exception = EXCEPTION_FLT_INVALID_OPERATION;
        }
    } else if (fpe & 0x10) { /* inexact */
        if (fpe == 0x10 && (cw & 0x20)) {
            env._Fe_stat |= FE_INEXACT;
        } else {
            exception = EXCEPTION_FLT_INEXACT_RESULT;
        }
    }

    if (exception)
        env._Fe_stat = 0;
    fesetenv(&env);
    if (exception)
        RaiseException(exception, 0, 1, &exception_arg);

    if (cw & 0x1) fpword |= _EM_INVALID;
    if (cw & 0x2) fpword |= _EM_DENORMAL;
    if (cw & 0x4) fpword |= _EM_ZERODIVIDE;
    if (cw & 0x8) fpword |= _EM_OVERFLOW;
    if (cw & 0x10) fpword |= _EM_UNDERFLOW;
    if (cw & 0x20) fpword |= _EM_INEXACT;
    switch (cw & 0xc00)
    {
        case 0xc00: fpword |= _RC_UP|_RC_DOWN; break;
        case 0x800: fpword |= _RC_UP; break;
        case 0x400: fpword |= _RC_DOWN; break;
    }
    switch (cw & 0x300)
    {
        case 0x0:   fpword |= _PC_24; break;
        case 0x200: fpword |= _PC_53; break;
        case 0x300: fpword |= _PC_64; break;
    }
    if (cw & 0x1000) fpword |= _IC_AFFINE;
    _control87(fpword, 0xffffffff);

    return res;
}

_Dcomplex* CDECL _Cbuild(_Dcomplex *ret, double r, double i)
{
    ret->_Val[0] = r;
    ret->_Val[1] = i;
    return ret;
}

double CDECL MSVCR120_creal(_Dcomplex z)
{
    return z._Val[0];
}

/*********************************************************************
 *      ilogb (MSVCR120.@)
 *
 * Copied from musl: src/math/ilogb.c
 */
int CDECL ilogb(double x)
{
    union { double f; UINT64 i; } u = { x };
    int e = u.i >> 52 & 0x7ff;

    if (!e)
    {
        u.i <<= 12;
        if (u.i == 0) return FP_ILOGB0;
        /* subnormal x */
        for (e = -0x3ff; u.i >> 63 == 0; e--, u.i <<= 1);
        return e;
    }
    if (e == 0x7ff) return u.i << 12 ? FP_ILOGBNAN : INT_MAX;
    return e - 0x3ff;
}

/*********************************************************************
 *      ilogbf (MSVCR120.@)
 *
 * Copied from musl: src/math/ilogbf.c
 */
int CDECL ilogbf(float x)
{
    union { float f; UINT32 i; } u = { x };
    int e = u.i >> 23 & 0xff;

    if (!e)
    {
        u.i <<= 9;
        if (u.i == 0) return FP_ILOGB0;
        /* subnormal x */
        for (e = -0x7f; u.i >> 31 == 0; e--, u.i <<= 1);
        return e;
    }
    if (e == 0xff) return u.i << 9 ? FP_ILOGBNAN : INT_MAX;
    return e - 0x7f;
}
#endif /* _MSVCR_VER>=120 */
