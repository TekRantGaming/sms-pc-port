#!/usr/bin/env python3
"""Regression checks: scripted headless runs, hashed and compared.

usage: tools/regress/regress.py [--check | --record] [options] CHECK...

CHECK is a group (title, plaza, fps60, eclipse, vanilla, all) or one run
(--list shows them). Every run is made in each word size (--arch) with the
deterministic clock (SMS_VI_DETERMINISTIC), no settings file, no texture packs,
skipped movies and an empty memory card (the Eclipse runs: the card saved by
ecl-firstboot, whose files are hashed too), and its captures are hashed; the
captures are deleted afterwards unless --keep is given, the logs are kept in
the --work folder.
With the default two runs at a time, all takes about 23 minutes on four cores
with llvmpipe (vanilla 10, eclipse 13).

--check (the default) compares each run's hashes with tools/regress/baseline.txt
and the 32-bit run's with the 64-bit run's, which must be identical (the gate
run's setNextStage actor is a game-heap address, which differs between the
word sizes; its stage and frames must not). --record runs the same checks
except the baseline and replaces the baseline entries of the runs it made; it
refuses to record when 32 and 64-bit differ.

Nothing is built: the executables are taken as they are from build/linux-32
and build/linux-64 (vanilla), build-ecl and build-ecl64 (Eclipse,
docs/ECLIPSE.md), or the folders given with --build32/--build64/--ecl32/--ecl64.
"""
import argparse
import hashlib
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.join(ROOT, 'tools', 'regress')
BASELINE = os.path.join(HERE, 'baseline.txt')

# Scripted input (SMS_AUTOPRESS, fields in retail numbering).
# Plaza: title, file select, new game on file A, then a short walk in Delfino
# Plaza (SMS_WARP=1,0,1 sends the new file there right after the airstrip).
PLAZA_AP = 'START@1400,STICK_LEFT@2000,A@2400,STICK_LEFT@3100,A@3250,A@3450,A@3800,STICK_UP@4400,B@4800'
# Gate: the same file-select presses, ending in the plaza (SMS_WARP=1,5,0).
GATE_AP = 'START@1400,STICK_LEFT@2000,A@2400,STICK_LEFT@3100,A@3250,A@3450'
# Eclipse, after BSE's first-boot settings screen has been saved: title, file
# select, the Tutorial's dialogue, a walk and a jump, the pause menu's Exit
# Area, and the title that follows.
ECL_TUTORIAL_AP = ('A@1300+6,A@1420+6,A@1540+6,A@1660+6,A@1780+6,A@1900+6,A@2020+6,A@2140+6,'
                   'A@2260+6,A@2380+6,STICK_UP@2550+60,A@2700+6,STICK_LEFT@2800+40,START@3400+8,'
                   'STICK_DOWN@3600+8,STICK_DOWN@3700+8,A@3800+8')
# Eclipse warps: the Tutorial input, then A through the warped stage's opening.
ECL_WARP_AP = ECL_TUTORIAL_AP + (',A@4700+8,A@5000+8,A@5300+8,A@5600+8,A@5900+8,A@6400+6,A@6520+6,'
                                 'A@6640+6,A@6760+6,A@6880+6,A@7000+6,A@7120+6,A@7240+6,A@7360+6,'
                                 'A@7480+6,A@7600+6,A@7720+6')

# The audio check hashes this much of the recorded WAV's sample data (88 s of
# 32 kHz stereo s16), which the run reaches by its last capture at field 5400.
WAV_BYTES = 32000 * 4 * 88

# kind: 'shots' (captures), 'audio' (captures and the WAV), 'gate' (gdb).
RUNS = {
    'title': dict(game='vanilla', kind='shots', shots=[300, 600, 900, 1200, 1500], timeout=600,
                  desc='boot to the title screen, captures at fields 300-1500'),
    'plaza': dict(game='vanilla', kind='shots', env={'SMS_WARP': '1,0,1', 'SMS_AUTOPRESS': PLAZA_AP},
                  shots=[3700, 4100, 4400, 4600, 4800, 5000, 5400], timeout=900,
                  desc='scripted new game into Delfino Plaza, captures at fields 3700-5400'),
    'plaza-audio': dict(game='vanilla', kind='audio',
                        env={'SMS_WARP': '1,0,1', 'SMS_AUTOPRESS': PLAZA_AP, 'SMS_AUDIO': '1'},
                        shots=[5400], timeout=900,
                        desc='the plaza run with audio, first 88 s of SMS_AUDIO_WAV'),
    'gate30': dict(game='vanilla', kind='gate', fps=30, timeout=1500,
                   desc='plaza gate under gdb at 30 fps: Mario captured, setNextStage'),
    'gate60': dict(game='vanilla', kind='gate', fps=60, timeout=1500,
                   desc='the same at SMS_FRAME_RATE=60: captured after 100 frames, same setNextStage'),
    'ecl-firstboot': dict(game='eclipse', kind='shots', env={'SMS_AUTOPRESS': 'B@650+10'},
                          shots=[600, 1200, 1500], timeout=900, cards=True,
                          desc="Eclipse first boot: BSE's settings screen, saved; its card seeds the other Eclipse runs"),
    'ecl-tutorial': dict(game='eclipse', kind='shots', env={'SMS_AUTOPRESS': ECL_TUTORIAL_AP}, card='ecl-firstboot',
                         shots=[1200, 2620, 2760, 3000, 3750, 3900, 4200, 4600, 5000], timeout=1200,
                         desc='Eclipse Tutorial: dialogue, walk, jump, Exit Area, title'),
    'ecl-petey': dict(game='eclipse', kind='shots', env={'SMS_AUTOPRESS': ECL_WARP_AP, 'SMS_WARP': '72,0'},
                      card='ecl-firstboot', shots=[5700, 6000, 6600, 7200, 7800], timeout=1500,
                      desc='Eclipse Fire Petey (SMS_WARP=72,0)'),
    # Luigi and Piantissimo, whom only a save that has unlocked them can
    # pick: tools/regress/character.py (gdb) sets the character as the
    # Tutorial loads and reports the moveset's better_movement.prm values and
    # the character's jumps (items prm:*, mario:*, jumpN).
    'ecl-luigi': dict(game='eclipse', kind='shots', env={'SMS_AUTOPRESS': ECL_TUTORIAL_AP},
                      gdb=('character.py', {'REGRESS_CHARACTER': '1'}), card='ecl-firstboot',
                      shots=[2620, 2760, 3000], timeout=900,
                      desc='Eclipse Tutorial as Luigi (gdb): his better_movement.prm and jumps'),
    'ecl-piantissimo': dict(game='eclipse', kind='shots', env={'SMS_AUTOPRESS': ECL_TUTORIAL_AP},
                            gdb=('character.py', {'REGRESS_CHARACTER': '2'}), card='ecl-firstboot',
                            shots=[2620, 2760, 3000], timeout=900,
                            desc='Eclipse Tutorial as Piantissimo (gdb): his better_movement.prm and jumps'),
    'ecl-zhine': dict(game='eclipse', kind='shots', env={'SMS_AUTOPRESS': ECL_WARP_AP, 'SMS_WARP': '79,0'},
                      card='ecl-firstboot', shots=[5700, 6000, 6600, 7200, 7800, 8400], timeout=1500,
                      desc='Eclipse Dark Zhine (SMS_WARP=79,0)'),
}
GROUPS = {
    'title': ['title'],
    'plaza': ['plaza', 'plaza-audio'],
    'fps60': ['gate30', 'gate60'],
    'eclipse': ['ecl-firstboot', 'ecl-tutorial', 'ecl-luigi', 'ecl-piantissimo', 'ecl-petey', 'ecl-zhine'],
}
GROUPS['vanilla'] = GROUPS['title'] + GROUPS['plaza'] + GROUPS['fps60']
GROUPS['all'] = GROUPS['vanilla'] + GROUPS['eclipse']

ECLIPSE_DISC = os.path.join(ROOT, 'mods', 'eclipse', 'Super Mario Eclipse v1.1.0.iso')
DISC_EXTS = ('iso', 'gcm', 'ciso')

print_lock = threading.Lock()


def say(msg):
    with print_lock:
        print(msg, flush=True)


def find_disc():
    # rom/ (as run.sh), then the decomp's orig/ folder, then a sibling
    # sms-english clone's (this machine's layout).
    for d in (os.path.join(ROOT, 'rom'), os.path.join(ROOT, 'decomp', 'orig', 'GMSE01'),
              os.path.join(os.path.dirname(ROOT), 'sms-english', 'orig', 'GMSE01')):
        if os.path.isdir(d):
            found = sorted(f for f in os.listdir(d) if f.rsplit('.', 1)[-1].lower() in DISC_EXTS)
            if len(found) == 1:
                return os.path.join(d, found[0])
    return None


def short_hash(data):
    return hashlib.sha256(data).hexdigest()[:16]


def file_hash(path):
    with open(path, 'rb') as f:
        return short_hash(f.read())


def image_hash(exe):
    """Hash of the executable's loadable image, without its build ID."""
    tmp = exe + '.regress-img'
    try:
        r = subprocess.run(['objcopy', '-O', 'binary', '--remove-section=.note.gnu.build-id', exe, tmp],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return file_hash(tmp) if r.returncode == 0 else '?'
    except OSError:
        return '?'
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def base_env(save_dir, shot_dir=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith('SMS_')}
    env.update({
        'SMS_SETTINGS': os.path.join(HERE, 'empty-settings.txt'),
        'SMS_TEXTURE_PACKS': '0',
        'SMS_HEADLESS': '1',
        'SMS_AUDIO': '0',
        'SMS_SKIP_MOVIES': '1',
        'SMS_VI_DETERMINISTIC': '1',
        'SMS_QUIET_STUBS': '1',
        'SMS_SAVE_DIR': save_dir,
    })
    if shot_dir:
        env['SMS_SHOT_DIR'] = shot_dir
    return env


def kill_group(p):
    try:
        os.killpg(p.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    p.wait()


class Result:
    def __init__(self, run, arch):
        self.run, self.arch = run, arch
        self.items = {}      # item -> value
        self.error = None
        self.seconds = 0


GDB = ['gdb', '-q', '-batch', '-nx',
       '-ex', 'set debuginfod enabled off', '-ex', 'set pagination off', '-ex', 'set confirm off',
       '-ex', 'handle SIGSEGV stop', '-ex', 'handle SIG34 nostop noprint',
       '-ex', 'handle SIGPIPE nostop noprint']


def do_run(name, arch, exe, disc, work, keep):
    spec = RUNS[name]
    res = Result(name, arch)
    d = os.path.join(work, '%s-%s' % (name, arch))
    shutil.rmtree(d, ignore_errors=True)
    shots_dir, save_dir = os.path.join(d, 'shots'), os.path.join(d, 'save')
    os.makedirs(shots_dir)
    os.makedirs(save_dir)
    if spec.get('card'):
        src = os.path.join(work, '%s-%s' % (spec['card'], arch), 'save')
        if not os.path.isdir(src):
            res.error = 'no memory card from %s' % spec['card']
            return res
        shutil.copytree(src, save_dir, dirs_exist_ok=True)
    env = base_env(save_dir, shots_dir)
    env.update(spec.get('env', {}))
    if spec.get('shots'):
        env['SMS_SHOTS'] = ','.join(str(n) for n in spec['shots'])
    log_path = os.path.join(d, 'run.log')
    wav = os.path.join(d, 'audio.wav')
    if spec['kind'] == 'audio':
        env['SMS_AUDIO_WAV'] = wav
    t0 = time.time()
    if spec['kind'] == 'gate':
        env.update({'SMS_WARP': '1,5,0', 'SMS_FRAME_RATE': str(spec['fps']), 'SMS_AUTOPRESS': GATE_AP})
        del env['SMS_SHOT_DIR']
        cmd = GDB + ['-x', os.path.join(HERE, 'gate.py'), '-ex', 'run', '--args', exe, disc]
        with open(log_path, 'wb') as log:
            p = subprocess.Popen(cmd, cwd=os.path.dirname(exe), env=env, stdout=log, stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
            try:
                p.wait(timeout=spec['timeout'])
            except subprocess.TimeoutExpired:
                res.error = 'timed out after %d s' % spec['timeout']
            kill_group(p)
        res.seconds = time.time() - t0
        text = open(log_path, errors='replace').read()
        m_err = re.search(r'^gate: error: (.*)$', text, re.M)
        m_in = re.search(r'^gate: warpIn frame (\d+)$', text, re.M)
        m_ns = re.search(r'^gate: setNextStage\(([^,]+), (.+)\) after (\d+) frames$', text, re.M)
        if m_err:
            res.error = 'gdb script: ' + m_err.group(1)
        elif not (m_in and m_ns):
            gave = re.search(r'^gate: (gave up.*)$', text, re.M)
            res.error = res.error or (gave.group(1) if gave else 'no capture or setNextStage (see %s)' % log_path)
        else:
            res.items['warpIn'] = 'frame=%s' % m_in.group(1)
            res.items['setNextStage'] = 'stage=%s,actor=%s,frames=%s' % tuple(
                re.sub(r'\s+', '_', g) for g in m_ns.groups())
        return res

    last = max(spec['shots'])
    last_path = os.path.join(shots_dir, 'field%05d.ppm' % last)
    marker = ('[vi] captured field %d ' % last).encode()
    cmd = [exe, disc]
    if spec.get('gdb'):
        script, script_env = spec['gdb']
        env.update(script_env)
        cmd = GDB + ['-x', os.path.join(HERE, script), '-ex', 'run', '--args', exe, disc]
    with open(log_path, 'wb') as log:
        p = subprocess.Popen(cmd, cwd=os.path.dirname(exe), env=env, stdout=log,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
        deadline = t0 + spec['timeout']
        while True:
            if p.poll() is not None:
                break
            if time.time() > deadline:
                res.error = 'timed out after %d s' % spec['timeout']
                break
            done = os.path.exists(last_path)
            if done:
                with open(log_path, 'rb') as f:
                    done = marker in f.read()
            if done and spec['kind'] == 'audio':
                done = os.path.exists(wav) and os.path.getsize(wav) >= 44 + WAV_BYTES
            if done:
                break
            time.sleep(1)
        kill_group(p)
    res.seconds = time.time() - t0
    text = open(log_path, errors='replace').read()
    if 'fatal signal' in text and not res.error:
        res.error = 'the game faulted (see %s)' % log_path
    for n in spec['shots']:
        f = os.path.join(shots_dir, 'field%05d.ppm' % n)
        if os.path.exists(f):
            res.items['field%05d' % n] = file_hash(f)
        elif not res.error:
            res.error = 'field %d was not captured (see %s)' % (n, log_path)
    if spec.get('gdb'):
        # What the gdb script reports: "regress: ITEM VALUE".
        for m in re.finditer(r'^regress: (\S+) (\S+)$', text, re.M):
            res.items[m.group(1)] = m.group(2)
        if re.search(r'^Python Exception', text, re.M) and not res.error:
            res.error = 'the gdb script failed (see %s)' % log_path
    if spec.get('cards') and not res.error:
        # The memory card the run saved, file by file: the same bytes in
        # both word sizes, as the console would write.
        for f in sorted(os.listdir(save_dir)):
            res.items['card:' + f] = file_hash(os.path.join(save_dir, f))
    if spec['kind'] == 'audio':
        if os.path.exists(wav) and os.path.getsize(wav) >= 44 + WAV_BYTES:
            with open(wav, 'rb') as f:
                f.seek(44)
                res.items['wav%d' % WAV_BYTES] = short_hash(f.read(WAV_BYTES))
        elif not res.error:
            res.error = 'the WAV holds less than %d bytes of samples' % WAV_BYTES
    if not keep and not res.error:
        # Keep the memory card for the runs that start from it.
        shutil.rmtree(shots_dir, ignore_errors=True)
        if os.path.exists(wav):
            os.remove(wav)
    return res


def load_baseline():
    base, recorded = {}, {}
    if os.path.exists(BASELINE):
        for line in open(BASELINE):
            line = line.split('#', 1)[0].split()
            if len(line) != 4:
                continue
            run, arch, item, value = line
            if item == 'recorded':
                recorded[(run, arch)] = value
            else:
                base.setdefault((run, arch), {})[item] = value
    return base, recorded


def save_baseline(base, recorded):
    lines = ['# tools/regress/regress.py baseline: RUN ARCH ITEM VALUE.',
             '# Frame and audio values are the first 16 hex digits of the SHA-256 of the',
             '# capture (the WAV: of its first %d bytes of samples); "recorded" names the' % WAV_BYTES,
             '# port commit the run was recorded from. Rewritten by --record.', '']
    for run in RUNS:
        for arch in ('32', '64'):
            if (run, arch) not in base:
                continue
            lines.append('%s %s recorded %s' % (run, arch, recorded.get((run, arch), '?')))
            for item, value in base[(run, arch)].items():
                lines.append('%s %s %s %s' % (run, arch, item, value))
    with open(BASELINE, 'w') as f:
        f.write('\n'.join(lines) + '\n')


def compare_archs(spec, a, b):
    """Differences between the 32 and 64-bit results of one run."""
    diffs = []
    for item in sorted(set(a.items) | set(b.items)):
        va, vb = a.items.get(item), b.items.get(item)
        if spec['kind'] == 'gate' and item == 'setNextStage' and va and vb:
            # The actor is a game-heap address; the heaps differ by word size.
            va = re.sub(r'actor=[^,]*,', '', va)
            vb = re.sub(r'actor=[^,]*,', '', vb)
        if va != vb:
            diffs.append(item)
    return diffs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog='\n\n'.join(__doc__.split('\n\n')[1:]))
    ap.add_argument('checks', nargs='*', help='groups or runs (default: all)')
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument('--check', action='store_true', help='compare with the baseline (default)')
    mode.add_argument('--record', action='store_true', help='record the baseline of these runs')
    ap.add_argument('--list', action='store_true', help='list the groups and runs')
    ap.add_argument('--arch', default='32,64', help='word sizes to run (default 32,64)')
    ap.add_argument('--build32', default='build/linux-32')
    ap.add_argument('--build64', default='build/linux-64')
    ap.add_argument('--ecl32', default='build-ecl')
    ap.add_argument('--ecl64', default='build-ecl64')
    ap.add_argument('--disc', help='GMSE01 disc image (default: rom/, decomp/orig/GMSE01, ../sms-english/orig/GMSE01)')
    ap.add_argument('--eclipse-disc', default=ECLIPSE_DISC)
    ap.add_argument('--work', default=os.path.join(ROOT, 'build', 'regress'),
                    help='folder for the runs (default build/regress)')
    ap.add_argument('--jobs', type=int, default=2, help='runs at a time (default 2: one per word size)')
    ap.add_argument('--keep', action='store_true', help='keep the captures and WAVs')
    args = ap.parse_args()

    if args.list:
        for g, runs in GROUPS.items():
            print('%-8s %s' % (g, ' '.join(runs)))
        print()
        for r, s in RUNS.items():
            print('%-14s %s' % (r, s['desc']))
        return 0

    runs = []
    for c in args.checks or ['all']:
        names = GROUPS.get(c, [c] if c in RUNS else None)
        if names is None:
            ap.error('unknown check %r (--list shows them)' % c)
        for n in names:
            if n not in runs:
                runs.append(n)
    # A run that starts from another run's memory card needs that run first.
    for n in list(runs):
        card = RUNS[n].get('card')
        if card and card not in runs:
            runs.insert(runs.index(n), card)
    runs.sort(key=list(RUNS).index)
    archs = [a for a in args.arch.split(',') if a]
    if any(a not in ('32', '64') for a in archs):
        ap.error('--arch takes 32, 64 or 32,64')

    exes = {}
    for game, a32, a64 in (('vanilla', args.build32, args.build64), ('eclipse', args.ecl32, args.ecl64)):
        if not any(RUNS[n]['game'] == game for n in runs):
            continue
        for arch, d in (('32', a32), ('64', a64)):
            if arch not in archs:
                continue
            exe = os.path.join(ROOT, d, 'sms') if not os.path.isabs(d) else os.path.join(d, 'sms')
            if not os.path.isfile(exe):
                sys.exit('regress: %s not found (build it first, or pass --%s%s)' %
                         (exe, 'build' if game == 'vanilla' else 'ecl', arch))
            exes[(game, arch)] = exe
    discs = {}
    if any(RUNS[n]['game'] == 'vanilla' for n in runs):
        discs['vanilla'] = args.disc or find_disc()
        if not discs['vanilla'] or not os.path.isfile(discs['vanilla']):
            sys.exit('regress: no GMSE01 disc image found; pass --disc')
    if any(RUNS[n]['game'] == 'eclipse' for n in runs):
        discs['eclipse'] = args.eclipse_disc
        if not os.path.isfile(discs['eclipse']):
            sys.exit('regress: %s not found (python3 tools/mods/get.py eclipse)' % discs['eclipse'])
    if any(RUNS[n]['kind'] == 'gate' or RUNS[n].get('gdb') for n in runs) and not shutil.which('gdb'):
        sys.exit('regress: the fps60 check and the Eclipse character runs need gdb')

    commit = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--short', 'HEAD'], capture_output=True,
                            text=True).stdout.strip() or '?'
    dirty = subprocess.run(['git', '-C', ROOT, 'status', '--porcelain', '--untracked-files=no'],
                           capture_output=True, text=True).stdout.strip()
    work = os.path.abspath(args.work)
    os.makedirs(work, exist_ok=True)
    say('regress: %s at %s%s, runs: %s' % ('recording' if args.record else 'checking', commit,
                                            ' (with uncommitted changes)' if dirty else '', ' '.join(runs)))
    for (game, arch), exe in sorted(exes.items()):
        say('  %s %s-bit: %s (image %s)' % (game, arch, os.path.relpath(exe, ROOT), image_hash(exe)))

    base, recorded = load_baseline()
    results = {}
    failures = []

    def job(name, arch):
        spec = RUNS[name]
        r = do_run(name, arch, exes[(spec['game'], arch)], discs[spec['game']], work, args.keep)
        results[(name, arch)] = r
        status = 'error: ' + r.error if r.error else '%d values' % len(r.items)
        say('  ran %s %s-bit in %.0f s: %s' % (name, arch, r.seconds, status))
        return r

    # Runs go in order; the word sizes of one run go side by side. A run whose
    # memory card comes from an earlier run waits for it (same order).
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for name in runs:
            list(pool.map(lambda a: job(name, a), archs))

    say('')
    for name in runs:
        spec = RUNS[name]
        rs = {a: results[(name, a)] for a in archs}
        for a in archs:
            r = rs[a]
            label = '%s %s-bit' % (name, a)
            if r.error:
                failures.append(label)
                say('FAIL %s: %s' % (label, r.error))
                continue
            if spec['kind'] == 'gate' and spec['fps'] == 60:
                m = re.match(r'frame=(\d+)$', r.items['warpIn'])
                if m.group(1) != '100':
                    failures.append(label)
                    say('FAIL %s: Mario captured at frame %s, not 100' % (label, m.group(1)))
            other = results.get(('gate30', a)) if name == 'gate60' else None
            if other and not other.error:
                # The same stage and actor, at the same moment: 60 fps frames
                # are half as long.
                ns60 = re.sub(r',frames=\d+$', '', r.items['setNextStage'])
                ns30 = re.sub(r',frames=\d+$', '', other.items['setNextStage'])
                f60 = int(r.items['warpIn'].split('=')[1])
                f30 = int(other.items['warpIn'].split('=')[1])
                if ns60 != ns30 or f60 != 2 * f30:
                    failures.append(label + ' vs 30 fps')
                    say('FAIL %s: setNextStage %s after capture at frame %d; at 30 fps %s at frame %d' % (
                        label, ns60, f60, ns30, f30))
                else:
                    say('PASS %s vs 30 fps: same setNextStage (%s), captured at frame %d (30 fps: %d)' % (
                        label, ns60, f60, f30))
            want = base.get((name, a))
            if args.record:
                if want is not None:
                    say('     %s: %s the baseline (%s)' % (label, 'same as' if want == r.items else 'replaces',
                                                         recorded.get((name, a), '?')))
                continue
            if want is None:
                failures.append(label)
                say('FAIL %s: no baseline (record one with --record)' % label)
                continue
            bad = [i for i in sorted(set(want) | set(r.items)) if want.get(i) != r.items.get(i)]
            if bad:
                failures.append(label)
                say('FAIL %s: differs from the baseline (%s) in %s' % (
                    label, recorded.get((name, a), '?'),
                    ', '.join('%s %s (baseline %s)' % (i, r.items.get(i), want.get(i)) for i in bad)))
            else:
                say('PASS %s: %d values match the baseline (%s)%s' % (
                    label, len(want), recorded.get((name, a), '?'),
                    ', ' + r.items['warpIn'].replace('frame=', 'captured at frame ') + ', ' + r.items['setNextStage']
                    if spec['kind'] == 'gate' else ''))
        if '32' in rs and '64' in rs and not rs['32'].error and not rs['64'].error:
            diffs = compare_archs(spec, rs['32'], rs['64'])
            if diffs:
                failures.append('%s 32 vs 64' % name)
                say('FAIL %s 32 vs 64-bit: differ in %s' % (name, ', '.join(diffs)))
            else:
                say('PASS %s 32 vs 64-bit: identical (%d values%s)' % (
                    name, len(rs['32'].items), ', but for the actor address' if spec['kind'] == 'gate' else ''))

    if args.record:
        if failures:
            say('\nregress: not recording: %d failure(s)' % len(failures))
            return 1
        for name in runs:
            for a in archs:
                base[(name, a)] = dict(results[(name, a)].items)
                recorded[(name, a)] = commit + ('+' if dirty else '')
        save_baseline(base, recorded)
        say('\nregress: recorded %d run(s) in %s' % (len(runs) * len(archs), os.path.relpath(BASELINE, ROOT)))
        return 0
    say('\nregress: %s' % ('all PASS' if not failures else '%d FAIL: %s' % (len(failures), ', '.join(failures))))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
