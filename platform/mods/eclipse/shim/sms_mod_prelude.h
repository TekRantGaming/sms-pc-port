// Force-included into every BetterSunshineEngine and Eclipse source the port
// compiles: headers their PowerPC build reaches through its own include order.
#pragma once
#include <stddef.h>
#include <Dolphin/types.h>
#ifdef __cplusplus
#include <JSystem/memory.hxx>
#endif

// The port's function for a retail address a mod calls through a cast
// (fixup_sources.py rewrites the address into a call of this).
#ifdef __cplusplus
extern "C"
#endif
void* sms_mod_rawaddr(unsigned int addr);

// The port's object for a retail data address a mod reads or writes
// (platform/mods/eclipse/rawdata.cpp; fixup_sources.py rewrites the casts).
#ifdef __cplusplus
extern "C"
#endif
void* sms_mod_rawdata(unsigned int addr);

// SMS_OFFSET(Class, retail offset): where the port keeps a member the mods
// address by retail offset (tools/mods/shi_layout/offsets.py).
#ifdef __cplusplus
#include <sms_offsets.h>
#endif
