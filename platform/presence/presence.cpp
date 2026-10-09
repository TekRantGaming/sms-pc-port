// Discord Rich Presence feed: with SMS_PRESENCE=1 (the launcher sets it), each
// change in what the player is doing is printed to stdout as one
// `[presence] {...}` line. The launcher reads those lines and talks to Discord
// itself, so the game needs no network code and plays the same without it.
#include "port_presence.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

int g_enabled = -1; // -1 until SMS_PRESENCE is read
PortPresence g_last;

} // namespace

// Called by the game thread once a frame; cheap when nothing has changed.
extern "C" void port_presence_frame(const PortPresence* p)
{
	if (g_enabled < 0) {
		const char* e = getenv("SMS_PRESENCE");
		g_enabled     = e && !strcmp(e, "1");
		memset(&g_last, 0xFF, sizeof g_last); // print the first frame
	}
	if (!g_enabled || !memcmp(p, &g_last, sizeof g_last))
		return;
	g_last = *p;
	printf("[presence] {\"state\":%d,\"area\":%d,\"episode\":%d,\"shines\":%d,"
	       "\"blueCoins\":%d,\"lives\":%d,\"saveFile\":%d,\"paused\":%d,\"cutscene\":%d}\n",
	       p->state, p->area, p->episode, p->shines, p->blueCoins, p->lives,
	       p->saveFile, p->paused, p->cutscene);
	// A pipe is fully buffered; the launcher should see the change now.
	fflush(stdout);
}
