// ARAM (16 MiB auxiliary RAM) as a host buffer; ARQ transfers copy
// immediately and complete as interrupts.
#include "port_compat.h"
#include "port_os.h"
#include "port_platform.h"
#include <dolphin/ar.h>
#include <dolphin/arq.h>

namespace {
const u32 kAramSize = 16u << 20;
const u32 kAramBase = 0x4000;
u8* g_aram;
u32 g_alloc = kAramBase;

void copy(u32 type, u32 src, u32 dst, u32 len)
{
	if (type == ARAM_DIR_MRAM_TO_ARAM) {
		if (dst + len <= kAramSize)
			memcpy(g_aram + dst, (void*)(uintptr_t)src, len);
		else
			port_log("[ar] write out of range 0x%x+0x%x\n", dst, len);
	} else {
		if (src + len <= kAramSize)
			memcpy((void*)(uintptr_t)dst, g_aram + src, len);
		else
			port_log("[ar] read out of range 0x%x+0x%x\n", src, len);
	}
}
} // namespace

extern "C" u32 ARInit(u32*, u32)
{
	if (!g_aram)
		g_aram = (u8*)calloc(1, kAramSize);
	return kAramBase;
}
extern "C" int ARCheckInit(void) { return g_aram != NULL; }
extern "C" u32 ARAlloc(u32 length)
{
	u32 p = g_alloc;
	g_alloc += length;
	return p;
}
extern "C" u32 ARFree(u32* length) { return g_alloc; }
extern "C" u32 ARGetBaseAddress(void) { return kAramBase; }
extern "C" u32 ARGetSize(void) { return kAramSize; }
extern "C" u32 ARGetDMAStatus(void) { return 0; }
extern "C" void ARStartDMA(u32 type, u32 mainmem, u32 aram, u32 length)
{
	if (type == ARAM_DIR_MRAM_TO_ARAM)
		copy(type, mainmem, aram, length);
	else
		copy(type, aram, mainmem, length);
}
extern "C" void ARQInit(void) {}
extern "C" void ARQPostRequest(ARQRequest* req, u32 owner, u32 type, u32 prio, u32 source, u32 dest, u32 length,
                               ARQCallback callback)
{
	req->owner    = owner;
	req->type     = type;
	req->priority = prio;
	req->source   = source;
	req->dest     = dest;
	req->length   = length;
	req->callback = callback;
	copy(type, source, dest, length);
	// The copy is instant, so the completion interrupt fires before returning:
	// JASystem::Dvd busy-waits on the callback's counter without any OS call.
	if (callback)
		port_irq_run_now([req, callback]() { callback((u32)(uintptr_t)req); });
}
extern "C" u8* port_aram_ptr(u32 addr) { return g_aram ? g_aram + addr : NULL; }
