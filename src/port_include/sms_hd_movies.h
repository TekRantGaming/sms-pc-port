#ifndef SMS_HD_MOVIES_H
#define SMS_HD_MOVIES_H
#include <dolphin/types.h>
#ifdef __cplusplus
extern "C" {
#endif
BOOL port_draw_hd_thp(u8* y, u8* u, u8* v, s16 x, s16 pos_y,
                     s16 width, s16 height, s16 draw_width, s16 draw_height);
// A complete installed pack, when HD textures are enabled; NULL otherwise.
const char* port_hd_cutscene_directory(void);
#ifdef __cplusplus
}
#endif
#endif
