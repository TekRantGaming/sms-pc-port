#!/usr/bin/env python3
"""Counts fused multiply-adds per function: the DOL against a host build.

tools/fmacount/fmacount.py [options]

For every decomp unit the port compiles, it counts the scalar fused
multiply-adds (fmadd, fmsub, fnmadd, fnmsub, single and double; not the
paired-single ps_madd family) in each function of the DOL's own object (the
decomp's split of the original, <decomp build>/GMSE01/obj), and the fused
multiply-adds that a host compiler emits for the same function when it
compiles the port's source of the unit (the command in compile_commands.json,
with the compiler and -ffp-contract replaced): FMA instructions (vfmadd...ss/sd,
with -mfma), or calls to the functions named by --call (a software fma).

Only functions that the decomp's report.json gives as matching (fuzzy match
100) are compared, so the DOL's instructions are those of the source that is
compiled. Functions are compared by demangled qualified name (overloads summed).

Output: a TSV of unit, function, DOL count, host count (--tsv), and a summary:
functions with any fused site on either side, how many agree exactly, and
site-level agreement (sum of min / sum of max over those functions).
"""
import argparse, collections, concurrent.futures, json, os, re, shlex, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

PPC_FMA = re.compile(r'^f(n?)m(add|sub)s?\.?$')
X86_FMA = re.compile(r'^vfn?m(add|sub)(132|213|231)?[sp][sd]$')


def disasm_counts(objdump, obj, ppc, calls=()):
    out = subprocess.run([objdump, '-d', '-r', '--no-show-raw-insn', obj],
                         capture_output=True, text=True).stdout
    res = collections.OrderedDict()
    cur = None
    skip = []  # (function, instruction index) of MSL sqrtf Newton steps
    if ppc:
        ins = []
        for line in out.splitlines():
            m = re.match(r'^[0-9a-f]+ <(.*)>:$', line)
            parts = line.split('\t')
            if m:
                ins.append(('<', m.group(1)))
            elif len(parts) >= 2 and parts[1].split():
                ins.append(('i', parts[1].split()[0]))
        # MSL's std::sqrtf, expanded inline: frsqrte and three fnmsub steps in
        # double. The port calls sms_msl_sqrtf there (item 15), so these
        # three are not contraction sites of the game's source.
        for i, (k, op) in enumerate(ins):
            if k == 'i' and op == 'frsqrte':
                win = [j for j in range(i + 1, min(len(ins), i + 24)) if ins[j] == ('i', 'fnmsub')]
                if len(win) == 3:
                    skip += win
    skip = set(skip)
    idx = -1
    for line in out.splitlines():
        parts = line.split('\t')
        if ppc and (re.match(r'^[0-9a-f]+ <(.*)>:$', line) or (len(parts) >= 2 and parts[1].split())):
            idx += 1
            if idx in skip:
                continue
        m = re.match(r'^[0-9a-f]+ <(.*)>:$', line)
        if m:
            cur = m.group(1)
            res.setdefault(cur, 0)
            continue
        if cur is None:
            continue
        if calls and 'R_' in line and any(re.search(r'\b%s\b' % re.escape(c), line) for c in calls):
            res[cur] += 1
            continue
        parts = line.split('\t')
        if len(parts) < 2 or not parts[1].split():
            continue
        ins = parts[1].split()[0]
        if (PPC_FMA if ppc else X86_FMA).match(ins):
            res[cur] += 1
    return res


def qualname(demangled):
    """Qualified name without the parameter list (templates kept)."""
    s = demangled
    depth = 0
    for i, ch in enumerate(s):
        if ch == '<':
            depth += 1
        elif ch == '>':
            depth -= 1
        elif ch == '(' and depth == 0 and not s.startswith('operator()', max(0, i - 8)):
            s = s[:i]
            break
    # Itanium demangling puts a template function's return type first
    depth, cut = 0, 0
    for i, ch in enumerate(s):
        if ch == '<':
            depth += 1
        elif ch == '>':
            depth -= 1
        elif ch == ' ' and depth == 0 and not s[:i].endswith('operator'):
            cut = i + 1
    return s[cut:].replace(' ', '')


def demangle_host(names):
    out = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True).stdout.split('\n')
    return dict(zip(names, out))


def host_compile(entry, cxx, flags, outdir):
    args = shlex.split(entry['command'])
    src = entry['file']
    new = [cxx]
    skip = False
    for a in args[1:]:
        if skip:
            skip = False
            continue
        if a == '-o':
            skip = True
            continue
        if a in ('-c', src) or a.startswith('-ffp-contract') or a == '-g':
            continue
        if 'clang' in cxx and (a.startswith(('-fexec-charset', '-finput-charset')) or a == '-fno-unreachable-traps'):
            continue
        new.append(a)
    obj = os.path.join(outdir, re.sub(r'[^A-Za-z0-9_.]', '_', src) + '.o')
    if 'clang' in cxx:
        new += ['-fms-extensions', '-Wno-everything']  # as the macOS build does
    new += flags + ['-c', src, '-o', obj]
    r = subprocess.run(new, cwd=entry['directory'], capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr[-2000:]
    return obj, None


_dm_cache = {}


def demangle_cw(dtk, names):
    out = {}
    for n in names:
        if n not in _dm_cache:
            r = subprocess.run([dtk, 'demangle', n], capture_output=True, text=True)
            _dm_cache[n] = r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else n
        out[n] = _dm_cache[n]
    return out


def mwcc_compile(dsrc, ninja, target, extra, outdir):
    cmds = subprocess.run([ninja, '-C', dsrc, '-t', 'commands', target], capture_output=True, text=True).stdout
    if not cmds.strip():
        return None, 'no ninja command for ' + target
    cmd = cmds.strip().splitlines()[-1].split(' && ')[0]
    args = shlex.split(cmd)
    i = args.index('-o')
    obj = os.path.join(outdir, re.sub(r'[^A-Za-z0-9_.]', '_', target) + '.mw.o')
    args[i + 1] = obj
    args = [a for a in args if a != '-MMD'] + shlex.split(extra)
    r = subprocess.run(args, cwd=dsrc, capture_output=True, text=True)
    if r.returncode or not os.path.exists(obj):
        return None, (r.stdout + r.stderr)[-2000:]
    return obj, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--decomp-build', default=os.environ.get('SMS_DECOMP_BUILD', os.path.join(ROOT, 'decomp', 'build')))
    ap.add_argument('--version', default='GMSE01')
    ap.add_argument('--compile-commands', default=os.path.join(ROOT, 'build', 'linux-64', 'compile_commands.json'))
    ap.add_argument('--cxx', default='clang++', help='host C++ compiler (C units use --cc)')
    ap.add_argument('--cc', default=None)
    ap.add_argument('--flags', default='-ffp-contract=on -mfma -fno-vectorize -fno-slp-vectorize',
                    help='flags added to the unit\'s own (its -ffp-contract and -g are dropped)')
    ap.add_argument('--call', action='append', default=[], help='count calls to this symbol as fused sites')
    ap.add_argument('--exclude', default='', help='file of unit source paths to leave out')
    ap.add_argument('--only', default='', help='regex on source path')
    ap.add_argument('--mwcc', default='',
                    help='instead of the DOL\'s objects, recompile each fully matching unit with the decomp\'s own '
                         'MWCC command and these flags appended (e.g. "-inline off", to compare the contraction '
                         'rule without either compiler\'s inlining; pair it with -fno-inline in --flags)')
    ap.add_argument('--jobs', type=int, default=3)
    ap.add_argument('--tsv', default='')
    args = ap.parse_args()
    cc = args.cc or ('clang' if 'clang' in args.cxx else 'gcc')
    dbuild = os.path.join(args.decomp_build, args.version)
    objdump_ppc = os.path.join(args.decomp_build, 'binutils', 'powerpc-eabi-objdump')
    dtk = os.path.join(args.decomp_build, 'tools', 'dtk')
    dsrc = os.path.dirname(os.path.abspath(args.decomp_build))
    ninja = os.path.join(args.decomp_build, 'venv', 'bin', 'ninja')
    if not os.path.exists(ninja):
        ninja = 'ninja'
    report = json.load(open(os.path.join(dbuild, 'report.json')))
    units = {}
    for u in report['units']:
        sp = u['metadata'].get('source_path')
        if sp:
            units[sp] = u
    excl = set(open(args.exclude).read().split()) if args.exclude else set()
    entries = []
    for e in json.load(open(args.compile_commands)):
        f = e['file']
        m = re.search(r'/(?:patched|decomp)/((?:src|libs)/.*)$', f)
        if not m or m.group(1) not in units or m.group(1) in excl:
            continue
        if args.only and not re.search(args.only, m.group(1)):
            continue
        entries.append((m.group(1), e))
    flags = shlex.split(args.flags)
    tmp = tempfile.mkdtemp(prefix='fmacount')
    rows = []
    failed = []

    def work(item):
        sp, e = item
        u = units[sp]
        uname = u['name'].split('/', 1)[1]
        dol_obj = os.path.join(dbuild, 'obj', uname + '.o')
        if not os.path.exists(dol_obj):
            return sp, None, 'no DOL object'
        if args.mwcc:
            # MWCC's rule does not depend on whether the decomp matches, only
            # on the source both compilers see: the decomp's (a unit the port
            # patches differs where the patch changes it)
            target = 'build/%s/src/%s.o' % (args.version, uname)
            mobj, err = mwcc_compile(dsrc, ninja, target, args.mwcc, tmp)
            if mobj is None:
                return sp, None, 'mwcc: ' + (err or '')
            dol = disasm_counts(objdump_ppc, mobj, True)
            os.unlink(mobj)
        else:
            dol = disasm_counts(objdump_ppc, dol_obj, True)
        comp = cc if e['file'].endswith('.c') else args.cxx
        hobj, err = host_compile(e, comp, flags, tmp)
        if hobj is None:
            return sp, None, err
        host = disasm_counts('objdump', hobj, False, args.call)
        os.unlink(hobj)
        dmap = demangle_host(list(host))
        hq = collections.Counter()
        for k, v in host.items():
            hq[qualname(dmap[k])] += v
        dq = collections.Counter()
        present = set()
        partial = set()
        if args.mwcc:
            dm = demangle_cw(dtk, list(dol))
            for k, v in dol.items():
                q = qualname(dm[k])
                present.add(q)
                dq[q] += v
        for f in ([] if args.mwcc else u.get('functions', [])):
            name = f['metadata'].get('demangled_name', f['name'])
            q = qualname(name)
            if f.get('fuzzy_match_percent', 0) < 100:
                partial.add(q)  # an overload that does not match
                continue
            present.add(q)
            dq[q] += dol.get(f['name'], 0)
        present -= partial
        hostnames = set(qualname(dmap[k]) for k in host)
        out = []
        for q in sorted(present if not args.mwcc else present & hostnames):
            out.append((sp, q, dq[q], hq[q] if q in hostnames else -1))
        return sp, out, None

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for sp, out, err in ex.map(work, entries):
            if out is None:
                failed.append((sp, err))
            else:
                rows += out
    os.rmdir(tmp)
    if args.tsv:
        with open(args.tsv, 'w') as fh:
            for r in rows:
                fh.write('\t'.join(map(str, r)) + '\n')
    summarize(rows, failed)


def summarize(rows, failed=()):
    have = [r for r in rows if r[3] >= 0]
    fused = [r for r in have if r[2] or r[3]]
    agree = sum(1 for r in fused if r[2] == r[3])
    smin = sum(min(r[2], r[3]) for r in fused)
    smax = sum(max(r[2], r[3]) for r in fused)
    dsum = sum(r[2] for r in have)
    hsum = sum(r[3] for r in have)
    inl = [r for r in rows if r[3] < 0]
    print('units %d (failed %d), matched functions %d (%d not emitted by the host: inlined everywhere, %d DOL sites there)'
          % (len(set(r[0] for r in rows)), len(failed), len(rows), len(inl), sum(r[2] for r in inl)))
    print('functions with a fused site on either side: %d; equal counts: %d (%.1f%%)'
          % (len(fused), agree, 100.0 * agree / max(1, len(fused))))
    print('sites: DOL %d, host %d; sum(min)/sum(max) = %d/%d = %.1f%%'
          % (dsum, hsum, smin, smax, 100.0 * smin / max(1, smax)))
    for sp, err in failed[:5]:
        print('FAILED', sp, (err or '').strip().splitlines()[-1:] if err else '')


if __name__ == '__main__':
    main()
