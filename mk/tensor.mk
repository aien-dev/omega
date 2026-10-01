# M20 OMEGA_TENSOR (docs/tensor/M20_OMEGA_TENSOR.md): semantic tensor layer +
# CPU realization. Files under src/tensor/ and tests/test_omega_tensor.c.
# Picked up by `-include mk/*.mk`. Not part of `all` or `test`. CPU only:
# opens no device, never touches the GB10.
#
# test-tensor            CPU tests plain and with ASan/UBSan
# test-tensor-mutations  applies each MUT: source mutation in a scratch copy
#                        and proves test-tensor's binary then fails
# test-tensor-e1-reduce  alias of test-tensor (the reduction seam always
#                        calls omega_reduce_cpu, E1 WP-D, omega #134)
ifndef TENSOR_MK
TENSOR_MK := 1
.PHONY: test-tensor test-tensor-mutations test-tensor-e1-reduce
TENSOR_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -ffp-contract=off -Isrc -Isrc/tensor \
	-DOMEGA_NUMERIC_CPU_ONLY
TENSOR_SRCS = src/tensor/omega_tensor.c src/tensor/omega_tensor_cpu.c src/tensor/omega_tensor_reduce_seam.c \
	src/omega_numeric_reduce.c
TENSOR_DEPS = src/omega_numeric.c src/omega_numeric_provenance.c src/omega_blackwell_encoder.c \
	src/omega_blackwell_codegen.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c src/sha256.c
TENSOR_HDRS = src/tensor/omega_tensor.h src/tensor/omega_tensor_reduce_seam.h src/omega_numeric_reduce.h src/omega_numeric.h src/sha256.h

$(OUT_DIR)/test_omega_tensor: tests/test_omega_tensor.c $(TENSOR_SRCS) $(TENSOR_DEPS) $(TENSOR_HDRS)
	@mkdir -p $(OUT_DIR)
	gcc $(TENSOR_CFLAGS) -O2 -o $@ tests/test_omega_tensor.c $(TENSOR_SRCS) $(TENSOR_DEPS)

$(OUT_DIR)/test_omega_tensor_asan: tests/test_omega_tensor.c $(TENSOR_SRCS) $(TENSOR_DEPS) $(TENSOR_HDRS)
	@mkdir -p $(OUT_DIR)
	gcc $(TENSOR_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-o $@ tests/test_omega_tensor.c $(TENSOR_SRCS) $(TENSOR_DEPS)

test-tensor: $(OUT_DIR)/test_omega_tensor $(OUT_DIR)/test_omega_tensor_asan
	./$(OUT_DIR)/test_omega_tensor
	./$(OUT_DIR)/test_omega_tensor_asan

test-tensor-mutations:
	tools/tensor_mutations.sh

# Kept as an alias: the seam now always calls omega_reduce_cpu (E1 WP-D, #134 merged).
test-tensor-e1-reduce: test-tensor
endif
