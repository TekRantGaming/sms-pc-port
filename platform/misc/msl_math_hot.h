/* Builds MSL_HOT_PART (msl_math_trig.inc or msl_math_pow.inc) for
 * msl_math.c: as is, or with MSL_FMA_DISPATCH twice (with port_fmas_soft and
 * with the FMA instruction) behind the public names. */
#if MSL_FMA_DISPATCH
#define MSL_HOT(name) name##_soft
#define MSL_HOT_ATTR
#define HOT_FMADDS fmadds
#define HOT_FNMADDS fnmadds
#define HOT_FNMSUBS fnmsubs
#include MSL_HOT_PART
#undef MSL_HOT
#undef MSL_HOT_ATTR
#undef HOT_FMADDS
#undef HOT_FNMADDS
#undef HOT_FNMSUBS
#define MSL_HOT(name) name##_hw
#define MSL_HOT_ATTR MSL_FMA_TARGET
#define HOT_FMADDS hw_fmadds
#define HOT_FNMADDS hw_fnmadds
#define HOT_FNMSUBS hw_fnmsubs
#include MSL_HOT_PART
#undef MSL_HOT
#undef MSL_HOT_ATTR
#undef HOT_FMADDS
#undef HOT_FNMADDS
#undef HOT_FNMSUBS
#else
#define MSL_HOT(name) name
#define MSL_HOT_ATTR
#define HOT_FMADDS fmadds
#define HOT_FNMADDS fnmadds
#define HOT_FNMSUBS fnmsubs
#include MSL_HOT_PART
#undef MSL_HOT
#undef MSL_HOT_ATTR
#undef HOT_FMADDS
#undef HOT_FNMADDS
#undef HOT_FNMSUBS
#endif
