/* The GameCube's MSL trigonometry, compiled for the host
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
double sms_msl_atan(double x);          /* s_atan.c (fdlibm) */
double sms_msl_atan2(double y, double x); /* w_atan2.c, e_atan2.c (fdlibm) */
#ifdef __cplusplus
}
#endif

#endif /* SMS_MSL_MATH_H */
