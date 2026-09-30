# PowerPC side of tools/mslmath/check.sh: runs the DOL's own MSL objects (the
# decomp's split of the original, build/GMSE01/obj) under qemu-ppc. Reads 24-byte
# records from stdin (see harness.c), calls the function, writes f1 as 8 bytes.
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
	li r5, 24
	sc
	cmpwi r3, 24
	bne done
	lis r4, inbuf@ha
	addi r4, r4, inbuf@l
	lwz r16, 0(r4)
	lfd f1, 8(r4)
	lfd f2, 16(r4)
	lis r12, table@ha
	addi r12, r12, table@l
	slwi r16, r16, 2
	lwzx r12, r12, r16
	mtctr r12
	bctrl
	lis r4, outbuf@ha
	addi r4, r4, outbuf@l
	stfd f1, 0(r4)
	li r0, 4
	li r3, 1
	li r5, 8
	sc
	b loop
done:
	li r0, 1
	li r3, 0
	sc

	.section .data
	.balign 8
inbuf:	.space 24
outbuf:	.space 8
table:
	.4byte sinf, cosf, tanf, atanf, atan2f, acosf, atan, atan2, _inv_sqrtf
	.4byte expf, powf, dol_fmodf, dol_sqrtf, dol_sqrtf, dol_tutil_mod
	.4byte dol_tutil_sqrt, dol_tutil_inv_sqrt, dol_ms_sqrtf, dol_jpa_sqrtf
