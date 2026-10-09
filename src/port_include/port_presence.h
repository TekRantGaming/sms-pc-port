#ifndef PORT_PRESENCE_H
#define PORT_PRESENCE_H

// What the launcher's Discord Rich Presence shows, read by the game once a
// frame (patch zzz-presence-01) and printed by platform/presence when it
// changes. Plain integers, so the game and the port agree on nothing else.
typedef struct PortPresence {
	int state;     // TApplication::mAppState
	int area;      // mCurrArea's stage (TGameSequence::Area)
	int episode;   // flag 0x40003: the episode chosen on the select screen
	int shines;    // flag 0x40000
	int blueCoins; // flag 0x40001
	int lives;     // flag 0x20001
	int saveFile;  // TApplication::mSaveFile, -1 before a file is loaded
	int paused;    // TMarDirector's pause menu is open
	int cutscene;  // TMarDirector is playing an in-engine demo
} PortPresence;

#ifdef __cplusplus
extern "C"
#endif
void port_presence_frame(const PortPresence* presence);

#endif
