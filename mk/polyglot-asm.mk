# POLYGLOT-0 lane B1: hand AArch64 assembly candidates (spec/polyglot-0.md).
# test-polyglot-asm: bit-exact gate (MA-3 grid, 20,000 random, max_n, guard
# pages, error contract), plain build and ASan+UBSan build.
# This file is reached twice (Makefile's mk/*.mk and mk/polyglot.mk), hence
# the guard. It is read before the Makefile defines its own variables, so
# every list here is self-contained.
ifndef POLYGLOT_ASM_MK
POLYGLOT_ASM_MK := 1

PGA_ARCH = -march=armv8.6-a+dotprod+i8mm
PGA_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 $(PGA_ARCH) -Isrc
PGA_ASM = src/polyglot/asm/omx_sdot.S src/polyglot/asm/omx_crumb.S
PGA_SRCS = src/polyglot/omx_asm.c src/algebra/realize_common.c src/algebra/realize_binary.c \
	src/algebra/realize_bitplane.c src/algebra/realize_sparse.c \
	src/algebra/realize_rns.c src/algebra/realize_dense.c
PGA_HDRS = src/polyglot/omx_lang.h src/algebra/realize_common.h
PGA_DIR = $(OUT_DIR)/tests-polyglot
PGA_OBJS = $(PGA_DIR)/omx_sdot.o $(PGA_DIR)/omx_crumb.o
PGA_TEST = $(PGA_DIR)/test_asm
PGA_TEST_ASAN = $(PGA_DIR)/test_asm_asan

$(PGA_DIR)/%.o: src/polyglot/asm/%.S
	@mkdir -p $(dir $@)
	$(CC) $(PGA_ARCH) -c -o $@ $<

$(PGA_TEST): tests/polyglot/test_asm.c $(PGA_SRCS) $(PGA_HDRS) $(PGA_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(PGA_CFLAGS) -o $@ tests/polyglot/test_asm.c $(PGA_SRCS) $(PGA_OBJS) -lm

$(PGA_TEST_ASAN): tests/polyglot/test_asm.c $(PGA_SRCS) $(PGA_HDRS) $(PGA_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(PGA_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-o $@ tests/polyglot/test_asm.c $(PGA_SRCS) $(PGA_OBJS) -lm

.PHONY: test-polyglot-asm
test-polyglot-asm: $(PGA_TEST) $(PGA_TEST_ASAN)
	./$(PGA_TEST)
	./$(PGA_TEST_ASAN)

endif
