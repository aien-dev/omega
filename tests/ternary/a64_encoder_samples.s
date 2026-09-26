// Research-only oracle input: the same instruction sequence that
// omega_t_a64_encoder_samples() emits, in GNU assembler syntax.
// Regenerate the expected words with tests/ternary/regen_a64_oracle.sh.
add x0, x1, x2
sub x3, x4, x5
and x6, x7, x8
and w6, w7, w8
bic x15, x2, x6
orr x0, x2, x3, lsl #32
orr x3, x3, x3, lsr #32
eor x4, x0, x1
mov w2, w0
neg x10, x9
cmp x3, x2, asr #63
cmp x0, x9
add x8, x7, #3
sub x8, x6, #1
cmp x7, #1
cmn x7, #1
subs x5, x5, #1
csel x0, x9, x0, gt
csinc x6, x6, x6, le
csinv x0, x2, xzr, ge
cset x4, hi
cset x5, lo
lsl x4, x4, #1
lsl w2, w0, #3
lsr x3, x0, #32
lsr w7, w2, #31
ubfx x2, x0, #3, #29
asr x1, x2, #63
ror x0, x0, #32
sdiv x6, x0, x5
msub x7, x6, x5, x0
madd x0, x0, x6, x7
mul x2, x0, x1
smulh x3, x0, x1
clz w4, w4
lsl w2, w2, w4
movz x5, #3
movk x9, #0x34aa, lsl #32
movn x1, #0x1234, lsl #16
movn w13, #0
ret
