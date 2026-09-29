# POLYGLOT-0 lane B2: B1's kernels emitted by Omega's own AArch64 encoder
# (spec/polyglot-0.md section 4: toolchain candidate; the gate is a byte
# comparison with GNU as plus bit-exact results).
# test-polyglot-encoder:
#   1. assembles B1's .S with GNU as (via $(CC)) into objects of its own and
#      extracts each .text with objcopy -O binary (the reference bytes);
#   2. builds tests/polyglot/test_encoder.c plain and with ASan+UBSan;
#   3. runs both: byte comparison, W^X/final-form checks, Omega decoder round
#      trip, fail-closed encoder checks, then the B1 correctness suite
#      (MA-3 grid, 20,000 random, max_n, guard pages, error contract).
# Reached twice (Makefile's mk/*.mk and mk/polyglot.mk), hence the guard.
# Self-contained variable names; read before the Makefile's own variables.
ifndef POLYGLOT_ENCODER_MK
POLYGLOT_ENCODER_MK := 1

PGE_ARCH = -march=armv8.6-a+dotprod+i8mm
PGE_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 $(PGE_ARCH) -Isrc
OBJCOPY ?= objcopy
PGE_SRCS = src/polyglot/omx_encoder.c src/polyglot/omx_encoder_ext.c src/aarch64_encoder.c \
	src/aarch64_decoder.c src/algebra/realize_common.c src/algebra/realize_binary.c \
	src/algebra/realize_bitplane.c src/algebra/realize_sparse.c \
	src/algebra/realize_rns.c src/algebra/realize_dense.c
PGE_HDRS = src/polyglot/omx_lang.h src/polyglot/omx_encoder.h src/polyglot/omx_encoder_ext.h \
	src/aarch64_encoder.h src/aarch64_decoder.h src/aarch64_target.h src/algebra/realize_common.h
PGE_DIR = $(OUT_DIR)/tests-polyglot-encoder
PGE_BINS = $(PGE_DIR)/gnu_sdot.text.bin $(PGE_DIR)/gnu_crumb.text.bin
PGE_TEST = $(PGE_DIR)/test_encoder
PGE_TEST_ASAN = $(PGE_DIR)/test_encoder_asan

$(PGE_DIR)/gnu_%.o: src/polyglot/asm/omx_%.S
	@mkdir -p $(dir $@)
	$(CC) $(PGE_ARCH) -c -o $@ $<

$(PGE_DIR)/gnu_%.text.bin: $(PGE_DIR)/gnu_%.o
	$(OBJCOPY) -O binary --only-section=.text $< $@

$(PGE_TEST): tests/polyglot/test_encoder.c $(PGE_SRCS) $(PGE_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(PGE_CFLAGS) -o $@ tests/polyglot/test_encoder.c $(PGE_SRCS) -lm

$(PGE_TEST_ASAN): tests/polyglot/test_encoder.c $(PGE_SRCS) $(PGE_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(PGE_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-o $@ tests/polyglot/test_encoder.c $(PGE_SRCS) -lm

# Hook into the shared verifier and bench (mk/polyglot.mk lane contract).
POLYGLOT_LANE_SRCS += src/polyglot/omx_encoder.c src/polyglot/omx_encoder_ext.c \
	src/aarch64_encoder.c src/aarch64_decoder.c

.PHONY: test-polyglot-encoder
test-polyglot-encoder: $(PGE_TEST) $(PGE_TEST_ASAN) $(PGE_BINS)
	./$(PGE_TEST) $(PGE_BINS)
	./$(PGE_TEST_ASAN) $(PGE_BINS)

endif
