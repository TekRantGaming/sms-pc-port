/* The Gekko's fused multiply-adds where MWCC contracted the game's source
 * (-fp_contract on): tools/fmacontract/fmarewrite.py writes each site MWCC
 * fuses as one of these calls (decomp-patches/fma/), and the game code is
 * otherwise compiled without contraction (-ffp-contract=off).
 *
 *   port_fmaf(a, b, c)   a * b + c        fmadds
 *   port_fmsf(a, b, c)   a * b - c        fmsubs
 *   port_fnmsf(a, b, c)  -(a * b - c)     fnmsubs (c - a * b, c -= a * b)
 *
 * each rounded once; port_fmad/port_fmsd/port_fnmsd in double (fmadd ...),
 * and port_fmat/port_fmst/port_fnmst in templates, which pick the float or
 * double form when the product and the sum have that type and compute the
 * expression unfused otherwise (an integer or a class instantiation), as MWCC
 * did. -(a * b + c), fnmadds, is the negation of port_fmaf: exact.
 *
 * Single precision: the FMA instruction where the compiler may use one
 * (__FMA__: x86 built with SMS_FMA_HW, and other hosts' fmaf), and
 * port_fmas_soft (port_fma.h) on x86 otherwise; both give the correctly
 * rounded result, so the choice changes no value. Double precision: fma(),
 * which C99 defines as rounded once (glibc's is exact with or without the
 * instruction).
 *
 * The negated forms negate the rounded result, as the Gekko does (fnmsubs
 * gives -0 where a * b == c, where c - a * b gives +0); PORT_OPAQUE stops the
 * compiler from folding the negation into x86's fnmsub forms.
 *
 * With PORT_FMAC_COUNT (tools/fmacount) the helpers are external functions,
 * so that each site is a call to count. */
#ifndef SMS_PORT_FMAC_H
#define SMS_PORT_FMAC_H

#include "port_fma.h"
#include <math.h>

#if defined(PORT_FMAC_COUNT)
#ifdef __cplusplus
extern "C" {
#endif
float port_fmaf(float a, float b, float c);
float port_fmsf(float a, float b, float c);
float port_fnmsf(float a, float b, float c);
double port_fmad(double a, double b, double c);
double port_fmsd(double a, double b, double c);
double port_fnmsd(double a, double b, double c);
#ifdef __cplusplus
}
#endif
#else

#if (defined(__x86_64__) || defined(__i386__)) && !defined(__FMA__)
#define PORT_FMAC_FMAF(a, b, c) port_fmas_soft(a, b, c)
#else
#define PORT_FMAC_FMAF(a, b, c) __builtin_fmaf(a, b, c)
#endif

static inline float port_fmaf(float a, float b, float c) { return PORT_FMAC_FMAF(a, b, c); }
static inline float port_fmsf(float a, float b, float c) { return PORT_FMAC_FMAF(a, b, -c); }
static inline float port_fnmsf(float a, float b, float c)
{
	float r = PORT_FMAC_FMAF(a, b, -c);
	PORT_OPAQUE(r);
	return -r;
}
static inline double port_fmad(double a, double b, double c) { return __builtin_fma(a, b, c); }
static inline double port_fmsd(double a, double b, double c) { return __builtin_fma(a, b, -c); }
static inline double port_fnmsd(double a, double b, double c)
{
	double r = __builtin_fma(a, b, -c);
	PORT_OPAQUE(r);
	return -r;
}
#endif

#ifdef __cplusplus
template <class T> T port_fmac_val();
template <class P, class S> struct port_fmac_sel {
	template <class A, class B, class C> static inline __attribute__((always_inline)) S fma(A a, B b, C c) { return a * b + c; }
	template <class A, class B, class C> static inline __attribute__((always_inline)) S fms(A a, B b, C c) { return a * b - c; }
	template <class A, class B, class C> static inline __attribute__((always_inline)) S fnms(A a, B b, C c) { return c - a * b; }
};
template <> struct port_fmac_sel<float, float> {
	template <class A, class B, class C> static inline __attribute__((always_inline)) float fma(A a, B b, C c) { return port_fmaf(a, b, c); }
	template <class A, class B, class C> static inline __attribute__((always_inline)) float fms(A a, B b, C c) { return port_fmsf(a, b, c); }
	template <class A, class B, class C> static inline __attribute__((always_inline)) float fnms(A a, B b, C c) { return port_fnmsf(a, b, c); }
};
template <> struct port_fmac_sel<double, double> {
	template <class A, class B, class C> static inline __attribute__((always_inline)) double fma(A a, B b, C c) { return port_fmad(a, b, c); }
	template <class A, class B, class C> static inline __attribute__((always_inline)) double fms(A a, B b, C c) { return port_fmsd(a, b, c); }
	template <class A, class B, class C> static inline __attribute__((always_inline)) double fnms(A a, B b, C c) { return port_fnmsd(a, b, c); }
};
template <class A, class B, class C> struct port_fmac_ty {
	typedef __typeof__(port_fmac_val<A>() * port_fmac_val<B>()) P;
	typedef __typeof__(port_fmac_val<A>() * port_fmac_val<B>() + port_fmac_val<C>()) Sa;
	typedef __typeof__(port_fmac_val<A>() * port_fmac_val<B>() - port_fmac_val<C>()) Ss;
	typedef __typeof__(port_fmac_val<C>() - port_fmac_val<A>() * port_fmac_val<B>()) Sn;
};
template <class A, class B, class C>
static inline __attribute__((always_inline)) typename port_fmac_ty<A, B, C>::Sa port_fmat(A a, B b, C c)
{
	typedef port_fmac_ty<A, B, C> T;
	return port_fmac_sel<typename T::P, typename T::Sa>::fma(a, b, c);
}
template <class A, class B, class C>
static inline __attribute__((always_inline)) typename port_fmac_ty<A, B, C>::Ss port_fmst(A a, B b, C c)
{
	typedef port_fmac_ty<A, B, C> T;
	return port_fmac_sel<typename T::P, typename T::Ss>::fms(a, b, c);
}
template <class A, class B, class C>
static inline __attribute__((always_inline)) typename port_fmac_ty<A, B, C>::Sn port_fnmst(A a, B b, C c)
{
	typedef port_fmac_ty<A, B, C> T;
	return port_fmac_sel<typename T::P, typename T::Sn>::fnms(a, b, c);
}
#endif

#endif
