// Asset-free regression for delayed consumption of JAudio's port commands.
#include <JSystem/JAudio/JAInterface/JAISystemInterface.hpp>
#include <JSystem/JAudio/JASystem/JASCallback.hpp>
#include <dolphin/os.h>
#include <cassert>
#include <cstdio>

static s32 (*audioCallback)(void*);
static void* callbackData;
static int consumed;
static float tempo, pitch;

namespace JASystem { namespace Kernel {
int registerAiCallback(s32 (*callback)(void*), void* data)
{
	audioCallback = callback;
	callbackData = data;
	return 0;
}
} }

extern "C" BOOL OSDisableInterrupts() { return true; }
extern "C" BOOL OSRestoreInterrupts(BOOL) { return true; }

static void consume(JASystem::Kernel::TPortArgs* args)
{
	if (args->mFlags & 0x80) tempo = args->mTrackTempo;
	if (args->mFlags & 2) pitch = args->mTrackPitch;
	args->mFlags = 0;
	++consumed;
}

int main()
{
	JAIPlayerParameter players[3] = {};
	JAISeqUpdateData sequence = {};
	sequence.mPlayerParams = players;
	JASystem::Kernel::portCmdInit();
	for (int i = 0; i < 3; ++i)
		players[i].mCmd.setPortCmd(consume, &players[i].mArgs);

	// Music between two SE commands; the audio thread is delayed while
	// two game frames change different parameters on the same command.
	for (int i = 0; i < 3; ++i)
		assert(players[i].mCmd.addPortCmdOnce());
	JAISystemInterface::setSeqPortargsF32(&sequence, 1, 9, 1.07894f);
	JAISystemInterface::setSeqPortargsU32(&sequence, 1, 1, 0x80);
	assert(!players[1].mCmd.addPortCmdOnce());
	JAISystemInterface::setSeqPortargsF32(&sequence, 1, 3, 0.2f);
	JAISystemInterface::setSeqPortargsU32(&sequence, 1, 1, 2);
	assert(!players[1].mCmd.addPortCmdOnce());
	assert(players[1].mArgs.mFlags == 0x82);
	assert(audioCallback(callbackData) == 0);
	assert(consumed == 3 && tempo == 1.07894f && pitch == 0.2f);
	for (int i = 0; i < 3; ++i) assert(!players[i].mCmd.mHead);

	// The consumed node can be queued again for the second hit, then
	// coalesce the final slowdown endpoint before the next audio tick.
	JAISystemInterface::setSeqPortargsF32(&sequence, 1, 9, 1.15789f);
	JAISystemInterface::setSeqPortargsU32(&sequence, 1, 1, 0x80);
	assert(players[1].mCmd.addPortCmdOnce());
	JAISystemInterface::setSeqPortargsF32(&sequence, 1, 9, 0.3f);
	JAISystemInterface::setSeqPortargsU32(&sequence, 1, 1, 0x80);
	assert(!players[1].mCmd.addPortCmdOnce());
	assert(audioCallback(callbackData) == 0);
	assert(consumed == 4 && tempo == 0.3f);
	assert(audioCallback(callbackData) == 0 && consumed == 4);

	// U32 arguments other than flags retain replacement semantics.
	JAISystemInterface::setSeqPortargsU32(&sequence, 1, 8, 3);
	JAISystemInterface::setSeqPortargsU32(&sequence, 1, 8, 1);
	assert(players[1].mArgs.unk20 == 1);
	std::puts("pending audio commands: PASS");
}
