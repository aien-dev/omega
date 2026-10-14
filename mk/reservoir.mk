# omega#440 ionic-droplet reservoir, Gate 1 (docs/reservoir/RESERVOIR_REFERENCE.md):
# CPU simulator of the droplet-network model of Armendarez et al. (Sci. Adv.
# 2026) plus a binary64 ridge readout. Files only under src/reservoir/ and
# tests/reservoir/. Picked up by `-include mk/*.mk`; not part of `all` or
# `test`. CPU only: opens no device, no CUDA, never touches the GB10.
#
# test-reservoir   purity check, then the tests plain and with ASan/UBSan
#                  (the ASan build runs shorter oracle scenarios, -DRES_QUICK)
# reservoir-purity the reservoir objects reference no allocator, no libm, no
#                  file or process operation (all math goes through
#                  omega_math_*; the readout's square root is the inline
#                  instruction __builtin_sqrt with -fno-math-errno)
ifndef RESERVOIR_MK
RESERVOIR_MK := 1
.PHONY: test-reservoir reservoir-purity
RES_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -O2 -Isrc -Isrc/reservoir -Itests/reservoir \
	-ffp-contract=off -fno-fast-math -fno-math-errno -DOMEGA_NUMERIC_CPU_ONLY
RES_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DRES_QUICK
RES_SRCS = src/reservoir/omega_reservoir.c src/reservoir/omega_reservoir_readout.c
RES_HDRS = src/reservoir/omega_reservoir.h src/reservoir/omega_reservoir_readout.h src/omega_numeric.h src/sha256.h
# omega_numeric.c pulls in the provenance and encoder objects at link time.
RES_DEPS = src/omega_numeric.c src/omega_numeric_provenance.c src/omega_blackwell_encoder.c \
	src/omega_blackwell_codegen.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c src/sha256.c
RES_TEST_SRCS = tests/reservoir/test_reservoir.c tests/reservoir/res_oracle64.c
RES_TEST_HDRS = tests/reservoir/res_fixtures.h tests/reservoir/res_oracle64.h
RES_DIR = $(OUT_DIR)/tests-reservoir
RES_FORBIDDEN = ' U (malloc|calloc|realloc|free|aligned_alloc|posix_memalign|exp|expf|log|logf|pow|powf|sqrt|sqrtf|fma|fmaf|fopen|fwrite|fprintf|printf|mmap|open|fork|exec.*|dlopen|system|socket|cu[A-Z].*)$$'

$(RES_DIR)/%.o: src/reservoir/%.c $(RES_HDRS)
	@mkdir -p $(RES_DIR)
	$(CC) $(RES_CFLAGS) -pedantic -c -o $@ $<

reservoir-purity: $(RES_DIR)/omega_reservoir.o $(RES_DIR)/omega_reservoir_readout.o
	@if nm -u $^ | grep -E $(RES_FORBIDDEN) ; then \
		echo "reservoir object references an allocator, libm or an I/O or process operation"; exit 1; fi
	@echo "reservoir-purity: omega_reservoir.o and omega_reservoir_readout.o reference no forbidden symbol"

$(RES_DIR)/test_reservoir: $(RES_TEST_SRCS) $(RES_SRCS) $(RES_DEPS) $(RES_HDRS) $(RES_TEST_HDRS)
	@mkdir -p $(RES_DIR)
	$(CC) $(RES_CFLAGS) -o $@ $(RES_TEST_SRCS) $(RES_SRCS) $(RES_DEPS) -lm
$(RES_DIR)/test_reservoir_asan: $(RES_TEST_SRCS) $(RES_SRCS) $(RES_DEPS) $(RES_HDRS) $(RES_TEST_HDRS)
	@mkdir -p $(RES_DIR)
	$(CC) $(RES_CFLAGS) $(RES_ASAN) -o $@ $(RES_TEST_SRCS) $(RES_SRCS) $(RES_DEPS) -lm

test-reservoir: reservoir-purity $(RES_DIR)/test_reservoir $(RES_DIR)/test_reservoir_asan
	./$(RES_DIR)/test_reservoir
	./$(RES_DIR)/test_reservoir_asan
	@echo "test-reservoir: reservoir checks pass in plain and ASan/UBSan builds"
endif
