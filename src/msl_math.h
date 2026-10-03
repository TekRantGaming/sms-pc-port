/* The GameCube's MSL maths, compiled for the host
 * (platform/misc/msl_math.c). port_compat.h routes the game's calls here;
 * platform code that stands in for SDK code calling MSL (platform/mtx) calls
 * these directly. */
#ifndef SMS_MSL_MATH_H
#define SMS_MSL_MATH_H

#ifdef __cplusplus
extern "C" {
#endif
float sms_msl_sinf(float x);            /* trigf.c */
float sms_msl_cosf(float x);            /* trigf.c */
float sms_msl_tanf(float x);            /* trigf.c */
float sms_msl_atanf(float x);           /* inverse_trig.c */
float sms_msl_atan2f(float y, float x); /* inverse_trig.c */
float sms_msl_acosf(float x);           /* inverse_trig.c */
float sms_msl_expf(float x);            /* exponentialsf.c */
float sms_msl_powf(float x, float y);   /* exponentialsf.c */
float sms_msl_fmodf(float x, float y);  /* MSL's inline std::fmodf */
float sms_msl_sqrtf(float x);           /* MSL's inline std::sqrtf */
double sms_msl_atan(double x);          /* s_atan.c (fdlibm) */
double sms_msl_atan2(double y, double x); /* w_atan2.c, e_atan2.c (fdlibm) */
/* The game's own frsqrte helpers, which MWCC compiled with a fused step */
float sms_jg_sqrtf(float mag);          /* JGeometry::TUtil<f32>::sqrt */
float sms_jg_inv_sqrtf(float mag);      /* JGeometry::TUtil<f32>::inv_sqrt */
float sms_ms_sqrtf(float x);            /* MsSqrtf (MarioUtil/MathUtil.hpp) */
#ifdef __cplusplus
}
#endif

#endif /* SMS_MSL_MATH_H */
