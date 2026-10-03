#!/usr/bin/env python3
"""Run every decomp TU from build/linux-$SMS_ARCH/compile_commands.json (SMS_ARCH
default 32, or SMS_BUILD=dir) with -fsyntax-only (2 jobs, nice 19) and summarise
errors. Usage: syntax_check.py [substring...]"""
import json, subprocess, sys, os, re, collections
from concurrent.futures import ThreadPoolExecutor
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
bdir = os.path.join(root, os.environ.get('SMS_BUILD', 'build/linux-' + os.environ.get('SMS_ARCH', '32')))
cc = json.load(open(os.path.join(bdir, 'compile_commands.json')))
cc = [e for e in cc if re.search(r'/(decomp|patched)/(src|libs/[^/]+/src)/', e['file'])]
if len(sys.argv) > 1:
    cc = [e for e in cc if any(a in e['file'] for a in sys.argv[1:])]
def run(e):
    cmd = e['command']
    cmd = re.sub(r'\s-o\s+\S+', ' ', cmd) + ' -fsyntax-only -fmax-errors=50 ' + os.environ.get('EXTRA', '')
    r = subprocess.run('nice -n 19 ' + cmd, shell=True, cwd=e['directory'], capture_output=True, text=True)
    return e['file'], r.returncode, r.stderr
fails = []
errs = collections.Counter()
with ThreadPoolExecutor(2) as ex:
    for f, rc, err in ex.map(run, cc):
        if rc:
            fails.append(f)
            for m in re.finditer(r'^(\S+?):(\d+):\d+: error: (.*)$', err, re.M):
                errs[(m.group(1).replace(root + '/', ''), m.group(2), m.group(3)[:140])] += 1
            open(os.path.join(bdir, 'syntax_errors.log'), 'a').write('=== %s\n%s' % (f, err))
print('%d / %d TUs fail' % (len(fails), len(cc)))
for (f, l, m), n in errs.most_common(80):
    print('%4d %s:%s: %s' % (n, f, l, m))
