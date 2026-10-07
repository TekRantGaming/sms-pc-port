#ifdef _WIN32
/* MinGW COFF weak aliases do not satisfy ordinary undefined references for
 * these SDK entry points. Give the still-unimplemented calls real fallback
 * definitions; the Linux build keeps using the generated weak stubs. */
#include "port_compat.h"
#include "port_stub.h"
#include <dolphin/os/OSFont.h>

u16 OSGetFontEncode(void) { SDK_STUB(OSGetFontEncode); return 0; }
char* OSGetFontTexture(char*, void**, s32*, s32*, s32*) { SDK_STUB(OSGetFontTexture); return 0; }
char* OSGetFontWidth(char*, s32*) { SDK_STUB(OSGetFontWidth); return 0; }
BOOL OSInitFont(OSFontHeader*) { SDK_STUB(OSInitFont); return FALSE; }
#endif
