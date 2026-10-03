/* Drop-in replacement for decomp/libs/dolphin/include/dolphin/gx/GXVert.h in the PC build.
 * Put platform/gx/port_include ahead of decomp/include on the game's include
 * path (CMake: ${SMS_GX_OVERRIDE_INCLUDE_DIR}); the vertex writers then call
 * into sms_gx instead of storing to 0xCC008000. */
#include <sms_gx/gxvert_pc.h>
