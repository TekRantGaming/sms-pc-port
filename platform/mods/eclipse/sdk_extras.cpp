// Dolphin SDK functions BetterSunshineEngine calls that the retail game (and so
// the port) never needed. Each is a logged no-op until a feature needs more:
// streamed music (DVD audio streaming, its fade alarms), cache control, the
// memory card's game-code switch and the exception handler.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {
void once(const char* what)
{
	static const char* seen[32];
	for (int i = 0; i < 32 && seen[i]; i++)
		if (seen[i] == what)
			return;
	for (int i = 0; i < 32; i++)
		if (!seen[i]) {
			seen[i] = what;
			break;
		}
	fprintf(stderr, "[mod] %s: not supported natively (ignored)\n", what);
}
} // namespace

extern "C" {

// The GameCube boot block: BetterSunshineEngine reads the console type to
// tell Dolphin from hardware. Zero reads as retail hardware.
unsigned char BootInfo[0x40];

void DCEnable(void) { }
void DCDisable(void) { }
void ICEnable(void) { }
void ICDisable(void) { }
void ICFlashInvalidate(void) { }

void OSCreateAlarm(void* alarm) { once("OSCreateAlarm"); if (alarm) memset(alarm, 0, 40); }
void OSSetPeriodicAlarm(void*, int64_t, int64_t, void*) { once("OSSetPeriodicAlarm"); }
void OSCancelAlarm(void*) { }
void* OSGetCurrentContext(void) { return 0; }
void __OSUnhandledException(uint8_t, void*, uint32_t, uint32_t) { once("__OSUnhandledException"); }

void __CARDSetDiskID(const void*) { once("__CARDSetDiskID"); }

typedef void (*AISCallback)(uint32_t);
AISCallback AIRegisterStreamCallback(AISCallback) { once("AIRegisterStreamCallback"); return 0; }
void AISetStreamTrigger(uint32_t) { }

uint32_t DVDCancelStream(void*) { once("DVDCancelStream"); return 0; }
int DVDGetStreamErrorStatusAsync(void*, void*) { once("DVDGetStreamErrorStatusAsync"); return 0; }

} // extern "C"
