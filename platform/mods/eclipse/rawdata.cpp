// Game data code mods reach by retail address, *(T *)0x8040E0BC and the like
// (fixup_sources.py routes those casts through sms_mod_rawdata): the port's
// object at each address. Retail addresses from the disc's map.
#include <Enemy/Graph.hpp>
#include <Enemy/RocketNerve.hpp>
#include <MSound/MSModBgm.hpp>
#include <MarioUtil/ScreenUtil.hpp>
#include <NPC/NpcNerve.hpp>
#include <dolphin/gx.h>
#include <stdio.h>
#include <stdlib.h>

extern TRailNode sms_port_grDummyRail[3];
extern const char* sms_port_streamMovies[20];
extern "C" GXColor sms_port_emarioWaterColor;

extern "C" void* sms_mod_rawdata(unsigned int addr)
{
	switch (addr) {
	case 0x803ACA68: return MSBgmXFade::scTiming;                                // scTiming__10MSBgmXFade
	case 0x803ACAB0: return MSBgmXFade::scExp;                                   // scExp__10MSBgmXFade
	case 0x803AFB48: return sms_port_grDummyRail;                                // grDummyRail (graph.cpp)
	case 0x803DFA00: return sms_port_streamMovies;                               // movies$2059 (TMovieDirector::getStreamMovieName)
	case 0x8040DAB4: return (void*)&TNerveRocketPossessedNozzle::theNerve();     // instance$2890
	case 0x8040DABC: return (void*)&TNerveRocketFly::theNerve();                 // instance$2904
	case 0x8040DFD4: return (void*)&TNerveNPCGraphWander::theNerve();            // instance$2212
	case 0x8040DFE4: return (void*)&TNerveNPCGraphWait::theNerve();              // instance$2251
	case 0x8040DFF4: return (void*)&TNerveNPCWaitMarioApproach::theNerve();      // instance$2275
	case 0x8040E03C: return (void*)&TNerveNPCMad::theNerve();                    // instance$2414
	case 0x8040E0BC: return &gpScreenTexture;                                    // gpScreenTexture
	case 0x8040FA90: return &sms_port_emarioWaterColor;                          // @3761 (TEnemyMario::drawHPMeter)
	}
	fprintf(stderr, "[mod] retail data address %08x has no port object (platform/mods/eclipse/rawdata.cpp)\n", addr);
	abort();
}
