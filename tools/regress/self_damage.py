#!/usr/bin/env python3
"""Observe self-directed damage and collision calls in scripted stage samples.

This is a diagnostic, not proof that every combat sequence is safe. Only
out-of-line function entries are observed; inlined calls are outside coverage.
It can also be sourced before popo.py in gdb to check the known Puffer fixture.
"""
import os
import re
from pathlib import Path

try:
    import gdb
except ImportError:
    gdb = None

ROOT = Path(__file__).resolve().parents[2]
# Damage-like messages from Strategic/HitActor.hpp. Sending one to oneself
# is a review candidate even if the receiving implementation rejects it.
DAMAGE = {0, 1, 3, 9, 10, 12, 14}


def sites():
    receivers, collisions = set(), set()
    for path in (ROOT / 'decomp/src').rglob('*.cpp'):
        source = path.read_text(errors='replace')
        for match in re.finditer(r'(?:bool|BOOL|void)\s+(\w+::receiveMessage)'
                                 r'\s*\(([^)]*)\)\s*\{', source):
            if re.fullmatch(r'\s*THitActor\s*\*\s*\w+\s*,\s*u32\s+\w+\s*',
                            match[2]):
                receivers.add(match[1])
        collisions.update(re.findall(r'bool\s+(\w+::isCollidMove)'
                                     r'\s*\(THitActor\s*\*[^)]*\)\s*\{', source))
    return receivers, collisions


def install():
    import collections
    state = dict(frames=0, active=os.environ.get('SELF_DAMAGE_FIXTURE') == '1',
                 calls=collections.Counter(), self_collisions=collections.Counter(),
                 installed=collections.Counter(), missing=[])
    word = gdb.lookup_type('void').pointer().sizeof

    def report(message):
        print('self-damage: ' + message, flush=True)

    def arguments(count):
        # Read the native ABI at the first instruction. Unused/optimised
        # parameter debug variables are often unavailable even at entry.
        if word == 8:
            return [int(gdb.parse_and_eval('$' + reg)) for reg in ('rdi', 'rsi', 'rdx')[:count]]
        return [int(gdb.parse_and_eval('((unsigned int*)$esp)[%d]' % i))
                for i in range(1, count + 1)]

    class Observe(gdb.Breakpoint):
        def __init__(self, function, kind):
            self.function, self.kind = function, kind
            super().__init__("*'%s'" % function, internal=True)

        def stop(self):
            try:
                if not state['active']:
                    return False
                actor, target, *message = arguments(3 if self.kind == 'receive' else 2)
                state['calls'][self.function] += 1
                if actor != target:
                    return False
                if self.kind == 'collision':
                    state['self_collisions'][self.function] += 1
                    return False
                if (message[0] & 0xffffffff) in DAMAGE:
                    report('FAIL self-directed damage: %s actor=%#x message=%d' %
                           (self.function, actor, message[0] & 0xffffffff))
                    gdb.execute('bt 8')
                    return True
            except gdb.error as error:
                report('FAIL debugger: ' + str(error))
                return True
            return False

    for functions, kind in zip(sites(), ('receive', 'collision')):
        for function in sorted(functions):
            try:
                Observe(function, kind)
                state['installed'][kind] += 1
            except gdb.error:
                state['missing'].append(function)
    report('installed %d receiver and %d collision entries; unavailable=%s' %
           (state['installed']['receive'], state['installed']['collision'],
            ','.join(state['missing']) or 'none'))

    class Direct(gdb.Breakpoint):
        def __init__(self):
            super().__init__("*'TMarDirector::direct()'", internal=True)

        def stop(self):
            try:
                director = gdb.parse_and_eval('this')
                stage, episode = map(int, os.environ['SMS_WARP'].split(',')[:2])
                if (int(director['mMap']) != stage or int(director['unk7D']) != episode
                        or not int(director['unk260']) or int(director['mState']) not in (4, 7)):
                    return False
                rate = int(os.environ['SMS_FRAME_RATE'])
                if int(gdb.parse_and_eval('port_active_frame_rate')) != rate:
                    report('FAIL incorrect active frame rate')
                    return True
                state['active'] = True
                state['frames'] += 1
                if state['frames'] < rate * int(os.environ.get('SELF_DAMAGE_SECONDS', '8')):
                    return False
                report('calls=' + str(dict(sorted(state['calls'].items()))))
                report('self collision requests=' + str(dict(state['self_collisions'])))
                report('CLEAN %d gameplay frames, %d calls, %d distinct entries observed' %
                       (state['frames'], sum(state['calls'].values()), len(state['calls'])))
                return True
            except gdb.error as error:
                report('FAIL debugger: ' + str(error))
                return True

    if os.environ.get('SELF_DAMAGE_FIXTURE') != '1':
        Direct()


def main():
    import argparse
    import subprocess
    from concurrent.futures import ThreadPoolExecutor
    import regress

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disc', required=True, help='image or extracted GMSE01 files folder')
    parser.add_argument('--build32', default='build/linux-32')
    parser.add_argument('--build64', default='build/linux-64')
    parser.add_argument('--arch', default='64')
    parser.add_argument('--fps', default='30,120')
    parser.add_argument('--scenes', default='2,4;3,0;4,0;5,0;6,0;9,0',
                        help='semicolon-separated stage,episode pairs')
    parser.add_argument('--seconds', type=int, default=8)
    parser.add_argument('--fixture', choices=['popo'], help='run the launch fixture instead of stage samples')
    parser.add_argument('--work', default='build/self-damage-audit')
    parser.add_argument('--timeout', type=int, default=300)
    args = parser.parse_args()
    archs, rates, scenes = args.arch.split(','), args.fps.split(','), args.scenes.split(';')
    if (any(a not in ('32', '64') for a in archs)
            or any(f not in ('30', '60', '120') for f in rates)
            or args.seconds < 1 or any(not re.fullmatch(r'\d+,\d+', s) for s in scenes)):
        parser.error('invalid architecture, frame rate, duration or scene')
    if not os.path.exists(args.disc):
        parser.error('disc path does not exist')
    executables = {a: str(Path(getattr(args, 'build' + a), 'sms').resolve()) for a in archs}
    if any(not Path(exe).is_file() for exe in executables.values()):
        parser.error('build each requested executable first')
    if args.fixture:
        scenes = ['2,4']

    def run(case):
        arch, fps, scene = case
        directory = Path(args.work, '%s-%s-%s' % (arch, fps, scene.replace(',', '-'))).resolve()
        directory.mkdir(parents=True, exist_ok=True)
        save = directory / 'save'
        save.mkdir(exist_ok=True)
        env = regress.base_env(str(save))
        env.update(SMS_WARP=scene + ',0', SMS_FRAME_RATE=fps, SELF_DAMAGE_SECONDS=str(args.seconds),
                   SMS_AUTOPRESS=regress.GATE_AP + ',A@3800,A@4100,STICK_UP@4400+120,A@4460+6')
        command = regress.GDB + ['-x', str(Path(__file__).resolve())]
        if args.fixture:
            env['SELF_DAMAGE_FIXTURE'] = '1'
            command += ['-x', str(ROOT / 'tools/regress/popo.py')]
        command += ['-ex', 'run', '--args', executables[arch], str(Path(args.disc).resolve())]
        log_path = directory / 'run.log'
        with log_path.open('wb') as log:
            process = subprocess.Popen(command, cwd=regress.ROOT, env=env,
                                       stdout=log, stderr=subprocess.STDOUT,
                                       stdin=subprocess.DEVNULL, start_new_session=True)
            try:
                process.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                regress.kill_group(process)
        contents = log_path.read_text(errors='replace')
        marker = 'popo: PASS' if args.fixture else 'self-damage: CLEAN'
        clean = (marker in contents and 'self-damage: FAIL' not in contents
                 and 'popo: FAIL' not in contents and 'Fatal signal:' not in contents)
        detail = next((line for line in contents.splitlines()
                       if line.startswith(('self-damage: CLEAN', 'popo: PASS'))
                       or ': FAIL' in line), 'see ' + str(log_path))
        print('%s %s-bit %s fps stage %s: %s' %
              ('CLEAN' if clean else 'FAIL', arch, fps, scene, detail), flush=True)
        return clean

    with ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(run, [(a, f, s) for a in archs for f in rates for s in scenes]))
    return 0 if all(results) else 1


if gdb is not None:
    install()
elif __name__ == '__main__':
    raise SystemExit(main())
