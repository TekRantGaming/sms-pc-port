#!/usr/bin/env python3
"""ps2scalar.py BINUTILS TAG=OBJ[:FUNC,FUNC...] ... > out.s

qemu-ppc has no Gekko paired-single instructions, so tools/mtxmath/check.sh
runs the DOL's MTX/VEC objects through this rewrite: every instruction of the
original machine code is kept, in order, and each paired-single one becomes
the same scalar operation on both halves. ps0 of a register is the real FPR;
ps1 lives in a shadow array (ps1_shadow, one double per FPR).

The Gekko semantics modelled (as Dolphin implements them):
- psq_l/psq_lu/psq_lx/psq_st/psq_stu with GQR0 (floats, no scaling); W=1
  loads 1.0 into ps1 and stores ps0 only. psq_l with W=1 through GQR5 is an
  s16 load with no scale (OSInitFastCast's setting; J3DHermiteInterpolationS),
  converted with r0, r11 and gqr_tmp as scratch.
- ps_add/sub/mul/madd/msub/nmadd/nmsub/muls0/muls1/madds0/madds1 are the
  scalar single-precision instruction (fadds, fmadds, ...) on each half;
  ps_sum0/1, ps_merge00/01/10/11, ps_neg and ps_cmpo0 as documented.
- Single-precision scalar instructions (lfs, fadds, fsubs, fmuls, fdivs,
  fmadds, fmsubs, fnmadds, fnmsubs, fres, frsp) also write their result to
  ps1; double-precision ones (lfd, fmr, fneg, fsel, frsqrte, ...) leave ps1.
The arithmetic of each operation is qemu's. f16-f20 and r12 are scratch (the
objects do not use them); TAG@rN=OBJ... uses rN instead of r12 for that
object (J3DModel::calcWeightEnvelopeMtx uses r12).

Relocations: .sdata/.sdata2 constants are copied into this file under
TAG_SYMBOL labels; calls go to the named symbol (the other rewritten
functions, or MSL's sinf/cosf/tanf linked beside)."""
import re, subprocess, sys

B = sys.argv[1]
out = []
emit = out.append
data = []


def objdump(*a):
    return subprocess.check_output([B + '/powerpc-eabi-objdump'] + list(a), text=True)


def sym(tag, name):
    return tag + '_' + re.sub(r'[^A-Za-z0-9_]', '_', name)


SINGLE_FILL = {'lfs', 'lfsx', 'fadds', 'fsubs', 'fmuls', 'fdivs', 'fmadds', 'fmsubs', 'fnmadds', 'fnmsubs',
               'fres', 'frsp'}
PASS = {'lfd', 'stfd', 'stfs', 'fmr', 'fneg', 'fabs', 'fnabs', 'fsel', 'frsqrte', 'fcmpo', 'fcmpu', 'fmul',
        'fadd', 'fsub', 'fdiv', 'fmadd', 'fmsub', 'fnmadd', 'fnmsub', 'fctiwz'}
# paired-single arithmetic: name -> (scalar op, operand order in the ps form)
PS3 = {'ps_add': 'fadds', 'ps_sub': 'fsubs', 'ps_mul': 'fmuls', 'ps_div': 'fdivs'}
PS4 = {'ps_madd': 'fmadds', 'ps_msub': 'fmsubs', 'ps_nmadd': 'fnmadds', 'ps_nmsub': 'fnmsubs'}


SCR = 'r12'


def base():
    emit(f'\tlis {SCR}, ps1_shadow@ha')
    emit(f'\taddi {SCR}, {SCR}, ps1_shadow@l')


def fr(x):
    return int(x[1:])


def ld1(dst, src):  # f<dst> = ps1 of f<src>
    emit(f'\tlfd f{dst}, {8 * src}({SCR})')


def st1(src, dst):  # ps1 of f<dst> = f<src>
    emit(f'\tstfd f{src}, {8 * dst}({SCR})')


def rewrite(tag, obj, funcs):
    dis = objdump('-dr', '-Mgekko', obj)
    # constants
    syms = {}
    for line in objdump('-t', obj).splitlines():
        m = re.match(r'([0-9a-f]{8}) l\s+O (\.sdata2?|\.data|\.rodata)\s+([0-9a-f]{8}) (\S+)', line)
        if m:
            syms.setdefault(m.group(2), []).append((int(m.group(1), 16), m.group(4)))
    for sec, lst in syms.items():
        raw = objdump('-s', '-j', sec, obj)
        by = bytearray()
        for line in raw.splitlines():
            m = re.match(r' ([0-9a-f]{4,}) ((?:[0-9a-f]{2,8} ){1,4})', line)
            if m:
                by += bytes.fromhex(m.group(2).replace(' ', ''))
        data.append('\t.balign 8')
        labels = dict((o, n) for o, n in lst)
        for i in range(0, len(by), 4):
            if i in labels:
                data.append(f'{sym(tag, labels[i])}:')
            data.append(f'\t.4byte 0x{by[i:i + 4].hex()}')
    # code
    relocs = {}
    cur = None
    lines = []
    for line in dis.splitlines():
        m = re.match(r'([0-9a-f]{8}) <(\S+)>:', line)
        if m:
            cur = m.group(2)
            continue
        m = re.match(r'\s+([0-9a-f]+): (R_PPC_\S+)\s+(\S+)', line)
        if m:
            relocs[int(m.group(1), 16) & ~3] = (m.group(2), m.group(3))
            continue
        m = re.match(r'\s+([0-9a-f]+):\t(?:[0-9a-f]{2} ){4}\t(\S+)\s*(.*)', line)
        if m and cur:
            lines.append((cur, int(m.group(1), 16), m.group(2), m.group(3).strip()))
    lastf = None
    for f, addr, op, args in lines:
        if funcs and f not in funcs:
            continue
        if f != lastf:
            emit(f'\t.globl {f}')
            emit(f'{f}:')
            lastf = f
        emit(f'L_{tag}_{addr:x}:')
        a = [x.strip() for x in args.split(',')] if args else []
        rel = relocs.get(addr)
        # relocated operands
        if rel:
            kind, target = rel
            if kind == 'R_PPC_EMB_SDA21' and op == 'li':  # the address itself
                s = sym(tag, target)
                emit(f'\tlis {a[0]}, {s}@ha')
                emit(f'\taddi {a[0]}, {a[0]}, {s}@l')
                continue
            if kind == 'R_PPC_EMB_SDA21':
                s = sym(tag, target)
                emit(f'\tlis {SCR}, {s}@ha')
                emit(f'\t{op} {a[0]}, {s}@l({SCR})')
                if op in SINGLE_FILL:
                    base()
                    st1(fr(a[0]), fr(a[0]))
                continue
            if kind == 'R_PPC_ADDR16_HA':
                emit(f'\t{op} {a[0]}, {sym(tag, target)}@ha')
                continue
            if kind == 'R_PPC_ADDR16_LO':
                emit(f'\t{op} {a[0]}, {a[1]}, {sym(tag, target)}@l')
                continue
            if kind == 'R_PPC_REL24':
                emit(f'\t{op} {target}')
                continue
            raise SystemExit(f'unhandled relocation {rel} at {tag}:{addr:x}')
        # branches inside the function
        m = re.match(r'([0-9a-f]+) <', args)
        if m and op.startswith('b'):
            emit(f'\t{op} L_{tag}_{m.group(1)}')
            continue
        if op == 'psq_lx':  # frD, rA, rB, W, I
            d, w, i = fr(a[0]), int(a[3]), int(a[4])
            assert i == 0 and w == 0 and a[1] != 'r0', 'psq_lx: GQR0, W=0 only'
            emit(f'\tlfsx f{d}, {a[1]}, {a[2]}')
            emit(f'\tadd {SCR}, {a[1]}, {a[2]}')
            emit(f'\tlfs f16, 4({SCR})')
            base()
            st1(16, d)
            continue
        if op == 'lfsu':  # single-precision load with update: both halves
            m = re.match(r'(-?\d+)\((r\d+)\)', a[1])
            emit(f'\taddi {m.group(2)}, {m.group(2)}, {m.group(1)}')
            emit(f'\tlfs {a[0]}, 0({m.group(2)})')
            base()
            st1(fr(a[0]), fr(a[0]))
            continue
        if op == 'psq_l' and a[3] == '5':  # s16 through GQR5 (no scale), W=1
            d = fr(a[0])
            assert a[2] == '1', 'GQR5: W=1 only'
            emit(f'\tlha r0, {a[1]}')
            emit('\txoris r0, r0, 0x8000')
            emit('\tlis r11, gqr_tmp@ha')
            emit('\taddi r11, r11, gqr_tmp@l')
            emit('\tstw r0, 4(r11)')
            emit('\tlis r0, 0x4330')
            emit('\tstw r0, 0(r11)')
            emit(f'\tlfd f{d}, 0(r11)')
            emit('\tlfd f16, 8(r11)')
            emit(f'\tfsub f{d}, f{d}, f16')
            emit(f'\tfrsp f{d}, f{d}')
            emit('\tlis r11, one@ha')
            emit('\tlfs f16, one@l(r11)')
            base()
            st1(16, d)
            continue
        if op.startswith('psq_'):
            d = fr(a[0])
            m = re.match(r'(-?\d+)\((r\d+)\)', a[1])
            off, ra = int(m.group(1)), m.group(2)
            w, i = int(a[2]), int(a[3])
            assert i == 0, 'GQR0 only'
            if op.endswith('u'):
                emit(f'\taddi {ra}, {ra}, {off}')
                off = 0
            base()
            if op.startswith('psq_l'):
                emit(f'\tlfs f{d}, {off}({ra})')
                if w:
                    emit(f'\tlis {SCR}, one@ha')
                    emit(f'\tlfs f16, one@l({SCR})')
                    base()
                else:
                    emit(f'\tlfs f16, {off + 4}({ra})')
                st1(16, d)
            else:
                emit(f'\tstfs f{d}, {off}({ra})')
                if not w:
                    ld1(16, d)
                    emit(f'\tstfs f16, {off + 4}({ra})')
            continue
        if op.startswith('ps_'):
            base()
            r = [fr(x) for x in a if x.startswith('f')]
            if op == 'ps_cmpo0':
                emit(f'\tfcmpo {a[0]}, {a[1]}, {a[2]}')
                continue
            d = r[0]
            if op in PS3:  # d = a op b (ps_mul: d = a * c)
                x, y = r[1], r[2]
                ld1(16, x), ld1(17, y)
                emit(f'\t{PS3[op]} f19, f16, f17')
                emit(f'\t{PS3[op]} f20, f{x}, f{y}')
            elif op in PS4:  # frD, frA, frC, frB
                x, c, y = r[1], r[2], r[3]
                ld1(16, x), ld1(17, c), ld1(18, y)
                emit(f'\t{PS4[op]} f19, f16, f17, f18')
                emit(f'\t{PS4[op]} f20, f{x}, f{c}, f{y}')
            elif op in ('ps_muls0', 'ps_muls1'):
                x, c = r[1], r[2]
                ld1(16, x)
                if op == 'ps_muls1':
                    ld1(17, c)
                else:
                    emit(f'\tfmr f17, f{c}')
                emit('\tfmuls f19, f16, f17')
                emit(f'\tfmuls f20, f{x}, f17')
            elif op in ('ps_madds0', 'ps_madds1'):
                x, c, y = r[1], r[2], r[3]
                ld1(16, x), ld1(18, y)
                if op == 'ps_madds1':
                    ld1(17, c)
                else:
                    emit(f'\tfmr f17, f{c}')
                emit('\tfmadds f19, f16, f17, f18')
                emit(f'\tfmadds f20, f{x}, f17, f{y}')
            elif op in ('ps_sum0', 'ps_sum1'):  # frD, frA, frC, frB
                x, c, y = r[1], r[2], r[3]
                ld1(16, y)
                if op == 'ps_sum0':  # (a0 + b1, c1)
                    emit(f'\tfadds f20, f{x}, f16')
                    ld1(19, c)
                else:  # (c0, a0 + b1)
                    emit(f'\tfadds f19, f{x}, f16')
                    emit(f'\tfmr f20, f{c}')
            elif op.startswith('ps_merge'):
                x, y = r[1], r[2]
                hi, lo = op[-2], op[-1]
                if hi == '0':
                    emit(f'\tfmr f20, f{x}')
                else:
                    ld1(20, x)
                if lo == '0':
                    emit(f'\tfmr f19, f{y}')
                else:
                    ld1(19, y)
            elif op == 'ps_neg':
                x = r[1]
                ld1(16, x)
                emit('\tfneg f19, f16')
                emit(f'\tfneg f20, f{x}')
            else:
                raise SystemExit(f'unhandled {op} at {tag}:{addr:x}')
            emit(f'\tfmr f{d}, f20')
            st1(19, d)
            continue
        emit(f'\t{op} {args}' if args else f'\t{op}')
        if op in SINGLE_FILL:
            base()
            st1(fr(a[0]), fr(a[0]))
        elif op.startswith('f') and op not in PASS or op in ('lfsu', 'lfsux', 'psq_lx', 'psq_stx'):
            raise SystemExit(f'unhandled {op} at {tag}:{addr:x}')


emit('\t.section .text')
for spec in sys.argv[2:]:
    tag, rest = spec.split('=', 1)
    tag, _, SCR = tag.partition('@')
    SCR = SCR or 'r12'
    obj, _, fl = rest.partition(':')
    rewrite(tag, obj, set(fl.split(',')) if fl else None)
emit('\t.section .data')
emit('\t.balign 8')
emit('\t.globl ps1_shadow')
emit('ps1_shadow:\t.space 256')
emit('one:\t.float 1.0')
emit('\t.balign 8')
emit('gqr_tmp:\t.4byte 0, 0, 0x43300000, 0x80000000')
out += data
print('\n'.join(out))
