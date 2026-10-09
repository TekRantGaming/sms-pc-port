// The write-gather pipe writers of GXVert.h as real functions, declared by
// include/sms_gx/gxvert_pc.h (the TARGET_PC body of GXVert.h).
#include <sms_gx/gxvert_pc.h>

extern "C" {

void GXCmd1u8(u8 x) { GXPC_Put8(x); }
void GXCmd1u16(u16 x) { GXPC_Put16(x); }
void GXCmd1u32(u32 x) { GXPC_Put32(uint32_t(x)); }
void GXParam1u8(u8 x) { GXPC_Put8(x); }
void GXParam1u16(u16 x) { GXPC_Put16(x); }
void GXParam1u32(u32 x) { GXPC_Put32(uint32_t(x)); }
void GXParam1s8(s8 x) { GXPC_Put8(uint8_t(x)); }
void GXParam1s16(s16 x) { GXPC_Put16(uint16_t(x)); }
void GXParam1s32(s32 x) { GXPC_Put32(uint32_t(x)); }
void GXParam1f32(f32 x) { GXPC_PutF32(x); }
void GXParam3f32(f32 x, f32 y, f32 z) { GXPC_PutF32(x); GXPC_PutF32(y); GXPC_PutF32(z); }
void GXParam4f32(f32 x, f32 y, f32 z, f32 w) { GXPC_PutF32(x); GXPC_PutF32(y); GXPC_PutF32(z); GXPC_PutF32(w); }
void GXPosition3f32(f32 x, f32 y, f32 z) { GXPC_PutF32(x); GXPC_PutF32(y); GXPC_PutF32(z); }
void GXPosition3u8(u8 x, u8 y, u8 z) { GXPC_Put8(x); GXPC_Put8(y); GXPC_Put8(z); }
void GXPosition3s8(s8 x, s8 y, s8 z) { GXPC_Put8(uint8_t(x)); GXPC_Put8(uint8_t(y)); GXPC_Put8(uint8_t(z)); }
void GXPosition3u16(u16 x, u16 y, u16 z) { GXPC_Put16(x); GXPC_Put16(y); GXPC_Put16(z); }
void GXPosition3s16(s16 x, s16 y, s16 z) { GXPC_Put16(uint16_t(x)); GXPC_Put16(uint16_t(y)); GXPC_Put16(uint16_t(z)); }
void GXPosition2f32(f32 x, f32 y) { GXPC_PutF32(x); GXPC_PutF32(y); }
void GXPosition2u8(u8 x, u8 y) { GXPC_Put8(x); GXPC_Put8(y); }
void GXPosition2s8(s8 x, s8 y) { GXPC_Put8(uint8_t(x)); GXPC_Put8(uint8_t(y)); }
void GXPosition2u16(u16 x, u16 y) { GXPC_Put16(x); GXPC_Put16(y); }
void GXPosition2s16(s16 x, s16 y) { GXPC_Put16(uint16_t(x)); GXPC_Put16(uint16_t(y)); }
void GXPosition1x16(u16 x) { GXPC_Put16(x); }
void GXPosition1x8(u8 x) { GXPC_Put8(x); }
void GXNormal3f32(f32 x, f32 y, f32 z) { GXPC_PutF32(x); GXPC_PutF32(y); GXPC_PutF32(z); }
void GXNormal3s16(s16 x, s16 y, s16 z) { GXPC_Put16(uint16_t(x)); GXPC_Put16(uint16_t(y)); GXPC_Put16(uint16_t(z)); }
void GXNormal3s8(s8 x, s8 y, s8 z) { GXPC_Put8(uint8_t(x)); GXPC_Put8(uint8_t(y)); GXPC_Put8(uint8_t(z)); }
void GXNormal1x16(u16 x) { GXPC_Put16(x); }
void GXNormal1x8(u8 x) { GXPC_Put8(x); }
void GXColor4u8(u8 r, u8 gg, u8 b, u8 a) { GXPC_Put8(r); GXPC_Put8(gg); GXPC_Put8(b); GXPC_Put8(a); }
void GXColor1u32(u32 x) { GXPC_Put32(uint32_t(x)); }
void GXColor3u8(u8 r, u8 gg, u8 b) { GXPC_Put8(r); GXPC_Put8(gg); GXPC_Put8(b); }
void GXColor1u16(u16 x) { GXPC_Put16(x); }
void GXColor1x16(u16 x) { GXPC_Put16(x); }
void GXColor1x8(u8 x) { GXPC_Put8(x); }
void GXTexCoord2f32(f32 s, f32 t) { GXPC_PutF32(s); GXPC_PutF32(t); }
void GXTexCoord2s16(s16 s, s16 t) { GXPC_Put16(uint16_t(s)); GXPC_Put16(uint16_t(t)); }
void GXTexCoord2u16(u16 s, u16 t) { GXPC_Put16(s); GXPC_Put16(t); }
void GXTexCoord2s8(s8 s, s8 t) { GXPC_Put8(uint8_t(s)); GXPC_Put8(uint8_t(t)); }
void GXTexCoord2u8(u8 s, u8 t) { GXPC_Put8(s); GXPC_Put8(t); }
void GXTexCoord1f32(f32 s) { GXPC_PutF32(s); }
void GXTexCoord1s16(s16 s) { GXPC_Put16(uint16_t(s)); }
void GXTexCoord1u16(u16 s) { GXPC_Put16(s); }
void GXTexCoord1s8(s8 s) { GXPC_Put8(uint8_t(s)); }
void GXTexCoord1u8(u8 s) { GXPC_Put8(s); }
void GXTexCoord1x16(u16 x) { GXPC_Put16(x); }
void GXTexCoord1x8(u8 x) { GXPC_Put8(x); }
void GXMatrixIndex1u8(u8 x) { GXPC_Put8(x); }

}  // extern "C"
