/* Force-included before the decompiled GD sources (decomp/libs/dolphin/src/gd).
 * GD only writes command bytes into memory, so it runs unchanged on the host
 * once the two PowerPC-isms it touches are mapped: the cached->physical
 * address conversion (onto sms_gx's physical-address window) and __cntlzw. */
#ifndef SMS_GX_GD_HOST_PREFIX_H
#define SMS_GX_GD_HOST_PREFIX_H

#include <stdint.h>
#include <dolphin/os.h>

#ifdef __cplusplus
extern "C"
#endif
uint32_t GXPC_PtrToPhys(const void* ptr);

#undef OSCachedToPhysical
#define OSCachedToPhysical(caddr) GXPC_PtrToPhys((const void*)(caddr))

#ifndef __cntlzw
static inline unsigned int sms_gx_cntlzw(unsigned int x) { return x ? (unsigned int)__builtin_clz(x) : 32u; }
#define __cntlzw(x) sms_gx_cntlzw((unsigned int)(x))
#endif

#endif
