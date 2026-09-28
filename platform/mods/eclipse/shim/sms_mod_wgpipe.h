// Stands in for SunshineHeaderInterface's `extern WGPipe volatile wgPipe;`
// (fixup_sources.py): stores to the GameCube's write-gather pipe
// (wgPipe.U8 = ..., as its inline vertex and register writers do) go to the
// port's graphics layer, as the game's own do (sms_gx/gx_pc.h, GXPC_Write*).
#pragma once

#ifdef __cplusplus
extern "C++" {
extern "C" {
void GXPC_Write8(unsigned char v);
void GXPC_Write16(unsigned short v);
void GXPC_Write32(unsigned int v);
void GXPC_WriteF32(float v);
}

struct SmsModWGPipe {
	struct U8_ { void operator=(unsigned char v) const { GXPC_Write8(v); } } U8;
	struct S8_ { void operator=(signed char v) const { GXPC_Write8((unsigned char)v); } } S8;
	struct U16_ { void operator=(unsigned short v) const { GXPC_Write16(v); } } U16;
	struct S16_ { void operator=(short v) const { GXPC_Write16((unsigned short)v); } } S16;
	struct U32_ { void operator=(unsigned int v) const { GXPC_Write32(v); } } U32;
	struct S32_ { void operator=(int v) const { GXPC_Write32((unsigned int)v); } } S32;
	struct F32_ { void operator=(float v) const { GXPC_WriteF32(v); } } F32;
};
static const SmsModWGPipe wgPipe = {};
}
#else
extern WGPipe volatile wgPipe;
#endif
