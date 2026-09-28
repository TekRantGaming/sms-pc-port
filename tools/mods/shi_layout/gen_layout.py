"""Rewrite SunshineHeaderInterface class bodies so every member a mod may use sits
where the port keeps the retail member at the same retail offset, on 32- and 64-bit hosts.

gen_layout.py SHI_INCLUDE OUT_INCLUDE
Inputs (cwd): shi32.json shi64.json (SHI as the mods compile it), port32.json port64.json
(the port's layouts), vlate.json (classes whose retail vptr follows their first members),
used_all.json (members the mods use, by declaring class)."""
import sys, json, re, glob, os, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shiparse, retail

W = os.environ.get('SHI_LAYOUT_WORK', '.')


def load(name): return json.load(open(os.path.join(W, name)))


S = {32: load('shi32.json')['types'], 64: load('shi64.json')['types']}
P = {32: load('port32.json')['types'], 64: load('port64.json')['types']}
R32, _ = retail.build(P[32], load('vlate.json'))
# SHI as the mods saw it on the GameCube: Kuribo's compiler places vtable pointers as
# CodeWarrior does, so SHI's offsets are retail offsets once its vptrs are moved too.
G32, _ = retail.build(S[32], load('vlate_shi.json'))
PTR = {32: 4, 64: 8}
ALIAS = {'ObjData': 'TMapObjData', 'obj_info': 'TMapObjCollisionInfo', 'map_col_info': 'TMapObjCollisionInfo',
         'map_col_data': 'TMapObjCollisionData', 'sink_data': 'TMapObjSinkData', 'sound_data': 'TMapObjSoundData',
         'sound_info': 'TMapObjSoundInfo', 'anim_data': 'TMapObjAnimData', 'hit_data': 'TMapObjHitDataTable',
         'obj_hit_info': 'TMapObjHitInfo', 'ObjPhysicalData': 'TMapObjPhysicalData', 'ObjPhysicalInfo': 'TMapObjPhysicalInfo'}
PAD = re.compile(r'^(_+[0-9A-Fa-f]+|_+unk\w*|_+pad\w*|unk[0-9A-Fa-f]+|padding\d*|pad\d*|_[A-Z0-9_]+)$')
used_pairs = set(tuple(x) for x in load('used_all.json'))
SEQ = {}    # class -> [(name, stmt text)] from the original headers
BODY = {}   # class -> original body text
LOG = []


def pn(r): return ALIAS.get(r, r)


def keyed(t):
    out = []; cnt = {}
    for g in t['fields']:
        n = g['name']; k = cnt.get(n, 0); cnt[n] = k + 1
        out.append(((n, k), g))
    return out


def tail_pad(g): return (g['name'] or '').startswith('_pcTailPad')


def tmap(T, off, D, TD=None, size=0):
    """retail offset in port record T -> offset in target layout D (P[32] or P[64])"""
    t = R32.get(T); t6 = D.get(TD or T)
    if t is None or t6 is None: return None
    if off >= t['size']:
        return t6['size'] if off == t['size'] else None
    k6 = dict(keyed(t6))
    for key, g in keyed(t):
        if g['bitsize'] or tail_pad(g): continue
        if g['off'] <= off < g['off'] + g['size']:
            h = k6.get(key)
            if h is None: return None
            rel = off - g['off']
            if rel == 0: return h['off']
            if g['dims']:
                es = g['esize']; idx = rel // es; rel2 = rel - idx * es
                b = h['off'] + idx * h['esize']
                if rel2 == 0: return b
                if 'rec' in g and 'rec' in h:
                    r = tmap(g['rec'], rel2, D, h['rec'])
                    return None if r is None else b + r
                return b + rel2 if g['esize'] == h['esize'] else None
            if 'rec' in g and 'rec' in h:
                r = tmap(g['rec'], rel, D, h['rec'])
                return None if r is None else h['off'] + r
            return h['off'] + rel if g['size'] == h['size'] else None
    if off == 0: return 0
    prev = None
    for key, g in keyed(t):
        if g['off'] + g['size'] <= off and not tail_pad(g): prev = (key, g)
    if prev:
        h = k6.get(prev[0])
        if h:
            o = h['off'] + h['size'] + (off - prev[1]['off'] - prev[1]['size'])
            # retail padding the target layout lacks: stay before the next member
            nxt = [x['off'] for x in t6['fields'] if x['off'] >= h['off'] + h['size'] and not x['bitsize']]
            if nxt and size and o + size > min(nxt):
                o = max(h['off'] + h['size'], min(nxt) - size)
            return o
    return None


def dataend(T, types):
    t = types[T]; e = 0
    for g in t['fields']:
        x = g['off'] + (dataend(g['rec'], types) if g['base'] and 'rec' in g and g['rec'] in types else g['size'])
        e = max(e, x)
    return e


def plannable(R):
    return R in S[32] and R in S[64] and R in SEQ and pn(R) in P[32] and pn(R) in P[64] and pn(R) in R32


def native_dsize(X, a):
    return dataend(X, S[a])


_memo = {}


def plan(R, a):
    """(decisions, tail, changed, dsize, size) for arch a; decisions aligned with SEQ[R]"""
    if (R, a) in _memo: return _memo[(R, a)]
    _memo[(R, a)] = None  # recursion guard
    res = _plan(R, a)
    _memo[(R, a)] = res
    return res


def rsize(X, a, dims):
    """final size of a member of SHI record type X (with array dims) in arch a"""
    if '{' in X: return None
    k = 1
    for d in dims: k *= d
    if X in S[a] and plannable(X):
        p = plan(X, a)
        if p: return p[4] * k
    return S[a][X]['size'] * k if X in S[a] else None


def _plan(R, a):
    s32 = G32[R]; sa = S[a][R]; D = P[a]; PR = pn(R)
    rows = list(zip(s32['fields'], sa['fields']))
    mem = [(f, fa) for f, fa in rows if not f['base'] and not (f['name'] or '').startswith('_vptr')]
    body = BODY[R]
    cur = PTR[a] if any((fa['name'] or '').startswith('_vptr') and fa['off'] == 0 for f, fa in rows) else 0
    synced = True
    for f, fa in rows:
        if f['base']:
            o = tmap(PR, f['off'], D)
            if o is None or 'rec' not in f: return None
            X = f['rec']
            if X in S[a] and plannable(X) and plan(X, a):
                bd = plan(X, a)[3]
            else:
                bd = native_dsize(X, a)
            if o != fa['off'] or bd != native_dsize(X, a): synced = False
            cur = max(cur, o + bd)
    out = []; changed = False
    for (n, st), (f, fa) in zip(SEQ[R], mem):
        refd = (R, n) in used_pairs or len(re.findall(r'\b%s\b' % re.escape(n), body)) > 1
        if a == 64 and PAD.match(n) and not refd:
            out.append((0, 'exclude', 0)); changed = True; synced = False; continue
        size = fa['size']
        if f.get('rec') and not f['bitsize']:
            rs = rsize(f['rec'], a, f['dims'])
            if rs is not None: size = rs
        if f['bitsize']:
            LOG.append('%s.%s: bitfield left in place (%d-bit)' % (R, n, a))
            out.append((0, 'keep', 0)); synced = False; continue
        o = tmap(PR, f['off'], D, size=size)
        if o is None or o < cur:
            why = 'no %d-bit position' % a if o is None else 'overlaps by %d (%d-bit)' % (cur - o, a)
            if refd or not PAD.match(n):
                LOG.append('%s.%s: %s, left in place' % (R, n, why))
                out.append((0, 'keep', size)); cur += size; synced = False; changed = changed or a == 64; continue
            LOG.append('%s.%s: %s, excluded' % (R, n, why))
            out.append((0, 'exclude', 0)); changed = True; synced = False; continue
        pad = o - cur
        if synced and o == fa['off']: pad = 0
        mode = 'keep'
        end = tmap(PR, f['off'] + f['size'], D)
        span = None if end is None else end - o
        if span is not None and size > span and re.match(r'^\s*(const\s+)?(size_t|size_type|ssize_t)\b', st):
            mode = 'retype'; size = 4
        if pad or mode != 'keep' or o != fa['off'] or size != fa['size']:
            changed = True
            synced = synced and pad == 0 and mode == 'keep' and o == fa['off'] and size == fa['size']
        out.append((pad, mode, size))
        cur = o + size
    # where the class's retail end lands
    if s32['size'] >= R32[PR]['size']: tgt = dataend(PR, D)
    else: tgt = tmap(PR, s32['size'], D)
    if tgt is None: tgt = cur
    tail = tgt - cur if tgt > cur else 0
    if tgt < cur: LOG.append('%s: %d-bit data end %d past port %d' % (R, a, cur, tgt))
    if tail: changed = True
    ds = cur + tail
    al = sa.get('align') or 1
    size = (ds + al - 1) // al * al
    if not changed: size = sa['size']; ds = native_dsize(R, a)
    return out, tail, changed, ds, size


def version(raw_stmt, pad, mode, idx, a):
    txt = ''
    if pad: txt += '    u8 _pc%d_%d[%d];\n' % (a, idx, pad)
    if mode == 'keep': txt += raw_stmt.strip('\n') + '\n'
    elif mode == 'retype':
        rt = re.sub(r'\b(size_t|size_type)\b', 'u32', raw_stmt, 1); rt = re.sub(r'\bssize_t\b', 's32', rt, 1)
        txt += rt.strip('\n') + '\n'
    return txt


def class_seq(s, d):
    st = [x for x in shiparse.statements(s, d['body']) if x[3] == 'data']
    seq = []
    for a_, b_, t, k in st:
        names = shiparse.declnames(t)
        if len(names) != 1: return None
        seq.append((names[0], a_, b_, t))
    return seq


def main():
    SRC, OUT = sys.argv[1], sys.argv[2]
    if os.path.exists(OUT): shutil.rmtree(OUT)
    shutil.copytree(SRC, OUT)
    files = {}; twice = set()
    for f in glob.glob(SRC + '/**/*.h*', recursive=True):
        raw, s, cl = shiparse.classes(f)
        for c in cl:
            if c['template'] or c['name'] not in S[32]: continue
            if c['name'] in files or c['name'] in twice:
                twice.add(c['name']); SEQ.pop(c['name'], None); files.pop(c['name'], None); continue
            seq = class_seq(s, c)
            mem = [g['name'] for g in S[32][c['name']]['fields'] if not g['base'] and not (g['name'] or '').startswith('_vptr')]
            if seq is None or [x[0] for x in seq] != mem:
                if mem: LOG.append('%s: declarations do not match members, skipped' % c['name'])
                continue
            SEQ[c['name']] = [(n, t) for n, _, _, t in seq]
            BODY[c['name']] = s[c['body'][0]:c['body'][1]]
            files[c['name']] = f
    done = {32: [], 64: []}
    order = sorted(SEQ, key=lambda r: (-r.count('::'), r))
    for R in order:
        if not plannable(R): continue
        plans = {a: plan(R, a) for a in (32, 64)}
        if not plans[32] or not plans[64]:
            LOG.append('%s: base without a position, skipped' % R); continue
        if not plans[32][2] and not plans[64][2]: continue
        out_file = OUT + files[R][len(SRC):]
        raw, s, cl = shiparse.classes(out_file)
        d = [c for c in cl if c['name'] == R and not c['template']][0]
        seq = class_seq(s, d)
        E = []
        for i, (n, a_, b_, t) in enumerate(seq):
            dec = {a: plans[a][0][i] for a in (32, 64)}
            ch = {a: plans[a][2] and (dec[a][0] or dec[a][1] != 'keep') for a in (32, 64)}
            if not ch[32] and not ch[64]: continue
            orig = raw[a_:b_]
            v = {a: (version(orig, dec[a][0], dec[a][1], i, a) if ch[a] else orig.strip('\n') + '\n') for a in (32, 64)}
            E.append((a_, b_, '\n#if __SIZEOF_POINTER__ == 8\n' + v[64] + '#else\n' + v[32] + '#endif\n'))
        tails = {a: plans[a][1] if plans[a][2] else 0 for a in (32, 64)}
        if tails[32] or tails[64]:
            e = d['body'][1]
            E.append((e, e, '\n#if __SIZEOF_POINTER__ == 8\n' + ('    u8 _pc64_tail[%d];\n' % tails[64] if tails[64] else '') + '#else\n' + ('    u8 _pc32_tail[%d];\n' % tails[32] if tails[32] else '') + '#endif\n'))
        for x, y, t in sorted(E, key=lambda e: (e[0], e[1]), reverse=True):
            raw = raw[:x] + t + raw[y:]
        open(out_file, 'w', errors='surrogateescape').write(raw)
        for a in (32, 64):
            if plans[a][2]: done[a].append(R)
    json.dump({'done32': done[32], 'done64': done[64], 'log': LOG}, open(os.path.join(W, 'genl.json'), 'w'), indent=1)
    print('changed: 32-bit %d classes, 64-bit %d classes' % (len(done[32]), len(done[64])))
    print('\n'.join(LOG))


if __name__ == "__main__": main()
