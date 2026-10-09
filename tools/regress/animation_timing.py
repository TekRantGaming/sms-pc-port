#!/usr/bin/env python3
"""Check Petey breathing and duck dizzy particles at 30, 60 and 120 fps.

The fixture selects the ordinary vomit nerve on a loaded boss; its own
changeBck and MActor updates run normally. No animation rate is injected.
The duck fixture selects the stomp animation state and requires its real
emitter to reach the renderer with visible, finite particles.
"""
import os

try:
    import gdb
except ImportError:
    gdb = None


def install():
    fps = int(os.environ['SMS_FRAME_RATE'])
    state = dict(actor=None, ready=False, started=False, frames=0,
                 initial=None, calls=0, native=None)

    def finish(message):
        print('animation: ' + message, flush=True)
        return True

    def animation(actor):
        return actor['mMActor']['mAnmByType'][0]

    class Rate(gdb.FinishBreakpoint):
        def __init__(self, actor):
            super().__init__(gdb.newest_frame(), internal=True)
            self.actor = actor

        def stop(self):
            rate = float(animation(self.actor)['unk4']['mRate'])
            expected = state['native'] * 30 / fps
            if abs(rate - expected) > 0.00001:
                return finish('FAIL Petey %d fps rate %.6f, expected %.6f' %
                              (fps, rate, expected))
            state['started'] = True
            print('animation: Petey %d fps rate %.6f' % (fps, rate), flush=True)
            return False

    class Change(gdb.Breakpoint):
        def stop(self):
            if state['ready'] and int(gdb.parse_and_eval('index')) == 21:
                state['native'] = float(gdb.parse_and_eval(
                    '((TBossPakkunParams*)((TEnemyManager*)this->mManager)->unk38)'
                    '->mSLVomitAnmRate.value'))
                Rate(gdb.parse_and_eval('this'))
            return False

    class Perform(gdb.Breakpoint):
        def stop(self):
            if not int(gdb.parse_and_eval('cue')) & 2:
                return False
            actor = gdb.parse_and_eval('this')
            if not int(actor['mMtxCalc']):
                return False
            state['calls'] += 1
            if state['calls'] == 30:
                actual = int(gdb.parse_and_eval('port_active_frame_rate'))
                if actual != fps:
                    return finish('FAIL requested %d fps, active %d' % (fps, actual))
                name = 'TNerveBPVomit'
                gdb.execute("set var *(void**)&'%s::theNerve()::instance' = "
                            "(char*)&'vtable for %s' + 2*sizeof(void*)" % (name, name))
                gdb.execute("set var *(unsigned char*)&'guard variable for "
                            "%s::theNerve()::instance' = 1" % name)
                gdb.execute("set var this->mSpine->mCurrent = "
                            "&'%s::theNerve()::instance'" % name)
                gdb.execute('set var this->mSpine->mTime = 0')
                gdb.execute('set var this->mSpine->mVertebrae.mSize = 0')
                state['ready'] = True
            if state['ready']:
                # Keep the off-camera fixture participating in animation.
                gdb.execute('set var this->mLiveFlag = 0x80')
            if state['started']:
                ctrl = animation(actor)['unk4']
                frame = float(ctrl['mFrame'])
                if state['initial'] is None:
                    state['initial'] = frame
                else:
                    state['frames'] += 1
                if state['frames'] == fps:
                    advance = frame - state['initial']
                    if abs(advance - state['native'] * 30) > 0.001:
                        return finish('FAIL Petey %d fps advances %.6f frames/s' % (fps, advance))
                    return finish('PASS Petey %d fps advances %.6f frames/s' % (fps, advance))
            if state['calls'] > fps * 20:
                return finish('FAIL fixture never entered breathing animation')
            return False

    Perform("*'TBossPakkun::perform(unsigned int, JDrama::TGraphics*)'")
    Change("*'TBossPakkun::changeBck(int)'")


def install_duck():
    import math
    state = dict(calls=0, actor=None)

    class Stun(gdb.Breakpoint):
        def stop(self):
            state['calls'] += 1
            if state['calls'] != 120:
                return False
            state['actor'] = int(gdb.parse_and_eval('this'))
            name = 'TNervePoihanaFreeze'
            gdb.execute("set var *(void**)&'%s::theNerve()::instance' = "
                        "(char*)&'vtable for %s' + 2*sizeof(void*)" % (name, name))
            gdb.execute("set var *(unsigned char*)&'guard variable for "
                        "%s::theNerve()::instance' = 1" % name)
            gdb.execute("set var this->mSpine->mCurrent = "
                        "&'%s::theNerve()::instance'" % name)
            gdb.execute('set var this->mSpine->mTime = 1')
            # The state selected by isHitValid after a stomp on a stunned
            # duck. Ordinary calcRootMatrix, emit and draw remain untouched.
            gdb.execute('set var this->mCurrentBckAnm = 3')
            self.enabled = False
            return False

    class Draw(gdb.Breakpoint):
        def stop(self):
            if state['actor'] is None:
                return False
            manager = gdb.parse_and_eval('gpMarioParticleManager')
            emitter = gdb.parse_and_eval('this->mDrawCtx.mBaseEmitter')
            for i in range(int(manager['unk3B4'])):
                info = manager['unk10'][303 - 253][i]
                if int(info['unk0']) != state['actor'] or info['mEmitter'] != emitter:
                    continue
                particles = emitter['mParticleList']
                if int(particles['mLinkCount']) == 0:
                    return False
                pointer = particles['mHead']['mData']
                particle = pointer.cast(gdb.lookup_type('JPAParticle').pointer())
                if int(particle['unk10']) & 8 or float(particle['mDrawParams']['mAlpha']) <= 0:
                    return False
                if not all(math.isfinite(float(particle['mGlobalPosition'][axis]))
                           for axis in ('x', 'y', 'z')):
                    print('animation: FAIL duck dizzy particle has invalid position', flush=True)
                    return True
                print('animation: PASS duck %s fps dizzy emitter draws %d visible particles' %
                      (os.environ['SMS_FRAME_RATE'], int(particles['mLinkCount'])), flush=True)
                return True
            return False

    Stun("*'TPoiHana::calcRootMatrix()'")
    Draw("*'JPADraw::draw(float (*) [4])'")


def main():
    import argparse
    import subprocess
    import regress
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disc', default=regress.find_disc())
    parser.add_argument('--exe', default='build/linux-64/sms')
    parser.add_argument('--fps', default='30,60,120')
    parser.add_argument('--fixture', choices=('petey', 'duck'), default='petey')
    parser.add_argument('--work', default='build/animation-timing')
    parser.add_argument('--timeout', type=int, default=240)
    args = parser.parse_args()
    if not args.disc or not os.path.exists(args.disc):
        parser.error('supply a GMSE01 image or extracted files folder with --disc')
    rates = args.fps.split(',')
    if any(rate not in ('30', '60', '120') for rate in rates):
        parser.error('--fps takes 30,60,120')
    passed = True
    for rate in rates:
        directory = os.path.abspath(os.path.join(args.work, rate))
        save = os.path.join(directory, 'save')
        os.makedirs(save, exist_ok=True)
        env = regress.base_env(save)
        env.update(SMS_WARP='2,4,0' if args.fixture == 'petey' else '4,0,0',
                   REGRESS_ANIMATION_FIXTURE=args.fixture, SMS_FRAME_RATE=rate,
                   SMS_AUTOPRESS=regress.GATE_AP + ',A@3800,A@4100')
        command = regress.GDB + ['-x', os.path.abspath(__file__), '-ex', 'run',
                                 '--args', os.path.abspath(args.exe), os.path.abspath(args.disc)]
        log_path = os.path.join(directory, 'run.log')
        with open(log_path, 'wb') as log:
            process = subprocess.Popen(command, cwd=regress.ROOT, env=env,
                                       stdout=log, stderr=subprocess.STDOUT,
                                       stdin=subprocess.DEVNULL, start_new_session=True)
            try:
                process.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                regress.kill_group(process)
        with open(log_path, errors='replace') as log:
            contents = log.read()
        ok = ('animation: PASS' in contents and 'animation: FAIL' not in contents
              and 'Fatal signal:' not in contents and 'Python Exception' not in contents)
        result = next((line for line in contents.splitlines() if line.startswith('animation: PASS')
                       or line.startswith('animation: FAIL')), 'FAIL: see ' + log_path)
        print(result, flush=True)
        passed = passed and ok
    return 0 if passed else 1


if gdb is not None:
    if os.environ.get('REGRESS_ANIMATION_FIXTURE', 'petey') == 'duck':
        install_duck()
    else:
        install()
elif __name__ == '__main__':
    raise SystemExit(main())
