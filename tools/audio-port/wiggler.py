"""GDB integration fixture for Wiggler's existing recovery/defeat music paths.

Warp to Gelato Beach episode 3 (SMS_WARP=4,2,1) with audio enabled.
Source this file before `run`; see README.md for the complete command.
The fixture injects two recovery states and the death state, then checks the
real audio thread's external tempo/pitch and the eventual stopped handle.
It does not simulate player attacks or validate the boss animations.
"""
import gdb

word = gdb.lookup_type('void').pointer().sizeof
visits = 0
defeating = False
slowed = False
finish_frames = 0


def value(expression):
    return gdb.parse_and_eval(expression)


def require(condition, message):
    if not condition:
        print('wiggler audio: FAIL ' + message, flush=True)
        gdb.execute('quit 1')


def nerve(name):
    # A forced state may precede its first natural theNerve() call. Give the
    # static instance its normal vptr without calling into a stopped thread.
    instance = "'%s::theNerve()::instance'" % name
    if int(value('(void*)*(void**)&' + instance)) == 0:
        gdb.execute("set {void*}&%s = (char*)&'vtable for %s' + %d" %
                    (instance, name, 2 * word))
    gdb.execute('set this->mSpine->mCurrent = (TNerveBase<TLiveActor>*)&' + instance)


class Fight(gdb.Breakpoint):
    def stop(self):
        global visits, defeating, slowed
        visits += 1
        if visits not in (100, 200, 300, 400, 1600):
            return False
        try:
            bgm = value('MSBgm::smBgmInTrack[1]')
            sound = bgm.dereference()['unk14'] if int(bgm) else 0
            require(int(sound) != 0, 'boss BGM stopped prematurely')
            gdb.execute('set $audio_seq = (JAISeqParameter*)MSBgm::smBgmInTrack[1]->unk14->mCustomParameter')
            gdb.execute('set $audio_root = $audio_seq->mUpdateData->mPlayerParams[JAIGlobalParameter::seqTrackMax].mTrack')
            tempo = float(value('$audio_root->mOuterParam->mTempo'))
            pitch = float(value('$audio_root->mOuterParam->mPitch'))
            if visits in (200, 300):
                expected = 1.07894 if visits == 200 else 1.15789
                require(abs(tempo - expected) < 0.0001,
                        'hit tempo %.6f, expected %.6f' % (tempo, expected))
            if visits == 400:
                require(abs(tempo - float(value('$audio_seq->mTempo.mCurrentValue'))) < 0.02,
                        'defeat tempo is not reaching the audio thread')
                require(pitch > 1.0, 'initial defeat pitch ramp is absent')
            if visits == 1600:
                require(tempo < 1.0 and pitch < 1.0, 'defeat slowdown is absent')
                slowed = True
            print('wiggler audio: visit=%d tempo=%.6f pitch=%.6f' %
                  (visits, tempo, pitch), flush=True)
            if visits in (100, 200):
                nerve('TNerveBossHanachanSnort')
                gdb.execute('set this->mSpine->mTime = 199')
                gdb.execute('set this->mHitPoints = %d' % (2 if visits == 100 else 1))
                gdb.execute('set this->mLiveFlag = this->mLiveFlag | 0x20000')
            if visits == 300:
                nerve('TNerveBossHanachanDead')
                defeating = True
        except gdb.error as error:
            require(False, str(error))
        return False


Fight('TBossHanachan::perform', internal=True)


class Finish(gdb.Breakpoint):
    def stop(self):
        global finish_frames
        if not defeating:
            return False
        finish_frames += 1
        bgm = value('MSBgm::smBgmInTrack[1]')
        if not int(bgm) or not int(bgm.dereference()['unk14']):
            require(slowed, 'music stopped before the slowdown was observed')
            print('wiggler audio: PASS two hit tempos, defeat pitch/tempo ramp, stop', flush=True)
            gdb.execute('quit 0')
        require(finish_frames < 1000, 'defeat music did not stop')
        return False


Finish('MSound::mainLoop', internal=True)
