/* POLYGLOT-0 shared verifier: callee-saved register sentinel trampoline.
 * (review G-S4; harness from docs/polyglot/review-polyglot-0.md appendix)
 *
 * int omx_cs_call(run_fn fn, const oma_rz_plan *p, const int8_t *x, int32_t *y, uint64_t out[18])
 *
 * Loads x19-x28 and d8-d15 (AAPCS64 callee-saved) with known sentinels,
 * calls fn(p, x, y), then stores the ten x registers and the eight d
 * registers to out[0..17] and restores the caller's values. Returns fn's
 * return value (w0 is not touched after the call). The sentinels are
 * OMX_CS_SENTINEL(i) = 0x5a00000000000000 | (i << 48) | (0x1111 * (i + 1)),
 * i = 0..17, built with movz/movk so every value has high and low bits set.
 */
	.text
	.p2align 4
	.globl	omx_cs_call
	.type	omx_cs_call, %function
omx_cs_call:                        // x0 fn, x1 p, x2 x, x3 y, x4 out
	stp	x29, x30, [sp, #-176]!
	mov	x29, sp
	stp	x19, x20, [sp, #16]
	stp	x21, x22, [sp, #32]
	stp	x23, x24, [sp, #48]
	stp	x25, x26, [sp, #64]
	stp	x27, x28, [sp, #80]
	stp	d8, d9, [sp, #96]
	stp	d10, d11, [sp, #112]
	stp	d12, d13, [sp, #128]
	stp	d14, d15, [sp, #144]
	str	x4, [sp, #160]
	mov	x16, x0
	mov	x0, x1
	mov	x1, x2
	mov	x2, x3
	.macro	SENT reg, i
	movz	\reg, #(0x1111 * (\i + 1)) & 0xffff
	movk	\reg, #(0x5a00 | \i), lsl #48
	.endm
	SENT	x19, 0
	SENT	x20, 1
	SENT	x21, 2
	SENT	x22, 3
	SENT	x23, 4
	SENT	x24, 5
	SENT	x25, 6
	SENT	x26, 7
	SENT	x27, 8
	SENT	x28, 9
	SENT	x9, 10
	fmov	d8, x9
	SENT	x9, 11
	fmov	d9, x9
	SENT	x9, 12
	fmov	d10, x9
	SENT	x9, 13
	fmov	d11, x9
	SENT	x9, 14
	fmov	d12, x9
	SENT	x9, 15
	fmov	d13, x9
	SENT	x9, 16
	fmov	d14, x9
	SENT	x9, 17
	fmov	d15, x9
	blr	x16
	ldr	x4, [sp, #160]
	stp	x19, x20, [x4]
	stp	x21, x22, [x4, #16]
	stp	x23, x24, [x4, #32]
	stp	x25, x26, [x4, #48]
	stp	x27, x28, [x4, #64]
	stp	d8, d9, [x4, #80]
	stp	d10, d11, [x4, #96]
	stp	d12, d13, [x4, #112]
	stp	d14, d15, [x4, #128]
	ldp	x19, x20, [sp, #16]
	ldp	x21, x22, [sp, #32]
	ldp	x23, x24, [sp, #48]
	ldp	x25, x26, [sp, #64]
	ldp	x27, x28, [sp, #80]
	ldp	d8, d9, [sp, #96]
	ldp	d10, d11, [sp, #112]
	ldp	d12, d13, [sp, #128]
	ldp	d14, d15, [sp, #144]
	ldp	x29, x30, [sp], #176
	ret
	.size	omx_cs_call, . - omx_cs_call

	.section .note.GNU-stack, "", %progbits
