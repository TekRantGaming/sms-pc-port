# PowerPC side of tools/gxmath/check.sh: runs the DOL's own GX functions (the
# decomp's split of the original, lifted by tools/mslmath/lift.py) under
# qemu-ppc. Reads 128-byte records from stdin (see harness.cpp), calls the
# function and writes its results: 24 bytes (six floats) for functions 0 to 2,
# the vertex stream for GXDrawSphere (tools/gxmath/fifo.py).
	.section .text
	.globl _start
_start:
	lis r2, _SDA2_BASE_@ha
	addi r2, r2, _SDA2_BASE_@l
	lis r13, _SDA_BASE_@ha
	addi r13, r13, _SDA_BASE_@l
	stwu r1, -64(r1)
	lis r14, __ctors_start@ha	# MSL's tables have static initialisers
	addi r14, r14, __ctors_start@l
	lis r15, __ctors_end@ha
	addi r15, r15, __ctors_end@l
1:	cmplw r14, r15
	bge 2f
	lwz r12, 0(r14)
	mtctr r12
	bctrl
	addi r14, r14, 4
	b 1b
2:
loop:
	li r0, 3
	li r3, 0
	lis r4, inbuf@ha
	addi r4, r4, inbuf@l
	li r5, 128
	sc
	cmpwi r3, 128
	bne done
	lis r20, inbuf@ha
	addi r20, r20, inbuf@l
	lis r21, outbuf@ha
	addi r21, r21, outbuf@l
	lis r22, light@ha
	addi r22, r22, light@l
	li r0, 0
	stw r0, 0(r21)
	stw r0, 4(r21)
	stw r0, 8(r21)
	stw r0, 12(r21)
	stw r0, 16(r21)
	stw r0, 20(r21)
	li r5, 16
	mtctr r5
	mr r5, r22
1:	stw r0, 0(r5)
	addi r5, r5, 4
	bdnz 1b
	lwz r16, 0(r20)
	lwz r17, 4(r20)
	lfs f1, 8(r20)
	lfs f2, 12(r20)
	lfs f3, 16(r20)
	cmpwi r16, 0
	beq distattn
	cmpwi r16, 1
	beq specular
	cmpwi r16, 2
	beq project
	b sphere

distattn:			# GXInitLightDistAttn(light, f[0], f[1], iarg): k
	mr r3, r22
	mr r4, r17
	bl dol_GXInitLightDistAttn
	lwz r0, 28(r22)
	stw r0, 0(r21)
	lwz r0, 32(r22)
	stw r0, 4(r21)
	lwz r0, 36(r22)
	stw r0, 8(r21)
	b write24

specular:			# GXInitSpecularDir(light, f[0], f[1], f[2]): dir, pos
	mr r3, r22
	bl dol_GXInitSpecularDir
	lwz r0, 52(r22)
	stw r0, 0(r21)
	lwz r0, 56(r22)
	stw r0, 4(r21)
	lwz r0, 60(r22)
	stw r0, 8(r21)
	lwz r0, 40(r22)
	stw r0, 12(r21)
	lwz r0, 44(r22)
	stw r0, 16(r21)
	lwz r0, 48(r22)
	stw r0, 20(r21)
	b write24

project:			# GXProject(f[0..2], f[3..14], f[15..21], f[22..27]): sx, sy, sz
	addi r3, r20, 20
	addi r4, r20, 68
	addi r5, r20, 96
	mr r6, r21
	addi r7, r21, 4
	addi r8, r21, 8
	bl dol_GXProject

write24:
	li r0, 4
	li r3, 1
	mr r4, r21
	li r5, 24
	sc
	b loop

sphere:				# GXDrawSphere(iarg >> 8, iarg & 255): the vertex stream
	rlwinm r3, r17, 24, 24, 31
	rlwinm r4, r17, 0, 24, 31
	bl dol_GXDrawSphere
	rlwinm r3, r17, 24, 24, 31
	rlwinm r4, r17, 0, 24, 31
	addi r4, r4, 1
	mullw r5, r3, r4
	mulli r5, r5, 48	# numMajor * (numMinor + 1) * 2 vertices * 6 floats
	li r0, 4
	li r3, 1
	lis r4, gxstream@ha
	addi r4, r4, gxstream@l
	addi r4, r4, 4
	sc
	b loop
done:
	li r0, 1
	li r3, 0
	sc

# The SDK functions GXDrawSphere calls besides sinf and cosf: the vertex
# descriptor has no texture coordinate (as in TSky::perform), and the rest only
# set state.
	.globl GXGetVtxDesc
GXGetVtxDesc:
	li r0, 0
	stw r0, 0(r4)
	blr
	.globl GXGetVtxDescv, GXGetVtxAttrFmtv, GXClearVtxDesc, GXSetVtxDesc
	.globl GXSetVtxAttrFmt, GXBegin, GXSetVtxDescv, GXSetVtxAttrFmtv
GXGetVtxDescv:
GXGetVtxAttrFmtv:
GXClearVtxDesc:
GXSetVtxDesc:
GXSetVtxAttrFmt:
GXBegin:
GXSetVtxDescv:
GXSetVtxAttrFmtv:
	blr

	.section .data
	.balign 8
inbuf:	.space 128
outbuf:	.space 24
	.balign 8
light:	.space 64

	.section .bss
	.balign 65536
	.globl gxstream
gxstream:
	.space 0x400000
