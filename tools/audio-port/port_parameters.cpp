// Exercise real outerInit/setSePortParameter with a minimal track test double.
#include <JSystem/JAudio/JAInterface/JAISystemInterface.hpp>
#include <JSystem/JAudio/JAInterface/JAISound.hpp>
#include <JSystem/JAudio/JAInterface/JAIGlobalParameter.hpp>
#include <JSystem/JAudio/JASystem/JASCallback.hpp>
#include <dolphin/os.h>
#include <cassert>
#include <cstdio>
#include <cstring>

static s32 (*audioCallback)(void*);
static void* callbackData;
static int interruptions, neighbours;

namespace JASystem {
TTrack::TTrack() : mOuterParam(0) { }
void TOscillator::init() { }
TRegisterParam::TRegisterParam() { }
void TTrack::setInterrupt(u16 interrupt)
{
	assert(interrupt == 5);
	++interruptions;
}
void TTrack::muteTrack(u8) { }
namespace Kernel {
int registerAiCallback(s32 (*callback)(void*), void* data)
{
	audioCallback = callback;
	callbackData = data;
	return 0;
}
} }
u32 JAIGlobalParameter::seqTrackMax = 32;
u32 JAIGlobalParameter::getParamSeqTrackMax() { return seqTrackMax; }
JAISeqParameter* JAISound::getSeqParameter()
{
	return (JAISeqParameter*)mCustomParameter;
}
extern "C" BOOL OSDisableInterrupts() { return true; }
extern "C" BOOL OSRestoreInterrupts(BOOL) { return true; }
extern "C" void port_ptr32_trap(const void*) { assert(false); }

static void neighbour(JASystem::Kernel::TPortArgs*) { ++neighbours; }
static JASystem::TTrack track, replacement;

int main(int argc, char** argv)
{
	assert(argc == 2);
	JAIPlayerParameter players[33] = {};
	JAISeqUpdateData sequence = {};
	sequence.mPlayerParams = players;
	sequence.mSeqVolume = 0.75f;
	sequence.mSeqPitch = sequence.mSeqTempo = 1.0f;
	JASystem::TTrack::TOuterParam outer;
	track.mOuterParam = &outer;
	JASystem::Kernel::portCmdInit();

	if (std::strcmp(argv[1], "parameters") == 0) {
		JAISystemInterface::outerInit(&sequence, &track, 32, 0xffff, 0);
		audioCallback(callbackData);
		// Distinct updates share one pending command, as music fades and
		// spatial SE changes do. Every value and dirty flag must survive.
		const u8 slots[] = { 2, 3, 4, 5, 6, 9 };
		const u32 flags[] = { 1, 2, 4, 8, 0x10, 0x80 };
		const f32 values[] = { 0.3f, 0.8f, 0.9f, 0.4f, 0.6f, 1.2f };
		for (int i = 0; i < 6; ++i) {
			JAISystemInterface::setSeqPortargsF32(&sequence, 32, slots[i], values[i]);
			JAISystemInterface::setSeqPortargsU32(&sequence, 32, 1, flags[i]);
			assert(bool(players[32].mCmd.addPortCmdOnce()) == (i == 0));
		}
		JAISystemInterface::setSeqPortargsF32(&sequence, 32, 2, 0.2f);
		JAISystemInterface::setSeqPortargsU32(&sequence, 32, 1, 1);
		audioCallback(callbackData);
		assert(outer.mVolume == 0.2f && outer.mPitch == 0.8f);
		assert(outer.mPan == 0.9f && outer.mFxmix == 0.4f);
		assert(outer.mDolby == 0.6f && outer.mTempo == 1.2f);
		assert((players[32].mArgs.mFlags & 0xdf) == 0);
		assert(!players[32].mCmd.mHead && interruptions == 0);
	} else if (std::strcmp(argv[1], "interrupt") == 0) {
		JAISystemInterface::outerInit(&sequence, &track, 32, 0xffff, 0);
		audioCallback(callbackData);
		players[32].mArgs.unk20 = 1;
		JAISystemInterface::setSeqPortargsU32(&sequence, 32, 1, 0x40);
		assert(players[32].mCmd.addPortCmdOnce());
		audioCallback(callbackData);
		assert(interruptions == 1);
		// A later parameter-only update must not replay interrupt 5.
		JAISystemInterface::setSeqPortargsF32(&sequence, 32, 3, 0.8f);
		JAISystemInterface::setSeqPortargsU32(&sequence, 32, 1, 2);
		assert(players[32].mCmd.addPortCmdOnce());
		audioCallback(callbackData);
		assert(outer.mPitch == 0.8f && interruptions == 1);
	} else {
		assert(std::strcmp(argv[1], "rebind") == 0);
		// First use must initialise membership even on non-zero heap memory.
		players[32].mCmd.mHead = (JASystem::Kernel::TPortHead*)1;
		JAISystemInterface::outerInit(&sequence, &track, 32, 0xffff, 0);
		audioCallback(callbackData);
		for (int i = 0; i < 2; ++i)
			players[i].mCmd.setPortCmd(neighbour, &players[i].mArgs);
		assert(players[0].mCmd.addPortCmdOnce());
		assert(players[32].mCmd.addPortCmdOnce());
		assert(players[1].mCmd.addPortCmdOnce());
		// Starting/reinitialising the track before the audio tick rebinds
		// a command already in the middle of the shared queue.
		JASystem::TTrack::TOuterParam replacementOuter;
		replacement.mOuterParam = &replacementOuter;
		sequence.mSeqTempo = 1.15789f;
		JAISystemInterface::outerInit(&sequence, &replacement, 32, 0xffff, 0);
		audioCallback(callbackData);
		assert(neighbours == 2 && replacementOuter.mTempo == 1.15789f);
		assert(outer.mTempo == 1.0f);
		for (int i = 0; i < 33; ++i) assert(!players[i].mCmd.mHead);
		assert(players[32].mCmd.addPortCmdOnce());
		audioCallback(callbackData);
	}
	std::printf("audio port %s: PASS\n", argv[1]);
}
