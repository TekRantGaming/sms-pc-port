/* Port override of decomp/include/dolphin/os/OSStopwatch.h (same include
 * guard). The 64-bit members are spelled s64, which keeps the GameCube's
 * 8-byte alignment on i386 (see dolphin/types.h), so TMarDirector, which
 * embeds a stopwatch, keeps its retail offsets for code mods. Game and
 * platform code both reach this copy first. */
#ifndef _DOLPHIN_OSSTOPWATCH_H_
#define _DOLPHIN_OSSTOPWATCH_H_

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct OSStopwatch {
	char* name;
	s64 total;
	unsigned long hits;
	s64 min;
	s64 max;
	s64 last;
	int running;
};

void OSInitStopwatch(struct OSStopwatch* sw, char* name);
void OSStartStopwatch(struct OSStopwatch* sw);
void OSStopStopwatch(struct OSStopwatch* sw);
long long OSCheckStopwatch(struct OSStopwatch* sw);
void OSResetStopwatch(struct OSStopwatch* sw);
void OSDumpStopwatch(struct OSStopwatch* sw);

#ifdef __cplusplus
}
#endif

#endif
