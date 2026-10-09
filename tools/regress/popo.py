#!/usr/bin/env python3
"""Check Puffer launches and Petey hits in both word sizes at 30/60/120 fps.

Run from the port checkout; --disc accepts a GMSE01 image or extracted files
folder. Inside gdb, this script attaches a real Puffer to FLUDD, charges and
releases it through checkTrigger, and lifts the projectile into clear air.
The ordinary physics, animation and collision passes then run for 20 native
updates. A forced auxiliary self-hit also covers rates where the hitboxes
do not naturally overlap. Finally, a scripted collision with the real
sleeping Petey must wake him and explode the Puffer.
"""
import os

try:
    import gdb
except ImportError:
    gdb = None


def install_breakpoints():
    state = dict(popo=None, boss=None, frames=0, attached=False, triggers=0,
                 lifted=False, self_hits=0)

    def report(message):
        print('popo: ' + message, flush=True)

    def finish(message):
        report(message)
        # Let batch gdb exit after the stop callback returns. Killing the
        # inferior inside a FinishBreakpoint callback crashes some gdb builds.
        return True

    def nerve(name):
        return int(gdb.parse_and_eval("&'%s::theNerve()::instance'" % name))

    def set_nerve(actor, name):
        # Set a test fixture without an inferior call. The singleton's
        # normal lazy initialiser may not have run yet on this path.
        gdb.execute("set var *(void**)&'%s::theNerve()::instance' = "
                    "(char*)&'vtable for %s' + 2*sizeof(void*)" % (name, name))
        gdb.execute("set var *(unsigned char*)&'guard variable for "
                    "%s::theNerve()::instance' = 1" % name)
        gdb.execute('set var %s->mSpine->mCurrent = '
                    "&'%s::theNerve()::instance'" % (actor, name))
        gdb.execute('set var %s->mSpine->mTime = 0' % actor)
        gdb.execute('set var %s->mSpine->mVertebrae.mSize = 0' % actor)

    class Entry(gdb.Breakpoint):
        def __init__(self, function):
            # Select the out-of-line entry only, avoiding inlined locations
            # at which the function's arguments may be optimised out.
            # Keep a symbolic location so PIE relocation works in 32-bit.
            super().__init__("*'%s'" % function)

        def stop(self):
            try:
                return self.check()
            except gdb.error as error:
                return finish('FAIL debugger: %s' % error)

    class Init(Entry):
        def __init__(self, species, field):
            self.field = field
            super().__init__('%s::init(TLiveManager*)' % species)

        def check(self):
            if state[self.field] is None:
                state[self.field] = int(gdb.parse_and_eval('this'))
            return False

    class Perform(Entry):
        def check(self):
            if int(gdb.parse_and_eval('this')) != state['popo']:
                return False
            if int(gdb.parse_and_eval('cue')) & 2:
                state['frames'] += 1
            if state['frames'] == 120 and not state['attached']:
                requested = int(os.environ['SMS_FRAME_RATE'])
                actual = int(gdb.parse_and_eval('port_active_frame_rate'))
                if actual != requested:
                    return finish('FAIL requested %d fps, active %d' % (requested, actual))
                state['attached'] = True
                set_nerve('this', 'TNervePopoPossessedNozzle')
                gdb.execute('set var this->mLiveFlag = 0x80')
                gdb.execute('set var this->mPosition = gpMarioOriginal->mPosition')
            if state['frames'] > 1200:
                return finish('FAIL launch did not complete')
            return False

    class Trigger(Entry):
        def check(self):
            if not state['attached'] or int(gdb.parse_and_eval('this')) != state['popo']:
                return False
            state['triggers'] += 1
            analog = 255 if state['triggers'] <= 30 else 0
            gdb.execute('set var gpMarioOriginal->mGamePad->mCompSPos[3] = %d' % analog)
            return False

    class HitResult(gdb.FinishBreakpoint):
        def stop(self):
            try:
                popo = gdb.parse_and_eval('(TPopo*)%#x' % state['popo'])
                current = int(popo['mSpine']['mCurrent'])
                if current != nerve('TNervePopoExplosion'):
                    return finish('FAIL Puffer did not explode on Petey hit')
                boss = gdb.parse_and_eval('(TBossPakkun*)%#x' % state['boss'])
                if int(boss['mSpine']['mCurrent']) != nerve('TNerveBPBreakSleep'):
                    return finish('FAIL Puffer did not wake Petey')
                return finish('PASS 20 flight updates, %d self-hits ignored, Petey woken' %
                              state['self_hits'])
            except gdb.error as error:
                return finish('FAIL hit result: %s' % error)

    class Bind(Entry):
        def check(self):
            if not state['attached'] or int(gdb.parse_and_eval('this')) != state['popo']:
                return False
            popo = gdb.parse_and_eval('this')
            current = int(popo['mSpine']['mCurrent'])
            if not state['lifted']:
                if current != nerve('TNervePopoFly'):
                    return False
                state['lifted'] = True
                gdb.execute('set var this->mPosition.y = this->mPosition.y + 2000')
            if current != nerve('TNervePopoFly'):
                return finish('FAIL premature explosion or interrupted flight')
            time = int(popo['mSpine']['mTime'])
            if time == 18:
                gdb.execute('set var this->mCollision->mColCount = 1')
                gdb.execute('set var this->mCollision->mCollisions[0] = this')
            if time == 20:
                if not state['self_hits'] or state['boss'] is None:
                    return finish('FAIL missing self-hit or Petey fixture')
                gdb.execute('set var this->mCollision->mColCount = 1')
                gdb.execute('set var this->mCollision->mCollisions[0] = (THitActor*)%#x' %
                            state['boss'])
                set_nerve('((TBossPakkun*)%#x)' % state['boss'], 'TNerveBPSleep')
                HitResult(gdb.newest_frame())
            return False

    class Collide(Entry):
        def check(self):
            if not state['lifted'] or int(gdb.parse_and_eval('this')) != state['popo']:
                return False
            if int(gdb.parse_and_eval('param_1')) == state['popo']:
                state['self_hits'] += 1
            return False

    class Receive(Entry):
        def check(self):
            if (state['lifted'] and int(gdb.parse_and_eval('this')) == state['popo']
                    and int(gdb.parse_and_eval('sender')) == state['popo']):
                return finish('FAIL Puffer received its own damage message')
            return False

    Init('TPopo', 'popo')
    Init('TBossPakkun', 'boss')
    Perform('TPopo::perform')
    Trigger('TPopo::checkTrigger()')
    Bind('TPopo::bind()')
    Collide('TPopo::isCollidMove(THitActor*)')
    Receive('TSmallEnemy::receiveMessage')


def main():
    import argparse
    import subprocess
    from concurrent.futures import ThreadPoolExecutor
    import regress

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disc', help='GMSE01 image or extracted files folder')
    parser.add_argument('--build32', default='build/linux-32')
    parser.add_argument('--build64', default='build/linux-64')
    parser.add_argument('--arch', default='32,64')
    parser.add_argument('--fps', default='30,60,120')
    parser.add_argument('--work', default='build/popo-regress')
    parser.add_argument('--timeout', type=int, default=300)
    args = parser.parse_args()
    disc = args.disc or regress.find_disc()
    if not disc or not os.path.exists(disc):
        parser.error('supply a GMSE01 image or extracted files folder with --disc')
    disc = os.path.abspath(disc)
    archs, rates = args.arch.split(','), args.fps.split(',')
    if any(a not in ('32', '64') for a in archs) or any(f not in ('30', '60', '120') for f in rates):
        parser.error('--arch takes 32,64; --fps takes 30,60,120')
    executables = {a: os.path.abspath(getattr(args, 'build' + a) + '/sms') for a in archs}
    if any(not os.path.isfile(exe) for exe in executables.values()):
        parser.error('build each requested executable first')

    def run(arch, fps):
        directory = os.path.abspath(os.path.join(args.work, '%s-%s' % (arch, fps)))
        os.makedirs(directory, exist_ok=True)
        save = os.path.join(directory, 'save')
        os.makedirs(save, exist_ok=True)
        env = regress.base_env(save)
        env.update(SMS_WARP='2,4,0', SMS_FRAME_RATE=fps,
                   SMS_AUTOPRESS=regress.GATE_AP + ',A@3800,A@4100')
        command = regress.GDB + ['-x', os.path.abspath(__file__), '-ex', 'run',
                                 '--args', executables[arch], disc]
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
        passed = ('popo: PASS' in contents and 'popo: FAIL' not in contents
                  and 'Fatal signal:' not in contents)
        detail = next((line for line in contents.splitlines() if line.startswith('popo: FAIL')),
                      'see ' + log_path)
        print('%s Puffer %s-bit %s fps: %s' %
              ('PASS' if passed else 'FAIL', arch, fps,
               'flight survives self-hits; Petey wakes on impact' if passed else detail), flush=True)
        return passed

    with ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(lambda pair: run(*pair),
                                [(a, f) for f in rates for a in archs]))
    return 0 if all(results) else 1


if gdb is not None:
    install_breakpoints()
elif __name__ == '__main__':
    raise SystemExit(main())
