/* Preserve the C++11 nullptr keyword in the platform layer. The original
 * game, compiled as C++03, still uses the decomp's integer-zero macro. */
#ifndef SMS_PORT_TYPES_H
#define SMS_PORT_TYPES_H
#include_next <types.h>
#if defined(__cplusplus) && __cplusplus >= 201103L
#undef nullptr
#endif
#endif
