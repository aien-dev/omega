# OSC-1 compiler slice (docs/osc/OSC-1-DESIGN.md) and the OSC-0B exit gate
# (II.11 executable memory model). Files under src/compiler/ and tests/compiler/.
# Picked up by `-include mk/*.mk`; not part of `all` or `test`. Links nothing
# but src/compiler/** and src/sha256.c (no physics, no runtime, no gate suites);
# the OSC-2 item 4 test also links the legacy writer src/aarch64_encoder.c + decoder.
# OSC-1 slice; not a general Omega compiler; no self-hosting.
#
# test-compiler-quick smoke (< 60 s single core): same programs, small sweeps; CI on every change
# test-compiler-full  = test-compiler (receipts and src/compiler CI use it)
# test-osc-ir-decode  strict OSC IR decoder (omega#444): full counts, ASan/UBSan, decoder mutants
#                     (part of test-compiler; test-osc-ir-decode-quick is part of test-compiler-quick)
# test-compiler       model sweep (10^6) + back end + compiler golden/negative/model
#                     agreement + cross-process determinism, plain and ASan/UBSan
#                     + legacy AArch64 writer differential/refusal + frozen-caller check
# oscc                build/compiler/oscc <file.osc>: prints IR and code digests
# compiler-receipt    digest-named receipt under evidence/OSC-1/receipts (clean tree only)
# osc2-receipt        ITEM=contracts|structs|arenas|encoder (default contracts): receipt under evidence/OSC-2/receipts (clean tree only)
ifndef COMPILER_MK
COMPILER_MK := 1
.PHONY: test-compiler test-compiler-full test-compiler-quick oscc compiler-receipt osc0b-model-receipt osc2-receipt osc3-receipt
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

$(OSC_DIR)/test_osc_alloc: tests/compiler/test_osc_alloc.c $(OSC_LIB) $(OSC_HDRS)
	@mkdir -p $(OSC_DIR)
	$(CC) $(OSC_CFLAGS) -o $@ tests/compiler/test_osc_alloc.c $(OSC_LIB)

$(OSC_DIR)/test_osc_alloc_asan: tests/compiler/test_osc_alloc.c $(OSC_LIB) $(OSC_HDRS)
	@mkdir -p $(OSC_DIR)
	$(CC) $(OSC_CFLAGS) $(OSC_ASAN) -o $@ tests/compiler/test_osc_alloc.c $(OSC_LIB)

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

# OSC-2 item 4: the legacy AArch64 writer (src/aarch64_encoder.c) and its decoder,
# differential against the pre-OSC-2 formulas + refusal path (docs/osc/OSC-2-DESIGN.md s.4)
OSC_LEGACY_A64 = src/aarch64_encoder.c src/aarch64_decoder.c
OSC_LEGACY_A64_HDRS = src/aarch64_encoder.h src/aarch64_decoder.h src/aarch64_target.h

$(OSC_DIR)/test_legacy_a64: tests/compiler/test_legacy_a64.c $(OSC_LEGACY_A64) $(OSC_LEGACY_A64_HDRS)
	@mkdir -p $(OSC_DIR)
	$(CC) $(OSC_CFLAGS) -o $@ tests/compiler/test_legacy_a64.c $(OSC_LEGACY_A64)

$(OSC_DIR)/test_legacy_a64_asan: tests/compiler/test_legacy_a64.c $(OSC_LEGACY_A64) $(OSC_LEGACY_A64_HDRS)
	@mkdir -p $(OSC_DIR)
	$(CC) $(OSC_CFLAGS) $(OSC_ASAN) -o $@ tests/compiler/test_legacy_a64.c $(OSC_LEGACY_A64)

# omega#444 (one-layer stage 2): the strict OSC IR decoder osc_ir_decode against
# spec/one-layer-format.md. test_osc_ir_decode quick|full|asan (counts in its output).
# Decoder mutants: OSC_IR_DECODE_MUTANT=k switches off exactly one refusal in
# src/compiler/osc_ir.c; each must make the test report a FAIL (negative controls).
# ASan legs run with detect_stack_use_after_return=0: with the fake stack on, every
# osc_ir_validate call (a 6 KB frame) ran about 50x slower on the Spark (measured
# 0.012 ms vs 2.6 ms per encode); use-after-return is not what this test probes.
OSC_DEC_MUTANTS = 1 2 3 4 5 6 7 8 9 10
OSC_DEC_ASAN_ENV = ASAN_OPTIONS=detect_stack_use_after_return=0
.PHONY: test-osc-ir-decode test-osc-ir-decode-quick test-osc-ir-decode-mutants

$(OSC_DIR)/test_osc_ir_decode: tests/compiler/test_osc_ir_decode.c $(OSC_LIB) $(OSC_HDRS)
	@mkdir -p $(OSC_DIR)
	$(CC) $(OSC_CFLAGS) -o $@ tests/compiler/test_osc_ir_decode.c $(OSC_LIB)

$(OSC_DIR)/test_osc_ir_decode_asan: tests/compiler/test_osc_ir_decode.c $(OSC_LIB) $(OSC_HDRS)
	@mkdir -p $(OSC_DIR)
	$(CC) $(OSC_CFLAGS) $(OSC_ASAN) -o $@ tests/compiler/test_osc_ir_decode.c $(OSC_LIB)

$(OSC_DIR)/test_osc_ir_decode_mutant%: tests/compiler/test_osc_ir_decode.c $(OSC_LIB) $(OSC_HDRS)
	@mkdir -p $(OSC_DIR)
	$(CC) $(OSC_CFLAGS) -DOSC_IR_DECODE_MUTANT=$* -o $@ tests/compiler/test_osc_ir_decode.c $(OSC_LIB)

test-osc-ir-decode-mutants: $(foreach k,$(OSC_DEC_MUTANTS),$(OSC_DIR)/test_osc_ir_decode_mutant$(k))
	@for k in $(OSC_DEC_MUTANTS); do \
		out=$(OSC_DIR)/ir_decode_mutant$$k.out; \
		if $(abspath $(OSC_DIR))/test_osc_ir_decode_mutant$$k quick > $$out 2>&1; then echo "decoder mutant $$k NOT caught"; exit 1; fi; \
		grep -q '^FAIL: ' $$out || { echo "decoder mutant $$k stopped without a test FAIL line"; exit 1; }; \
		echo "decoder mutant $$k caught: $$(grep -m1 '^FAIL: ' $$out | cut -c1-110)"; \
	done
	@echo "test-osc-ir-decode-mutants: PASS ($(words $(OSC_DEC_MUTANTS)) of $(words $(OSC_DEC_MUTANTS)) caught)"

test-osc-ir-decode: $(OSC_DIR)/test_osc_ir_decode $(OSC_DIR)/test_osc_ir_decode_asan test-osc-ir-decode-mutants
	$(abspath $(OSC_DIR)/test_osc_ir_decode) full > $(OSC_DIR)/ir_decode.out
	@tail -1 $(OSC_DIR)/ir_decode.out; grep -q '^OSC_IR_DECODE_PASS$$' $(OSC_DIR)/ir_decode.out
	@grep -E "^(corpus|valid objects|malformed|refusal classes):" $(OSC_DIR)/ir_decode.out | cut -c1-160
	$(OSC_DEC_ASAN_ENV) $(abspath $(OSC_DIR)/test_osc_ir_decode_asan) asan > $(OSC_DIR)/ir_decode_asan.out
	@tail -1 $(OSC_DIR)/ir_decode_asan.out; grep -q '^OSC_IR_DECODE_PASS$$' $(OSC_DIR)/ir_decode_asan.out
	@echo "test-osc-ir-decode: PASS (strict decoder, plain full + ASan/UBSan + mutants)"

test-osc-ir-decode-quick: $(OSC_DIR)/test_osc_ir_decode $(OSC_DIR)/test_osc_ir_decode_asan test-osc-ir-decode-mutants
	@mkdir -p $(OSC_DIR)/quick
	$(abspath $(OSC_DIR)/test_osc_ir_decode) quick > $(OSC_DIR)/quick/ir_decode.out
	@tail -1 $(OSC_DIR)/quick/ir_decode.out; grep -q '^OSC_IR_DECODE_PASS$$' $(OSC_DIR)/quick/ir_decode.out
	$(OSC_DEC_ASAN_ENV) $(abspath $(OSC_DIR)/test_osc_ir_decode_asan) quick > $(OSC_DIR)/quick/ir_decode_asan.out
	@tail -1 $(OSC_DIR)/quick/ir_decode_asan.out; grep -q '^OSC_IR_DECODE_PASS$$' $(OSC_DIR)/quick/ir_decode_asan.out
	@echo "test-osc-ir-decode-quick: PASS (smoke counts; not gate evidence)"

# The ASan legs use smaller sweeps (stated in each test's output) to stay inside
# the 3-minute single-core budget; the plain legs run the full counts.
test-compiler: $(OSC_DIR)/test_osc_model $(OSC_DIR)/test_osc_model_asan \
		$(OSC_DIR)/test_osc_backend $(OSC_DIR)/test_osc_backend_asan \
		$(OSC_DIR)/test_osc_alloc $(OSC_DIR)/test_osc_alloc_asan \
		$(OSC_DIR)/test_osc_compiler $(OSC_DIR)/test_osc_compiler_asan $(OSC_DIR)/oscc \
		$(OSC_DIR)/test_legacy_a64 $(OSC_DIR)/test_legacy_a64_asan \
		test-osc-ir-decode
	$(abspath $(OSC_DIR)/test_osc_model) > $(OSC_DIR)/model.out
	@tail -1 $(OSC_DIR)/model.out; grep -q '^OSC0B_MODEL_PASS$$' $(OSC_DIR)/model.out
	$(abspath $(OSC_DIR)/test_osc_model_asan) 05c0b5eed0010001 100000 > $(OSC_DIR)/model_asan.out
	@tail -1 $(OSC_DIR)/model_asan.out; grep -q '^OSC0B_MODEL_SMOKE_PASS$$' $(OSC_DIR)/model_asan.out
	$(abspath $(OSC_DIR)/test_osc_backend) > $(OSC_DIR)/backend.out
	@tail -1 $(OSC_DIR)/backend.out; grep -q '^OSC1_BACKEND_PASS$$' $(OSC_DIR)/backend.out
	$(abspath $(OSC_DIR)/test_osc_backend_asan) > $(OSC_DIR)/backend_asan.out
	@tail -1 $(OSC_DIR)/backend_asan.out; grep -q '^OSC1_BACKEND_PASS$$' $(OSC_DIR)/backend_asan.out
	$(abspath $(OSC_DIR)/test_osc_alloc) > $(OSC_DIR)/alloc.out
	@tail -1 $(OSC_DIR)/alloc.out; grep -Eq '^PASS [0-9]+ [0-9]+$$' $(OSC_DIR)/alloc.out
	@grep "^alloc sweep: " $(OSC_DIR)/alloc.out
	$(abspath $(OSC_DIR)/test_osc_alloc_asan) > $(OSC_DIR)/alloc_asan.out
	@tail -1 $(OSC_DIR)/alloc_asan.out; grep -Eq '^PASS [0-9]+ [0-9]+$$' $(OSC_DIR)/alloc_asan.out
	@echo "test-osc-alloc: PASS"
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
	$(abspath $(OSC_DIR)/test_legacy_a64) > $(OSC_DIR)/legacy_a64.out
	@tail -1 $(OSC_DIR)/legacy_a64.out; grep -q '^OSC2_LEGACY_A64_PASS$$' $(OSC_DIR)/legacy_a64.out
	$(abspath $(OSC_DIR)/test_legacy_a64_asan) quick > $(OSC_DIR)/legacy_a64_asan.out
	@tail -1 $(OSC_DIR)/legacy_a64_asan.out; grep -q '^OSC2_LEGACY_A64_PASS$$' $(OSC_DIR)/legacy_a64_asan.out
	@grep "^legacy a64 differential: .* mismatches=0$$" $(OSC_DIR)/legacy_a64.out
	sh tests/compiler/legacy_a64_callers.sh --selftest > $(OSC_DIR)/legacy_callers.out
	@tail -1 $(OSC_DIR)/legacy_callers.out; grep -q '^OSC2_LEGACY_CALLERS_PASS ' $(OSC_DIR)/legacy_callers.out
	@echo "test-compiler: PASS (OSC-1 slice + OSC-2 contracts + structs + arenas + legacy AArch64 writer; OSC-2 slice; not a general Omega compiler; no self-hosting.)"


# OSC-3 item 1: the split. test-compiler-full is everything (the targets above,
# full counts; receipts use it). test-compiler is kept as an alias of full so old
# commands and scripts keep their meaning. test-compiler-quick (< 60 s single
# core) is the every-change CI leg: same binaries, same golden and negative
# programs, same determinism check, smaller sweeps and fuzz counts (stated in
# each output). Quick is a smoke run, never receipt evidence.
OSC_QDIR = $(OSC_DIR)/quick
OSC_QUICK_MODEL_SEQ ?= 100000
OSC_QUICK_FUZZ ?= 120
OSC_QUICK_ASAN_FUZZ ?= 120
test-compiler-full: test-compiler

test-compiler-quick: $(OSC_DIR)/test_osc_model $(OSC_DIR)/test_osc_backend $(OSC_DIR)/test_osc_backend_asan \
		$(OSC_DIR)/test_osc_alloc $(OSC_DIR)/test_osc_alloc_asan \
		$(OSC_DIR)/test_osc_compiler $(OSC_DIR)/test_osc_compiler_asan $(OSC_DIR)/oscc \
		$(OSC_DIR)/test_legacy_a64 $(OSC_DIR)/test_legacy_a64_asan \
		test-osc-ir-decode-quick
	@mkdir -p $(OSC_QDIR)
	$(abspath $(OSC_DIR)/test_osc_model) 05c0b5eed0010001 $(OSC_QUICK_MODEL_SEQ) > $(OSC_QDIR)/model.out
	@tail -1 $(OSC_QDIR)/model.out; grep -Eq '^OSC0B_MODEL_(SMOKE_)?PASS$$' $(OSC_QDIR)/model.out
	$(abspath $(OSC_DIR)/test_osc_backend) > $(OSC_QDIR)/backend.out
	@tail -1 $(OSC_QDIR)/backend.out; grep -q '^OSC1_BACKEND_PASS$$' $(OSC_QDIR)/backend.out
	$(abspath $(OSC_DIR)/test_osc_backend_asan) > $(OSC_QDIR)/backend_asan.out
	@tail -1 $(OSC_QDIR)/backend_asan.out; grep -q '^OSC1_BACKEND_PASS$$' $(OSC_QDIR)/backend_asan.out
	$(abspath $(OSC_DIR)/test_osc_alloc) > $(OSC_QDIR)/alloc.out
	@tail -1 $(OSC_QDIR)/alloc.out; grep -Eq '^PASS [0-9]+ [0-9]+$$' $(OSC_QDIR)/alloc.out
	@grep "^alloc sweep: " $(OSC_QDIR)/alloc.out
	$(abspath $(OSC_DIR)/test_osc_alloc_asan) > $(OSC_QDIR)/alloc_asan.out
	@tail -1 $(OSC_QDIR)/alloc_asan.out; grep -Eq '^PASS [0-9]+ [0-9]+$$' $(OSC_QDIR)/alloc_asan.out
	@echo "test-osc-alloc: PASS"
	$(abspath $(OSC_DIR)/test_osc_compiler) $(OSC_QUICK_FUZZ) > $(OSC_QDIR)/compiler.out
	@tail -1 $(OSC_QDIR)/compiler.out; grep -Eq '^OSC1_COMPILER_PASS( |$$)' $(OSC_QDIR)/compiler.out
	$(abspath $(OSC_DIR)/test_osc_compiler_asan) $(OSC_QUICK_ASAN_FUZZ) > $(OSC_QDIR)/compiler_asan.out
	@tail -1 $(OSC_QDIR)/compiler_asan.out; grep -Eq '^OSC1_COMPILER_PASS( |$$)' $(OSC_QDIR)/compiler_asan.out
	sh tests/compiler/determinism.sh $(abspath $(OSC_DIR)/oscc) > $(OSC_QDIR)/determinism.out
	@tail -1 $(OSC_QDIR)/determinism.out; grep -q '^OSC1_DETERMINISM_PASS$$' $(OSC_QDIR)/determinism.out
	@grep "^struct fuzz: .* mismatches=0$$" $(OSC_QDIR)/compiler.out
	@grep "^arena fuzz: .* mismatches=0$$" $(OSC_QDIR)/compiler.out
	@grep "^runtime model replay: .* rejected=0$$" $(OSC_QDIR)/compiler.out
	$(abspath $(OSC_DIR)/test_legacy_a64) quick > $(OSC_QDIR)/legacy_a64.out
	@tail -1 $(OSC_QDIR)/legacy_a64.out; grep -q '^OSC2_LEGACY_A64_PASS$$' $(OSC_QDIR)/legacy_a64.out
	sh tests/compiler/legacy_a64_callers.sh --selftest > $(OSC_QDIR)/legacy_callers.out
	@tail -1 $(OSC_QDIR)/legacy_callers.out; grep -q '^OSC2_LEGACY_CALLERS_PASS ' $(OSC_QDIR)/legacy_callers.out
	@echo "test-compiler-quick: PASS (smoke counts; not receipt evidence; full = make test-compiler-full. OSC-3 slice; not a general Omega compiler; no self-hosting.)"

osc0b-model-receipt:
	sh tests/compiler/osc0b_model_receipt.sh

compiler-receipt:
	sh tests/compiler/osc1_receipt.sh

osc2-receipt: ITEM ?= contracts
osc2-receipt:
	sh tests/compiler/osc2_receipt.sh $(ITEM)

# OSC-3: ITEM=review|split|handles|dropflags|effects|identity|module; needs PHYSICS_DIR (M6/M9/M14)
osc3-receipt:
	sh tests/compiler/osc3_receipt.sh $(ITEM)
endif
