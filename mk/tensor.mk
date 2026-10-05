# M20 OMEGA_TENSOR (docs/tensor/M20_OMEGA_TENSOR.md): semantic tensor layer +
# CPU realization. Files under src/tensor/ and tests/test_omega_tensor.c.
# Picked up by `-include mk/*.mk`. Not part of `all` or `test`. CPU only:
# opens no device, never touches the GB10.
#
# test-tensor            CPU tests plain and with ASan/UBSan, then
#                        test-tensor-no-hooks
# test-tensor-no-hooks   builds src/tensor/*.c with the default (library)
#                        flags and requires, via nm, that the test-only
#                        hook omega_tensor_test_set_storage_generation is
#                        absent. Only the test binaries define
#                        OMEGA_TENSOR_TEST_HOOKS.
# test-tensor-mutations  applies each MUT: source mutation in a scratch copy
#                        and proves test-tensor's binary then fails
# test-m20-receipt       tests/test_m20_receipt.sh: content-addressed M20
#                        receipt writer tools/m20_receipt.sh (fixtures only)
# test-tensor-e1-reduce  alias of test-tensor (the reduction seam always
#                        calls omega_reduce_cpu, E1 WP-D, omega #134)
# test-tensor-store      crash-safe storage lifetime (omega_tensor_store):
#                        plain + ASan/UBSan, then test-tensor-no-hooks
ifndef TENSOR_MK
TENSOR_MK := 1
.PHONY: test-tensor test-tensor-no-hooks test-tensor-mutations test-tensor-e1-reduce test-tensor-store
TENSOR_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -ffp-contract=off -Isrc -Isrc/tensor \
	-DOMEGA_NUMERIC_CPU_ONLY
TENSOR_SRCS = src/tensor/omega_tensor.c src/tensor/omega_tensor_cpu.c src/tensor/omega_tensor_reduce_seam.c \
	src/omega_numeric_reduce.c src/omega_numeric_transc.c
TENSOR_DEPS = src/omega_numeric.c src/omega_numeric_provenance.c src/omega_blackwell_encoder.c \
	src/omega_blackwell_codegen.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c src/sha256.c
TENSOR_HDRS = src/tensor/omega_tensor.h src/tensor/omega_tensor_reduce_seam.h src/omega_numeric_reduce.h src/omega_numeric.h src/sha256.h src/omega_numeric_transc.h
# CR-5 tests are #included by tests/test_omega_tensor.c: rebuild when they change.
TENSOR_HDRS += tests/tensor_reduce_multi_tests.inc
# Test builds only: compiles the test-only storage-generation hook.
TENSOR_TEST_FLAGS = -DOMEGA_TENSOR_TEST_HOOKS
TENSOR_HOOK_SYMS = omega_tensor_test_set_storage_generation omega_tensor_store_test_set_crash_step
# Crash-safe storage lifetime (omega_tensor_store); targets at the end of this file.
TENSOR_STORE_SRCS = src/tensor/omega_tensor_store.c
TENSOR_STORE_HDRS = src/tensor/omega_tensor_store.h

$(OUT_DIR)/test_omega_tensor: tests/test_omega_tensor.c $(TENSOR_SRCS) $(TENSOR_DEPS) $(TENSOR_HDRS)
	@mkdir -p $(OUT_DIR)
	gcc $(TENSOR_CFLAGS) $(TENSOR_TEST_FLAGS) -O2 -o $@ tests/test_omega_tensor.c $(TENSOR_SRCS) $(TENSOR_DEPS)

$(OUT_DIR)/test_omega_tensor_asan: tests/test_omega_tensor.c $(TENSOR_SRCS) $(TENSOR_DEPS) $(TENSOR_HDRS)
	@mkdir -p $(OUT_DIR)
	gcc $(TENSOR_CFLAGS) $(TENSOR_TEST_FLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-o $@ tests/test_omega_tensor.c $(TENSOR_SRCS) $(TENSOR_DEPS)

test-tensor: $(OUT_DIR)/test_omega_tensor $(OUT_DIR)/test_omega_tensor_asan
	./$(OUT_DIR)/test_omega_tensor
	./$(OUT_DIR)/test_omega_tensor_asan
	$(MAKE) --no-print-directory test-tensor-no-hooks

# Default (library) build of the tensor sources: no OMEGA_TENSOR_TEST_HOOKS.
# Fails if any test-only hook symbol is defined or referenced in the objects.
test-tensor-no-hooks: src/tensor/omega_tensor.c src/tensor/omega_tensor_cpu.c src/tensor/omega_tensor_reduce_seam.c $(TENSOR_HDRS) \
		$(TENSOR_STORE_SRCS) $(TENSOR_STORE_HDRS)
	@mkdir -p $(OUT_DIR)/tensor-lib
	gcc $(TENSOR_CFLAGS) -O2 -c -o $(OUT_DIR)/tensor-lib/omega_tensor.o src/tensor/omega_tensor.c
	gcc $(TENSOR_CFLAGS) -O2 -c -o $(OUT_DIR)/tensor-lib/omega_tensor_cpu.o src/tensor/omega_tensor_cpu.c
	gcc $(TENSOR_CFLAGS) -O2 -c -o $(OUT_DIR)/tensor-lib/omega_tensor_reduce_seam.o src/tensor/omega_tensor_reduce_seam.c
	gcc $(TENSOR_CFLAGS) -O2 -c -o $(OUT_DIR)/tensor-lib/omega_tensor_store.o src/tensor/omega_tensor_store.c
	@if nm $(OUT_DIR)/tensor-lib/omega_tensor_store.o | grep -q "\b_exit\b"; then \
		echo "test-tensor-no-hooks: FAIL (crash hook _exit referenced by default omega_tensor_store.o)"; exit 1; fi
	@nm $(OUT_DIR)/tensor-lib/omega_tensor.o | grep -q " T omega_tensor_ctx_create\b" \
		|| { echo "test-tensor-no-hooks: FAIL (nm sanity: omega_tensor_ctx_create not found)"; exit 1; }
	@for s in $(TENSOR_HOOK_SYMS); do \
		if nm $(OUT_DIR)/tensor-lib/*.o | grep -q "\b$$s\b"; then \
			echo "test-tensor-no-hooks: FAIL ($$s present in default build)"; exit 1; fi; \
	done; echo "test-tensor-no-hooks: PASS (test-only hook absent from default build)"

test-tensor-mutations:
	tools/tensor_mutations.sh

# Kept as an alias: the seam now always calls omega_reduce_cpu (E1 WP-D, #134 merged).
test-tensor-e1-reduce: test-tensor

.PHONY: test-m20-receipt
test-m20-receipt:
	tests/test_m20_receipt.sh

# Crash-safe storage lifetime (src/tensor/omega_tensor_store.{c,h}): round
# trip, crash at every commit phase (fork + test-only hook), torn writes,
# crafted journals. Plain and ASan/UBSan, then test-tensor-no-hooks. Writes
# only under $TMPDIR (default /tmp). Its mutants run in test-tensor-mutations.

$(OUT_DIR)/test_omega_tensor_store: tests/test_omega_tensor_store.c $(TENSOR_STORE_SRCS) $(TENSOR_SRCS) $(TENSOR_DEPS) \
		$(TENSOR_HDRS) $(TENSOR_STORE_HDRS)
	@mkdir -p $(OUT_DIR)
	gcc $(TENSOR_CFLAGS) $(TENSOR_TEST_FLAGS) -O2 -o $@ tests/test_omega_tensor_store.c $(TENSOR_STORE_SRCS) \
		$(TENSOR_SRCS) $(TENSOR_DEPS)

$(OUT_DIR)/test_omega_tensor_store_asan: tests/test_omega_tensor_store.c $(TENSOR_STORE_SRCS) $(TENSOR_SRCS) \
		$(TENSOR_DEPS) $(TENSOR_HDRS) $(TENSOR_STORE_HDRS)
	@mkdir -p $(OUT_DIR)
	gcc $(TENSOR_CFLAGS) $(TENSOR_TEST_FLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-o $@ tests/test_omega_tensor_store.c $(TENSOR_STORE_SRCS) $(TENSOR_SRCS) $(TENSOR_DEPS)

test-tensor-store: $(OUT_DIR)/test_omega_tensor_store $(OUT_DIR)/test_omega_tensor_store_asan
	./$(OUT_DIR)/test_omega_tensor_store
	./$(OUT_DIR)/test_omega_tensor_store_asan
	$(MAKE) --no-print-directory test-tensor-no-hooks

# ---- GB10 realization table (M20 cut gb10) ---------------------------------
# test-tensor-gb10-host  PHYSICS_DIR=<physics checkout at physics.lock>:
#                        builds the GB10 table + tests/test_omega_tensor_gb10.c
#                        against physics and runs host self checks only (no
#                        --chip, no device), the nm no-CPU-fallback check and
#                        the GB10 MUT: mutants (tools/tensor_mutations.sh --gb10)
# test-tensor-gb10-chip  the chip parity gate: tests/run_tensor_chip.sh (pins,
#                        clean trees, /tmp/aien-gb10.lock, receipt). Refuses
#                        unless GB10_CHIP_RUN=1 is set, so it never runs by
#                        accident. Never pass a quiet flag.
.PHONY: test-tensor-gb10-host test-tensor-gb10-chip
test-tensor-gb10-host:
	@test -n "$(PHYSICS_DIR)" && test -d "$(PHYSICS_DIR)" \
		|| { echo "test-tensor-gb10-host: set PHYSICS_DIR to the physics checkout"; exit 2; }
	tools/tensor_mutations.sh --gb10 "$(PHYSICS_DIR)"

test-tensor-gb10-chip:
	@test "$$GB10_CHIP_RUN" = 1 || { echo "test-tensor-gb10-chip: chip run refused (set GB10_CHIP_RUN=1; forge only)"; exit 2; }
	@test -n "$(PHYSICS_DIR)" || { echo "test-tensor-gb10-chip: set PHYSICS_DIR"; exit 2; }
	tests/run_tensor_chip.sh --physics-dir "$(PHYSICS_DIR)"
endif
