// Port shim over SunshineHeaderInterface's SMS/macros.h: PowerPC inline
// assembly cannot run natively, so assembly blocks compile to nothing and
// assembly-only functions become empty stubs; the patches that point at them
// are translated one by one on the port side. Register reads and writes go
// to a register context the port's hooks fill in.
#pragma once

#include_next <SMS/macros.h>

#undef SMS_ASM_FUNC
#undef SMS_ASM_BLOCK
#undef SMS_FROM_GPR
#undef SMS_TO_GPR
#undef SMS_FROM_FPR
#undef SMS_TO_FPR
#define SMS_ASM_FUNC
#define SMS_ASM_BLOCK(...)       ((void)0)
// A mod function reading the caller's registers reads what the port's hook
// at that call site put there instead (sms_modhook.h, SMS_MOD_GPR).
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern uintptr_t sms_mod_gpr[32];
extern double sms_mod_fpr[32];
#ifdef __cplusplus
}
#endif
#define SMS_FROM_GPR(reg, var)   ((var) = (__typeof__(var))sms_mod_gpr[reg])
#define SMS_TO_GPR(reg, var)     (sms_mod_gpr[reg] = (uintptr_t)(var))
#define SMS_FROM_FPR(reg, var)   ((var) = (__typeof__(var))sms_mod_fpr[reg])
#define SMS_TO_FPR(reg, var)     (sms_mod_fpr[reg] = (double)(var))
