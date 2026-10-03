#!/usr/bin/env python3
"""inline.py BINUTILS J3DCluster.o J3DTransform.o > inline.s

J3DTransform.hpp's J3DPSMulMtxVec is inline assembly, so the DOL has it only
inside its callers. This lifts the two copies in J3DSkinDeform::deform (the
3x4 position one and the 3x3 normal one, which are all the game uses) into
functions dol_J3DPSMulMtxVec(m, v, d) and dol_J3DPSMulMtxVec33(m, v, d, unit)
for tools/mtxmath: each is the DOL's instruction words, unchanged, after
moves into the registers MWCC gave the operands there. The 3x3 copy's
PSMulUnit01 address comes in as a fourth argument; the value the driver
passes is checked against J3DTransform.o's here."""
import re, subprocess, sys

B, cluster, transform = sys.argv[1:4]


def objdump(*a):
    return subprocess.check_output([B + '/powerpc-eabi-objdump'] + list(a), text=True)


dis = objdump('-dr', '-Mgekko', cluster)
start = dis.index('<deform__13J3DSkinDeformFP8J3DModel>:')
body = dis[start:dis.index('\n\n', start)]
ins = []  # (address, word, text)
for line in body.splitlines():
    m = re.match(r'\s+([0-9a-f]+):\t((?:[0-9a-f]{2} ){4})\t(.*)', line)
    if m:
        ins.append((int(m.group(1), 16), m.group(2).replace(' ', ''), m.group(3).strip()))
    elif 'R_PPC' in line and ins:
        ins[-1] = ins[-1][:2] + (ins[-1][2] + ' RELOC',)


def block(first, last, skip=()):
    out = [(a, w, t) for a, w, t in ins if first <= a <= last and a not in skip]
    assert all('RELOC' not in t for a, w, t in out), 'relocated instruction in the block'
    return out


# the first unrolled copy of each loop (the MWCC register choices are in the
# expectations below, which also pin the addresses to this object)
pos = block(0x10ec, 0x1138)
nrm = block(0x131c, 0x1374, skip=(0x1320,))  # 0x1320: addi r6, r6, PSMulUnit01@l
assert pos[0][2].startswith('psq_l   f0,0(r30),0,0') and pos[1][2].startswith('psq_l   f2,0(r3),0,0')
assert pos[-1][2].startswith('psq_st  f6,8(r27),1,0') and len(pos) == 20
assert nrm[0][2].startswith('psq_l   f0,0(r10),0,0') and nrm[2][2].startswith('psq_l   f13,0(r6),0,0')
assert nrm[-1][2].startswith('psq_st  f6,8(r11),1,0') and len(nrm) == 22
assert [t for a, w, t in ins if a == 0x1320][0].endswith('RELOC')

# PSMulUnit01, which gen.py's driver passes: 0.0f, -1.0f
syms = objdump('-t', transform)
m = re.search(r'([0-9a-f]{8}) g\s+O \.data\s+00000008 PSMulUnit01', syms)
off = int(m.group(1), 16)
raw = objdump('-s', '-j', '.data', transform)
data = bytearray()
for line in raw.splitlines():
    m = re.match(r' ([0-9a-f]{4,}) ((?:[0-9a-f]{2,8} ){1,4})', line)
    if m:
        data += bytes.fromhex(m.group(2).replace(' ', ''))
assert data[off:off + 8].hex() == '00000000bf800000', data[off:off + 8].hex()

print('\t.section .text')
for name, moves, words in (('dol_J3DPSMulMtxVec', ['mr r30, r4', 'mr r27, r5'], pos),
                           ('dol_J3DPSMulMtxVec33', ['mr r10, r4', 'mr r11, r5'], nrm)):
    print(f'\t.globl {name}')
    print(f'{name}:')
    for mv in moves:
        print('\t' + mv)
    for a, w, t in words:
        print(f'\t.4byte 0x{w}\t# {a:x}: {t}')
    print('\tblr')
