#!/usr/bin/env python3
"""gen.py OUTDIR: writes OUTDIR/driver.s (the PowerPC side of check.sh) and
OUTDIR/table.h (the host side), from the list of test cases below.

A record (big-endian) is: u32 case, u32 aux, NIN floats in[]; the result is
NOUT words out[] (the output buffer, filled with 0x13579bdf first) and a
return word (r3, or f1 stored as a float). Arguments: O is the output
buffer, In a pointer to in[n], fn the float in[n], aux the aux word, Hn a
pointer to the s16 in the high half of in[n], A a pointer to the aux word
(two big-endian u16 indices), Nk the count k, U a pointer to the DOL's
PSMulUnit01 (0, -1); copy (a, b) copies in[a..b-1] to the output buffer
first (in-place cases). A sixth field, when present, is the host's call
(C, with in, out, aux and ret) where the port's function differs from the
DOL's in name or arguments. WeightEnvelope drives the DOL's whole
J3DModel::calcWeightEnvelopeMtx over a model built in the driver."""
import os, re, sys

CASES = [
    # name, function, return, args, copy
    ('Identity', 'PSMTXIdentity', None, ['O'], None),
    ('Copy', 'PSMTXCopy', None, ['I0', 'O'], None),
    ('Concat', 'PSMTXConcat', None, ['I0', 'I12', 'O'], None),
    ('Concat_ab=a', 'PSMTXConcat', None, ['O', 'I12', 'O'], (0, 12)),
    ('Concat_ab=b', 'PSMTXConcat', None, ['I0', 'O', 'O'], (12, 24)),
    ('Inverse', 'PSMTXInverse', 'u', ['I0', 'O'], None),
    ('Inverse_inplace', 'PSMTXInverse', 'u', ['O', 'O'], (0, 12)),
    ('RotRad', 'PSMTXRotRad', None, ['O', 'aux', 'f0'], None),
    ('RotTrig', 'PSMTXRotTrig', None, ['O', 'aux', 'f0', 'f1'], None),
    ('RotAxisRad', 'PSMTXRotAxisRad', None, ['O', 'I0', 'f3'], None),
    ('Trans', 'PSMTXTrans', None, ['O', 'f0', 'f1', 'f2'], None),
    ('TransApply', 'PSMTXTransApply', None, ['I0', 'O', 'f12', 'f13', 'f14'], None),
    ('TransApply_inplace', 'PSMTXTransApply', None, ['O', 'O', 'f12', 'f13', 'f14'], (0, 12)),
    ('Scale', 'PSMTXScale', None, ['O', 'f0', 'f1', 'f2'], None),
    ('ScaleApply', 'PSMTXScaleApply', None, ['I0', 'O', 'f12', 'f13', 'f14'], None),
    ('ScaleApply_inplace', 'PSMTXScaleApply', None, ['O', 'O', 'f12', 'f13', 'f14'], (0, 12)),
    ('Quat', 'PSMTXQuat', None, ['O', 'I0'], None),
    ('LookAt', 'C_MTXLookAt', None, ['O', 'I0', 'I3', 'I6'], None),
    ('LightFrustum', 'C_MTXLightFrustum', None, ['O'] + ['f%d' % i for i in range(9)], None),
    ('LightPerspective', 'C_MTXLightPerspective', None, ['O'] + ['f%d' % i for i in range(6)], None),
    ('LightOrtho', 'C_MTXLightOrtho', None, ['O'] + ['f%d' % i for i in range(8)], None),
    ('Perspective', 'C_MTXPerspective', None, ['O'] + ['f%d' % i for i in range(4)], None),
    ('Ortho', 'C_MTXOrtho', None, ['O'] + ['f%d' % i for i in range(6)], None),
    ('Frustum*', 'C_MTXFrustum', None, ['O'] + ['f%d' % i for i in range(6)], None),
    ('MultVec', 'PSMTXMultVec', None, ['I0', 'I12', 'O'], None),
    ('MultVec_inplace', 'PSMTXMultVec', None, ['I0', 'O', 'O'], (12, 15)),
    ('MultVecArray', 'PSMTXMultVecArray', None, ['I0', 'I12', 'O', 'aux'], None),
    ('MultVecArray_inplace', 'PSMTXMultVecArray', None, ['I0', 'O', 'O', 'aux'], (12, 27)),
    ('MultVecSR', 'PSMTXMultVecSR', None, ['I0', 'I12', 'O'], None),
    ('VECAdd', 'PSVECAdd', None, ['I0', 'I3', 'O'], None),
    ('VECSubtract', 'PSVECSubtract', None, ['I0', 'I3', 'O'], None),
    ('VECScale', 'PSVECScale', None, ['I0', 'O', 'f3'], None),
    ('VECNormalize', 'PSVECNormalize', None, ['I0', 'O'], None),
    ('VECNormalize_inplace', 'PSVECNormalize', None, ['O', 'O'], (0, 3)),
    ('VECMag', 'PSVECMag', 'f', ['I0'], None),
    ('VECDotProduct', 'PSVECDotProduct', 'f', ['I0', 'I3'], None),
    ('VECCrossProduct', 'PSVECCrossProduct', None, ['I0', 'I3', 'O'], None),
    ('VECCrossProduct_c=a', 'PSVECCrossProduct', None, ['O', 'I3', 'O'], (0, 3)),
    ('VECSquareDistance', 'PSVECSquareDistance', 'f', ['I0', 'I3'], None),
    ('VECDistance', 'PSVECDistance', 'f', ['I0', 'I3'], None),
    # JSystem's and the game's paired-single routines (platform/mtx/jsys_ps.inc)
    ('J3DInvTranspose', 'J3DPSCalcInverseTranspose__FPA4_fPA3_f', 'u', ['I0', 'O'], None,
     '*ret = port_J3DPSCalcInverseTranspose((void*)in, (void*)out);'),
    ('J3DProjConcat', 'J3DMtxProjConcat__FPA4_fPA4_fPA4_f', None, ['I0', 'I12', 'O'], None,
     'port_J3DMtxProjConcat((void*)in, (void*)(in + 12), (void*)out);'),
    ('J3DProjConcat_ab=a', 'J3DMtxProjConcat__FPA4_fPA4_fPA4_f', None, ['O', 'I12', 'O'], (0, 12),
     'port_J3DMtxProjConcat((void*)out, (void*)(in + 12), (void*)out);'),
    ('J3DConcatIndexed1', 'J3DMTXConcatArrayIndexedSrc__FPA4_CfPA3_A4_CfPCUsPA3_A4_fUl', None,
     ['I0', 'I12', 'A', 'O', 'N1'], None,
     '{ unsigned short ix[2] = { aux >> 16, aux & 0xffff }; port_J3DMTXConcatArrayIndexedSrc((void*)in, (void*)(in + 12), ix, (void*)out, 1); }'),
    ('J3DConcatIndexed2', 'J3DMTXConcatArrayIndexedSrc__FPA4_CfPA3_A4_CfPCUsPA3_A4_fUl', None,
     ['I0', 'I12', 'A', 'O', 'N2'], None,
     '{ unsigned short ix[2] = { aux >> 16, aux & 0xffff }; port_J3DMTXConcatArrayIndexedSrc((void*)in, (void*)(in + 12), ix, (void*)out, 2); }'),
    ('J3DArrayConcat1', 'J3DPSMtxArrayConcat__FPA4_fPA4_fPA4_fUl', None, ['I0', 'I12', 'O', 'N1'], None,
     'port_J3DPSMtxArrayConcat((void*)in, (void*)(in + 12), (void*)out, 1);'),
    ('J3DArrayConcat2', 'J3DPSMtxArrayConcat__FPA4_fPA4_fPA4_fUl', None, ['I0', 'I12', 'O', 'N2'], None,
     'port_J3DPSMtxArrayConcat((void*)in, (void*)(in + 12), (void*)out, 2);'),
    ('J3DMulMtxVec', 'dol_J3DPSMulMtxVec', None, ['I0', 'I12', 'O'], None,
     'port_J3DPSMulMtxVec((void*)in, (void*)(in + 12), out);'),
    ('J3DMulMtxVec33', 'dol_J3DPSMulMtxVec33', None, ['I0', 'I9', 'O', 'U'], None,
     'port_J3DPSMulMtxVec33((void*)in, (void*)(in + 9), out);'),
    ('J3DWeightEnvelope', 'calcWeightEnvelopeMtx__8J3DModelFv', None, [], None,
     '{ float acc[12] = { 0 }; unsigned j = 0, n = aux >> 16, ix[2] = { aux >> 8 & 255, aux & 255 };'
     ' do port_J3DWeightEnvelopeMix((void*)acc, (void*)(in + 12 * ix[j]), (void*)(in + 24 + 12 * ix[j]), in[48 + j]);'
     ' while (++j < n); memcpy(out, acc, 48); }'),
    ('J3DHermiteS', 'J3DHermiteInterpolationS__FfPsPsPsPsPsPs', 'f', ['f0', 'H1', 'H2', 'H3', 'H4', 'H5', 'H6'], None,
     '{ float r = port_J3DHermiteInterpolationS(in[0], S16(1), S16(2), S16(3), S16(4), S16(5), S16(6)); memcpy(ret, &r, 4); }'),
    ('MsVECMag2', 'MsVECMag2__FP3Vec', 'f', ['I0'], None,
     '{ float r = port_MsVECMag2((void*)in); memcpy(ret, &r, 4); }'),
    ('MsVECNormalize', 'MsVECNormalize__FP3VecP3Vec', None, ['I0', 'O'], None, 'port_MsVECNormalize((void*)in, out);'),
    ('MsVECNormalize_inplace', 'MsVECNormalize__FP3VecP3Vec', None, ['O', 'O'], (0, 3),
     'port_MsVECNormalize(out, out);'),
]
NIN, NOUT, BATCH = 52, 24, 128
REC, RES = 8 + 4 * NIN, 4 * NOUT + 4

out = sys.argv[1]
s = []
e = s.append
e('''# Generated by tools/mtxmath/gen.py: reads records from stdin in batches, runs
# each case, writes the results to stdout.
	.section .text
	.globl _start
_start:
	lis r2, _SDA2_BASE_@ha
	addi r2, r2, _SDA2_BASE_@l
	lis r13, _SDA_BASE_@ha
	addi r13, r13, _SDA_BASE_@l
	stwu r1, -64(r1)
	lis r14, __ctors_start@ha
	addi r14, r14, __ctors_start@l
	lis r15, __ctors_end@ha
	addi r15, r15, __ctors_end@l
1:	cmplw r14, r15
	bge batch
	lwz r12, 0(r14)
	mtctr r12
	bctrl
	addi r14, r14, 4
	b 1b
batch:
	li r20, 0                    # bytes read
2:	li r0, 3
	li r3, 0
	lis r4, inbuf@ha
	addi r4, r4, inbuf@l
	add r4, r4, r20
	li r5, %d
	subf r5, r20, r5
	sc
	cmpwi r3, 0
	ble 3f
	add r20, r20, r3
	cmpwi r20, %d
	blt 2b
3:	li r21, %d
	divwu r22, r20, r21          # records in this batch
	cmpwi r22, 0
	beq done
	lis r14, inbuf@ha
	addi r14, r14, inbuf@l
	lis r15, outbuf@ha
	addi r15, r15, outbuf@l
	mr r23, r22
rec:	lis r0, 0x1357
	ori r0, r0, 0x9bdf
	li r3, %d
	mtctr r3
	addi r4, r15, -4
4:	stwu r0, 4(r4)
	bdnz 4b
	li r0, 0
	stw r0, %d(r15)
	lwz r16, 0(r14)
	lis r12, cases@ha
	addi r12, r12, cases@l
	slwi r16, r16, 2
	lwzx r12, r12, r16
	mtctr r12
	bctrl
	addi r14, r14, %d
	addi r15, r15, %d
	addic. r23, r23, -1
	bne rec
	li r0, 4                     # write the batch
	li r3, 1
	lis r4, outbuf@ha
	addi r4, r4, outbuf@l
	mulli r5, r22, %d
	sc
	cmpwi r20, %d
	beq batch
done:
	li r0, 1
	li r3, 0
	sc
''' % (REC * BATCH, REC * BATCH, REC, NOUT, 4 * NOUT, REC, RES, RES, REC * BATCH))


def lfs_fill(reg, off, base):
    e(f'\tlfs f{reg}, {off}({base})')
    e('\tlis r12, ps1_shadow@ha')
    e('\taddi r12, r12, ps1_shadow@l')
    e(f'\tstfd f{reg}, {8 * reg}(r12)')


def weight_envelope():
    # J3DModel (r3) and J3DModelData (r4), the fields calcWeightEnvelopeMtx
    # reads: one envelope of aux >> 16 mix matrices, indices (aux >> 8) & 255
    # and aux & 255 into the node matrices (in[0..23]) and inverse joint
    # matrices (in[24..47]), weights in[48..], result in the output buffer.
    e('\tlis r3, wm_model@ha')
    e('\taddi r3, r3, wm_model@l')
    e('\tlis r4, wm_data@ha')
    e('\taddi r4, r4, wm_data@l')
    e('\tstw r4, 4(r3)')
    e('\tlwz r6, 4(r14)')
    e('\tli r0, 1')
    e('\tsth r0, 132(r4)')         # getWEvlpMtxNum
    e('\tlis r5, wm_bytes@ha')
    e('\taddi r5, r5, wm_bytes@l')
    e('\tsrwi r0, r6, 16')
    e('\tstb r0, 0(r5)')           # getWEvlpMixMtxNum(0)
    e('\tstw r5, 136(r4)')
    e('\tli r0, 1')
    e('\tstb r0, 4(r5)')           # mScaleFlagArr[0..1]
    e('\tstb r0, 5(r5)')
    e('\taddi r0, r5, 4')
    e('\tstw r0, 80(r3)')
    e('\taddi r0, r5, 8')
    e('\tstw r0, 84(r3)')          # mEvlpScaleFlagArr
    e('\trlwinm r0, r6, 24, 24, 31')
    e('\tsth r0, 12(r5)')
    e('\tclrlwi r0, r6, 24')
    e('\tsth r0, 14(r5)')
    e('\taddi r0, r5, 12')
    e('\tstw r0, 140(r4)')         # getWEvlpMixMtxIndex
    e(f'\taddi r0, r14, {8 + 4 * 48}')
    e('\tstw r0, 144(r4)')         # getWEvlpMixWeight
    e(f'\taddi r0, r14, {8 + 4 * 24}')
    e('\tstw r0, 148(r4)')         # getInvJointMtx(0)
    e('\taddi r0, r14, 8')
    e('\tstw r0, 88(r3)')          # mNodeMatrices
    e('\tstw r15, 92(r3)')         # mWeightEvlpMatrices


for n, c in enumerate(CASES):
    name, fn, ret, args, copy = c[:5]
    e(f'case{n}:\t# {name}')
    e('\tmflr r0')
    e('\tstw r0, 4(r1)')
    e('\tstwu r1, -32(r1)')
    if copy:
        for k in range(copy[0], copy[1]):
            e(f'\tlwz r0, {8 + 4 * k}(r14)')
            e(f'\tstw r0, {4 * (k - copy[0])}(r15)')
    gpr, fpr = 3, 1
    if name == 'J3DWeightEnvelope':
        weight_envelope()
    for a in args:
        if a == 'A':
            e(f'\taddi r{gpr}, r14, 4')
            gpr += 1
        elif a == 'U':
            e(f'\tlis r{gpr}, psmulunit01@ha')
            e(f'\taddi r{gpr}, r{gpr}, psmulunit01@l')
            gpr += 1
        elif a[0] == 'N':
            e(f'\tli r{gpr}, {a[1:]}')
            gpr += 1
        elif a[0] == 'H':
            e(f'\taddi r{gpr}, r14, {8 + 4 * int(a[1:])}')
            gpr += 1
        elif a == 'O':
            e(f'\tmr r{gpr}, r15')
            gpr += 1
        elif a == 'aux':
            e(f'\tlwz r{gpr}, 4(r14)')
            gpr += 1
        elif a[0] == 'I':
            e(f'\taddi r{gpr}, r14, {8 + 4 * int(a[1:])}')
            gpr += 1
        elif fpr <= 8:
            lfs_fill(fpr, 8 + 4 * int(a[1:]), 'r14')
            fpr += 1
        else:  # the ninth float argument goes to the parameter area
            e(f'\tlwz r0, {8 + 4 * int(a[1:])}(r14)')
            e('\tstw r0, 8(r1)')
    e(f'\tbl {fn}')
    if ret == 'u':
        e(f'\tstw r3, {4 * NOUT}(r15)')
    elif ret == 'f':
        e(f'\tstfs f1, {4 * NOUT}(r15)')
    e('\tlwz r0, 36(r1)')
    e('\taddi r1, r1, 32')
    e('\tmtlr r0')
    e('\tblr')
e('\t.section .data')
e('\t.balign 4')
e('cases:')
for n in range(len(CASES)):
    e(f'\t.4byte case{n}')
e('\t.balign 8')
e('psmulunit01:\t.4byte 0x00000000, 0xbf800000') # J3DTransform.o .data, checked by check.sh
e('\t.section .bss')
e('\t.balign 8')
e('wm_model:\t.space 256')
e('wm_data:\t.space 256')
e('wm_bytes:\t.space 16')
e('\t.balign 8')
e(f'inbuf:\t.space {REC * BATCH}')
e(f'outbuf:\t.space {RES * BATCH}')
open(os.path.join(out, 'driver.s'), 'w').write('\n'.join(s) + '\n')

# host side
h = ['/* Generated by tools/mtxmath/gen.py */', f'#define NIN {NIN}', f'#define NOUT {NOUT}',
     '#define S16(k) ((float)(short)(fbits_u(in[k]) >> 16))',
     'static unsigned fbits_u(float f) { unsigned u; memcpy(&u, &f, 4); return u; }',
     'void port_J3DWeightEnvelopeMix(void*, void*, void*, float);',
     'float port_J3DHermiteInterpolationS(float, float, float, float, float, float, float);',
     'void port_J3DMTXConcatArrayIndexedSrc(void*, void*, unsigned short*, void*, unsigned);']
protos = set()
for c in CASES:
    name, fn, ret, args, copy = c[:5]
    if len(c) > 5:
        m = re.search(r'(port_\w+)\(', c[5])
        if m.group(1) in ('port_J3DWeightEnvelopeMix', 'port_J3DHermiteInterpolationS',
                          'port_J3DMTXConcatArrayIndexedSrc'):
            continue
        r = {'u': 'unsigned', 'f': 'float', None: 'void'}[ret]
        pa = ['unsigned' if a[0] == 'N' else 'void*' for a in args if a not in ('A', 'U')]
        protos.add(f'{r} {m.group(1)}({", ".join(pa)});')
        continue
    params = []
    for a in args:
        params.append('void*' if a in ('O',) or a[0] == 'I' else
                      ('unsigned' if fn == 'PSMTXMultVecArray' else 'char') if a == 'aux' else 'float')
    r = {'u': 'unsigned', 'f': 'float', None: 'void'}[ret]
    protos.add(f'{r} {fn}({", ".join(params)});')
h += sorted(protos)
h.append(f'#define NCASES {len(CASES)}')
h.append('static const char* case_names[] = { %s };' % ', '.join('"%s"' % c[0] for c in CASES))
h.append('static void run_case(unsigned n, unsigned aux, const float* in, float* out, unsigned* ret)')
h.append('{')
h.append('\tswitch (n) {')
for n, c in enumerate(CASES):
    name, fn, ret, args, copy = c[:5]
    h.append(f'\tcase {n}:')
    if copy:
        h.append(f'\t\tmemcpy(out, in + {copy[0]}, {4 * (copy[1] - copy[0])});')
    if len(c) > 5:
        h.append('\t\t' + c[5])
        h.append('\t\tbreak;')
        continue
    cargs = []
    for a in args:
        cargs.append('out' if a == 'O' else 'aux' if a == 'aux' else f'(void*)(in + {a[1:]})' if a[0] == 'I'
                     else f'in[{a[1:]}]')
    call = f'{fn}({", ".join(cargs)})'
    if ret == 'u':
        h.append(f'\t\t*ret = {call};')
    elif ret == 'f':
        h.append(f'\t\t{{ float r = {call}; memcpy(ret, &r, 4); }}')
    else:
        h.append(f'\t\t{call};')
    h.append('\t\tbreak;')
h.append('\t}')
h.append('}')
open(os.path.join(out, 'table.h'), 'w').write('\n'.join(h) + '\n')
