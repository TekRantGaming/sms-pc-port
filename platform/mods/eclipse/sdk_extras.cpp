// Dolphin SDK functions BetterSunshineEngine calls that the retail game (and so
// the port) never needed: OS alarms, which run its music streamer (the DVD
// audio stream itself is platform/audio/dtk.cpp), and logged no-ops for the
// rest: cache control, the memory card's game-code switch, the exception
// handler and the stream calls BSE makes that only report status.
#include <stdarg.h>
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

int64_t OSGetTime(void);
typedef void (*port_irq_poll_fn)(void);
void port_irq_add_source(port_irq_poll_fn fn);

}

// OS alarms: each due alarm's handler runs in interrupt context on the
// emulated CPU at its next check point (at least each retrace), as the
// decrementer interrupt would run it. The alarm's own struct (laid out by
// SunshineHeaderInterface) is left alone; the port keeps its state here.
namespace {
typedef void (*AlarmHandler)(void* alarm, void* context);
struct Alarm {
	void* alarm;
	int64_t fire, period; // period 0: once
	AlarmHandler handler;
};
// built with the game's flags (C++03): a fixed table, no containers
Alarm g_alarms[16];
int g_alarm_count;

void cancel_alarm(void* alarm)
{
	for (int i = 0; i < g_alarm_count; i++)
		if (g_alarms[i].alarm == alarm) {
			g_alarms[i] = g_alarms[--g_alarm_count];
			return;
		}
}

void poll_alarms()
{
	const int64_t now = OSGetTime();
	for (int i = 0; i < g_alarm_count; i++) {
		Alarm a = g_alarms[i];
		if (a.fire > now)
			continue;
		if (a.period > 0) {
			// a stalled game does not get a burst of late periods
			int64_t next     = a.fire + a.period;
			g_alarms[i].fire = next > now ? next : now + a.period;
		} else {
			g_alarms[i--] = g_alarms[--g_alarm_count];
		}
		a.handler(a.alarm, NULL); // may set or cancel alarms
	}
}

void set_alarm(void* alarm, int64_t fire, int64_t period, void* handler)
{
	static bool polled;
	if (!polled) {
		polled = true;
		port_irq_add_source(poll_alarms);
	}
	cancel_alarm(alarm);
	if (!handler)
		return;
	if (g_alarm_count == (int)(sizeof g_alarms / sizeof g_alarms[0])) {
		fprintf(stderr, "[mod] too many OS alarms\n");
		return;
	}
	Alarm& a  = g_alarms[g_alarm_count++];
	a.alarm   = alarm;
	a.fire    = fire;
	a.period  = period;
	a.handler = (AlarmHandler)handler;
}
} // namespace

extern "C" {

void OSCreateAlarm(void* alarm) { cancel_alarm(alarm); }
void OSSetAlarm(void* alarm, int64_t tick, void* handler) { set_alarm(alarm, OSGetTime() + tick, 0, handler); }
void OSSetAbsAlarm(void* alarm, int64_t time, void* handler) { set_alarm(alarm, time, 0, handler); }
void OSSetPeriodicAlarm(void* alarm, int64_t start, int64_t period, void* handler)
{
	set_alarm(alarm, start + period, period, handler);
}
void OSCancelAlarm(void* alarm) { cancel_alarm(alarm); }
void* OSGetCurrentContext(void) { return 0; }
void __OSUnhandledException(uint8_t, void*, uint32_t, uint32_t) { once("__OSUnhandledException"); }

void __CARDSetDiskID(const void*) { once("__CARDSetDiskID"); }

// BetterSunshineEngine's Console::log and its kin hand their va_list to
// OSReport as one argument, so the values never reached the format (the
// moveset's state warnings printed a stack address as the state);
// fixup_sources.py sends the va_list here instead.
void sms_mod_vreport(const char* fmt, va_list ap)
{
	fputs("[OSReport] ", stderr);
	vfprintf(stderr, fmt, ap);
	fflush(stderr);
}

} // extern "C"
