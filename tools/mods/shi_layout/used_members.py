#!/usr/bin/env python3
"""Which SunshineHeaderInterface data members the mods use, by declaring class.

used_members.py SHI_INCLUDE MOD_ROOT... > used_all.json   (cwd: the work directory, with shi32.json)

Every data member is renamed in a copy of the headers and every mod source is compiled
against it: each "no member named X in C" names a use. A second pass renames only the
padding-style members (_XX), since a failed access hides the accesses chained after it.
Unqualified uses of inherited members inside mod classes do not show up this way: list
them in used_extra.txt next to this script (Class.member, one per line).
"""
import glob, json, os, re, subprocess, sys, tempfile, concurrent.futures as cf

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import shiparse

SHI = sys.argv[1]; ROOTS = sys.argv[2:]
S32 = json.load(open('shi32.json'))['types']
PAD = re.compile(r'^(_+[0-9A-Fa-f]+|_+unk\w*|_+pad\w*|unk[0-9A-Fa-f]+|padding\d*|pad\d*|_[A-Z0-9_]+)$')
PORT_MODS = os.path.join(HERE, '../../../platform/mods')


def renamed_copy(out, only_pads):
    import shutil
    shutil.copytree(SHI, out)
    for f in glob.glob(out + '/**/*.h*', recursive=True):
        raw, s, cl = shiparse.classes(f)
        E = set()
        for c in cl:
            if c['template']: continue
            for a, b, t, k in shiparse.statements(s, c['body']):
                if k != 'data': continue
                for nm in shiparse.declnames(t):
                    if only_pads and not PAD.match(nm): continue
                    m = None
                    for m in re.finditer(r'\b%s\b' % re.escape(nm), raw[a:b]): pass
                    if m: E.add((a + m.start(), a + m.end(), nm + '__pcx'))
        for x, y, t in sorted(E, reverse=True): raw = raw[:x] + t + raw[y:]
        if E: open(f, 'w', errors='surrogateescape').write(raw)


def compile_all(inc):
    X = inc
    shi = ['-I' + X + d for d in ('', '/JSystem', '/Dolphin', '/SMS', '/Kamek', '/Kuribo')]
    srcs = []
    for r in ROOTS:
        srcs += [f for f in glob.glob(r + '/src/**/*.c*', recursive=True) if '/libs/dolphin' not in f and '/cstd/' not in f]
    def one(f):
        root = [r for r in ROOTS if f.startswith(r + '/')][0]
        bse = os.path.join(os.path.dirname(root), 'bse')
        inc = ['-I' + root + '/include', '-I' + bse + '/include', '-I' + bse + '/include/BetterSMS', '-I' + bse + '/src']
        cmd = ['clang++', '-m32', '-malign-double', '-std=gnu++20' if not f.endswith('.c') else '-std=gnu11', '-fsyntax-only',
               '-ferror-limit=0', '-w', '-fms-extensions', '-I' + PORT_MODS + '/eclipse/shim', '-I' + PORT_MODS + '/include'] + inc + shi + [
               '-DNTSCU', '-DKURIBO_NO_TYPES', '-include', 'sms_mod_prelude.h', f]
        if f.endswith('.c'): cmd.insert(1, '-xc')
        return subprocess.run(cmd, capture_output=True, text=True).stderr
    with cf.ThreadPoolExecutor(os.cpu_count()) as ex:
        return ''.join(ex.map(one, srcs))


def modbases():
    out = {}
    for r in ROOTS:
        for f in glob.glob(r + '/include/**/*.h*', recursive=True) + glob.glob(r + '/src/**/*.[ch]*', recursive=True):
            for m in re.finditer(r'\b(?:class|struct)\s+(\w+)\s*(?:final\s*)?:\s*((?:public|private|protected)?\s*[\w:<>, *]+?)\s*\{',
                                 open(f, errors='replace').read()):
                out[m.group(1)] = [re.sub(r'^(public|private|protected)\s+', '', b.strip()) for b in re.split(r',(?![^<]*>)', m.group(2))]
    return out


def owner(c, n, mb):
    t = S32.get(c)
    if not t:
        for b in mb.get(c, []):
            o = owner(b, n, mb)
            if o: return o
        return None
    for f in t['fields']:
        if f['name'] == n and not f['base']: return c
    for f in t['fields']:
        if f['base'] and f.get('rec'):
            o = owner(f['rec'], n, mb)
            if o: return o
    return None


def main():
    mb = modbases(); pairs = set()
    for pads in (False, True):
        with tempfile.TemporaryDirectory() as tmp:
            renamed_copy(tmp + '/include', pads)
            txt = compile_all(tmp + '/include')
        for n, c in re.findall(r"no member named '([^']*)' in '([^']*)'", txt): pairs.add((c, n))
        for n, c in re.findall(r"field designator '([^']*)' does not refer to any field in type '([^']*)'", txt): pairs.add((c, n))
    extra = os.path.join(HERE, 'used_extra.txt')
    if os.path.exists(extra):
        for l in open(extra):
            l = l.split('#')[0].strip()
            if l: pairs.add(tuple(l.split('.', 1)))
    used = set()
    for c, n in pairs:
        o = owner(c, n, mb)
        if o: used.add((o, n))
    json.dump(sorted(used), sys.stdout)
    print('used_members: %d members' % len(used), file=sys.stderr)


main()
