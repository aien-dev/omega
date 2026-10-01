# OSC-1 compiler slice (docs/osc/OSC-1-DESIGN.md) and the OSC-0B exit gate
# (II.11 executable memory model). Files under src/compiler/ and tests/compiler/.
# Picked up by `-include mk/*.mk`; not part of `all` or `test`. Links nothing
# but src/compiler/** and src/sha256.c (no physics, no runtime, no gate suites).
# OSC-1 slice; not a general Omega compiler; no self-hosting.
#
# test-compiler       model sweep (10^6) + back end + compiler golden/negative/model
#                     agreement + cross-process determinism, plain and ASan/UBSan
# oscc                build/compiler/oscc <file.osc>: prints IR and code digests
# compiler-receipt    digest-named receipt under evidence/OSC-1/receipts (clean tree only)
# osc2-receipt        ITEM=contracts|structs (default contracts): receipt under evidence/OSC-2/receipts (clean tree only)
ifndef COMPILER_MK
COMPILER_MK := 1
.PHONY: test-compiler oscc compiler-receipt osc0b-model-receipt osc2-receipt
OSC_DIR = $(OUT_DIR)/compiler
OSC_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc -Isrc/compiler -Isrc/compiler/model
OSC_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
OSC_LIB = $(filter-out src/compiler/oscc_main.c,$(wildcard src/compiler/*.c)) src/sha256.c
OSC_MODEL = src/compiler/model/osc_model.c
OSC_MODEL_GEN = src/compiler/model/osc_model_gen.c
OSC_HDRS = $(wildcard src/compiler/*.h) $(wildcard src/compiler/model/*.h) src/sha256.h

$(OSC_DIR)/test_osc_model: tests/compiler/test_osc_model.c $(OSC_MODEL) $(OSC_MODEL_GEN) $(OSC_HDRS)
	@mkdir -p $(OSC_DIR)
	$(CC) $(OSC_CFLAGS) -o $@ tests/compiler/test_osc_model.c $(OSC_MODEL) $(OSC_MODEL_GEN)

$(OSC_DIR)/test_osc_model_asan: tests/compiler/test_osc_model.c $(OSC_MODEL) $(OSC_MODEL_GEN) $(OSC_HDRS)
	@mkdir -p $(OSC_DIR)
	$(CC) $(OSC_CFLAGS) $(OSC_ASAN) -o $@ tests/compiler/test_osc_model.c $(OSC_MODEL) $(OSC_MODEL_GEN)

$(OSC_DIR)/test_osc_backend: tests/compiler/test_osc_backend.c $(OSC_LIB) $(OSC_HDRS)
	@mkdir -p $(OSC_DIR)
	$(CC) $(OSC_CFLAGS) -o $@ tests/compiler/test_osc_backend.c $(OSC_LIB)

$(OSC_DIR)/test_osc_backend_asan: tests/compiler/test_osc_backend.c $(OSC_LIB) $(OSC_HDRS)
	@mkdir -p $(OSC_DIR)
	$(CC) $(OSC_CFLAGS) $(OSC_ASAN) -o $@ tests/compiler/test_osc_backend.c $(OSC_LIB)

$(OSC_DIR)/test_osc_compiler: tests/compiler/test_osc_compiler.c $(OSC_LIB) $(OSC_MODEL) $(OSC_HDRS)
	@mkdir -p $(OSC_DIR)
	$(CC) $(OSC_CFLAGS) -o $@ tests/compiler/test_osc_compiler.c $(OSC_LIB) $(OSC_MODEL)

$(OSC_DIR)/test_osc_compiler_asan: tests/compiler/test_osc_compiler.c $(OSC_LIB) $(OSC_MODEL) $(OSC_HDRS)
	@mkdir -p $(OSC_DIR)
	$(CC) $(OSC_CFLAGS) $(OSC_ASAN) -o $@ tests/compiler/test_osc_compiler.c $(OSC_LIB) $(OSC_MODEL)

$(OSC_DIR)/oscc: src/compiler/oscc_main.c $(OSC_LIB) $(OSC_HDRS)
	@mkdir -p $(OSC_DIR)
	$(CC) $(OSC_CFLAGS) -o $@ src/compiler/oscc_main.c $(OSC_LIB)

oscc: $(OSC_DIR)/oscc

# The ASan legs use smaller sweeps (stated in each test's output) to stay inside
# the 3-minute single-core budget; the plain legs run the full counts.
test-compiler: $(OSC_DIR)/test_osc_model $(OSC_DIR)/test_osc_model_asan \
		$(OSC_DIR)/test_osc_backend $(OSC_DIR)/test_osc_backend_asan \
		$(OSC_DIR)/test_osc_compiler $(OSC_DIR)/test_osc_compiler_asan $(OSC_DIR)/oscc
	$(abspath $(OSC_DIR)/test_osc_model) > $(OSC_DIR)/model.out
	@tail -1 $(OSC_DIR)/model.out; grep -q '^OSC0B_MODEL_PASS$$' $(OSC_DIR)/model.out
	$(abspath $(OSC_DIR)/test_osc_model_asan) 05c0b5eed0010001 100000 > $(OSC_DIR)/model_asan.out
	@tail -1 $(OSC_DIR)/model_asan.out; grep -q '^OSC0B_MODEL_SMOKE_PASS$$' $(OSC_DIR)/model_asan.out
	$(abspath $(OSC_DIR)/test_osc_backend) > $(OSC_DIR)/backend.out
	@tail -1 $(OSC_DIR)/backend.out; grep -q '^OSC1_BACKEND_PASS$$' $(OSC_DIR)/backend.out
	$(abspath $(OSC_DIR)/test_osc_backend_asan) > $(OSC_DIR)/backend_asan.out
	@tail -1 $(OSC_DIR)/backend_asan.out; grep -q '^OSC1_BACKEND_PASS$$' $(OSC_DIR)/backend_asan.out
	$(abspath $(OSC_DIR)/test_osc_compiler) > $(OSC_DIR)/compiler.out
	@tail -1 $(OSC_DIR)/compiler.out; grep -Eq '^OSC1_COMPILER_PASS( |$$)' $(OSC_DIR)/compiler.out
	$(abspath $(OSC_DIR)/test_osc_compiler_asan) $(OSC_COMPILER_ASAN_ARGS) > $(OSC_DIR)/compiler_asan.out
	@tail -1 $(OSC_DIR)/compiler_asan.out; grep -Eq '^OSC1_COMPILER_PASS( |$$)' $(OSC_DIR)/compiler_asan.out
	sh tests/compiler/determinism.sh $(abspath $(OSC_DIR)/oscc) > $(OSC_DIR)/determinism.out
	@tail -1 $(OSC_DIR)/determinism.out; grep -q '^OSC1_DETERMINISM_PASS$$' $(OSC_DIR)/determinism.out
	@grep "^contract fuzz:" $(OSC_DIR)/compiler.out
	@grep "^struct fuzz: .* mismatches=0$$" $(OSC_DIR)/compiler.out
	@grep "^struct fuzz: .* mismatches=0$$" $(OSC_DIR)/compiler_asan.out
	@grep "^arena fuzz: .* mismatches=0$$" $(OSC_DIR)/compiler.out
	@grep "^arena fuzz: .* mismatches=0$$" $(OSC_DIR)/compiler_asan.out
	@grep "^arena destruction order: " $(OSC_DIR)/compiler.out
	@grep "^runtime model replay: .* rejected=0$$" $(OSC_DIR)/compiler.out
	@grep "^runtime model replay: .* rejected=0$$" $(OSC_DIR)/compiler_asan.out
	@echo "test-compiler: PASS (OSC-1 slice + OSC-2 contracts + structs + arenas; OSC-2 slice; not a general Omega compiler; no self-hosting.)"

osc0b-model-receipt:
	sh tests/compiler/osc0b_model_receipt.sh

compiler-receipt:
	sh tests/compiler/osc1_receipt.sh

osc2-receipt: ITEM ?= contracts
osc2-receipt:
	sh tests/compiler/osc2_receipt.sh $(ITEM)
endif
