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
#
# Gate 2 (tools/reservoir/, outside the purity check: tools allocate and do I/O):
# test-reservoir also builds res_qualify, runs the archive-reader tests on the
#                  two committed CC-BY fixtures (plain and ASan/UBSan) and checks
#                  that the declared configuration has not drifted from
#                  docs/reservoir/gate2_config.json
# qualify-reservoir RES_ARCHIVE=<unpacked project-zenodo-ready_with-paper dir>
#                  [RES_V2_RESULTS=<v2 results .mat>]: the Gate 2 comparison,
#                  benchmarks and timing on a clean tree; writes a content-
#                  addressed receipt under evidence/RESERVOIR/ (never overwrites)
ifndef RESERVOIR_MK
RESERVOIR_MK := 1
.PHONY: test-reservoir reservoir-purity qualify-reservoir
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
RES_TOOL_SRCS = tools/reservoir/res_qualify.c tools/reservoir/res_bench.c tools/reservoir/res_common.c \
	tools/reservoir/res_mat.c tools/reservoir/res_inflate.c
RES_TOOL_HDRS = tools/reservoir/res_common.h tools/reservoir/res_mat.h tools/reservoir/res_inflate.h
RES_TOOL_CFLAGS = $(RES_CFLAGS) -Itools/reservoir -pthread
RES_MAT_SRCS = tools/reservoir/res_mat.c tools/reservoir/res_inflate.c src/sha256.c
RES_MAT_TEST = tests/reservoir/test_res_mat.c

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

$(RES_DIR)/res_qualify: $(RES_TOOL_SRCS) $(RES_TOOL_HDRS) $(RES_SRCS) $(RES_DEPS) $(RES_HDRS) $(RES_TEST_HDRS)
	@mkdir -p $(RES_DIR)
	$(CC) $(RES_TOOL_CFLAGS) -DRES_BUILD_FLAGS='"$(strip $(RES_TOOL_CFLAGS))"' -o $@ $(RES_TOOL_SRCS) $(RES_SRCS) $(RES_DEPS) -lm
$(RES_DIR)/test_res_mat: $(RES_MAT_TEST) $(RES_MAT_SRCS) $(RES_TOOL_HDRS) $(RES_TEST_HDRS)
	@mkdir -p $(RES_DIR)
	$(CC) $(RES_TOOL_CFLAGS) -o $@ $(RES_MAT_TEST) $(RES_MAT_SRCS)
$(RES_DIR)/test_res_mat_asan: $(RES_MAT_TEST) $(RES_MAT_SRCS) $(RES_TOOL_HDRS) $(RES_TEST_HDRS)
	@mkdir -p $(RES_DIR)
	$(CC) $(RES_TOOL_CFLAGS) $(RES_ASAN) -o $@ $(RES_MAT_TEST) $(RES_MAT_SRCS)

test-reservoir: reservoir-purity $(RES_DIR)/test_reservoir $(RES_DIR)/test_reservoir_asan $(RES_DIR)/test_res_mat \
		$(RES_DIR)/test_res_mat_asan $(RES_DIR)/res_qualify
	./$(RES_DIR)/test_reservoir
	./$(RES_DIR)/test_reservoir_asan
	./$(RES_DIR)/test_res_mat
	./$(RES_DIR)/test_res_mat_asan
	./$(RES_DIR)/res_qualify config | cmp - docs/reservoir/gate2_config.json
	@echo "test-reservoir: reservoir checks pass in plain and ASan/UBSan builds; Gate 2 config unchanged"

qualify-reservoir:
	@test -n "$(RES_ARCHIVE)" || { echo "qualify-reservoir: set RES_ARCHIVE=<unpacked project-zenodo-ready_with-paper dir>"; exit 2; }
	RES_ARCHIVE="$(RES_ARCHIVE)" RES_V2_RESULTS="$(RES_V2_RESULTS)" tools/reservoir/qualify_reservoir.sh
endif
