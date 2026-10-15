# One-layer stage 3 (omega#446): independent checker2 and signed realization receipts.
# Files: src/checker2/*, tests/checker2/*, spec/one-layer-receipts.md. Picked up by
# `-include mk/*.mk`; not part of `all` or `test`. checker2 links none of src/compiler/*.c: it
# includes no compiler header and shares no parser, evaluator or instruction table with oscc.
# Links only src/sha256.c and the vendored in-house Ed25519 (src/searchtrace/sig, PROVENANCE.txt).
#   make checker2                build/checker2/checker2
#   make test-checker2           receipt, checker and LRAT tests (plain and ASan/UBSan), corpus +
#                                black-box differential against oscc, blaster semantics against oscc's
#                                compiled code (black box), then the mutant negative controls
#   make test-checker2-mutants   negative controls only: each GUARD line removed must fail a test
ifndef CHECKER2_MK
CHECKER2_MK := 1
.PHONY: checker2 test-checker2 test-checker2-owner-receipt test-checker2-lrat test-checker2-unit test-checker2-mutants test-checker2-differential test-checker2-blast-native
C2_DIR = $(OUT_DIR)/checker2
C2_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc -Isrc/checker2
C2_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
C2_SIG = src/searchtrace/sig/ed25519.c src/searchtrace/sig/sha512.c src/searchtrace/sig/ct.c
C2_RECEIPT = src/checker2/c2_receipt.c src/sha256.c $(C2_SIG)
C2_HDRS = $(wildcard src/checker2/*.h) src/sha256.h src/searchtrace/sig/aienos_sig.h src/searchtrace/sig/sig_internal.h

C2_CORE = src/checker2/c2_ir.c src/checker2/c2_check.c src/checker2/c2_lrat.c src/checker2/c2_blast.c
# checker identity input (spec section 5): SHA-256 over the checker2 sources in a fixed order
C2_ID_SRCS = src/checker2/c2_ir.h src/checker2/c2_ir.c src/checker2/c2_check.c src/checker2/c2_receipt.h \
	src/checker2/c2_receipt.c src/checker2/c2_lrat.h src/checker2/c2_lrat.c src/checker2/c2_blast.h src/checker2/c2_blast.c \
	src/checker2/checker2_main.c
C2_SOURCES_SHA256 = $(shell cat $(C2_ID_SRCS) | sha256sum | cut -c1-64)
C2_IRDIR = $(C2_DIR)/differential/ir

checker2: $(C2_DIR)/checker2

$(C2_DIR)/checker2: src/checker2/checker2_main.c $(C2_CORE) $(C2_RECEIPT) $(C2_HDRS)
	@mkdir -p $(C2_DIR)
	$(CC) $(C2_CFLAGS) -DC2_SOURCES_SHA256='"$(C2_SOURCES_SHA256)"' -o $@ src/checker2/checker2_main.c $(C2_CORE) $(C2_RECEIPT)

# the same sources built under another checker identity: receipts it issues must be refused by
# checker2 unless that identity is accepted explicitly (lrat.sh, spec section 6 step 6)
$(C2_DIR)/checker2_other: src/checker2/checker2_main.c $(C2_CORE) $(C2_RECEIPT) $(C2_HDRS)
	@mkdir -p $(C2_DIR)
	$(CC) $(C2_CFLAGS) -DC2_SOURCES_SHA256='"other-checker-build-for-tests"' -o $@ src/checker2/checker2_main.c $(C2_CORE) $(C2_RECEIPT)

$(C2_DIR)/test_c2_ir: tests/checker2/test_c2_ir.c $(C2_CORE) $(C2_HDRS)
	@mkdir -p $(C2_DIR)
	$(CC) $(C2_CFLAGS) -o $@ tests/checker2/test_c2_ir.c $(C2_CORE)

$(C2_DIR)/test_c2_ir_asan: tests/checker2/test_c2_ir.c $(C2_CORE) $(C2_HDRS)
	@mkdir -p $(C2_DIR)
	$(CC) $(C2_CFLAGS) $(C2_ASAN) -o $@ tests/checker2/test_c2_ir.c $(C2_CORE)

# producer side, used only by the differential (links the producer library, never checker2)
# (own variables: mk/compiler.mk is included after this file, so its variables are empty here)
C2_PRODUCER_LIB = $(filter-out src/compiler/oscc_main.c,$(wildcard src/compiler/*.c)) src/sha256.c
$(C2_DIR)/osc_emit_ir: tests/checker2/producer/osc_emit_ir.c $(C2_PRODUCER_LIB) $(wildcard src/compiler/*.h)
	@mkdir -p $(C2_DIR)
	$(CC) $(C2_CFLAGS:-Isrc/checker2=-Isrc/compiler) -o $@ tests/checker2/producer/osc_emit_ir.c $(C2_PRODUCER_LIB)

test-checker2-differential: $(OUT_DIR)/compiler/oscc $(C2_DIR)/osc_emit_ir $(C2_DIR)/checker2
	sh tests/checker2/differential.sh $(OUT_DIR)/compiler/oscc $(C2_DIR)/osc_emit_ir $(C2_DIR)/checker2 $(C2_DIR)/differential

$(C2_DIR)/test_c2_receipt: tests/checker2/test_c2_receipt.c $(C2_RECEIPT) $(C2_HDRS)
	@mkdir -p $(C2_DIR)
	$(CC) $(C2_CFLAGS) -o $@ tests/checker2/test_c2_receipt.c $(C2_RECEIPT)

$(C2_DIR)/test_c2_receipt_asan: tests/checker2/test_c2_receipt.c $(C2_RECEIPT) $(C2_HDRS)
	@mkdir -p $(C2_DIR)
	$(CC) $(C2_CFLAGS) $(C2_ASAN) -o $@ tests/checker2/test_c2_receipt.c $(C2_RECEIPT)

# untrusted SAT solver (not in the checker identity; every UNSAT answer is re-checked by c2_lrat)
$(C2_DIR)/c2sat: src/checker2/untrusted/c2sat.c
	@mkdir -p $(C2_DIR)
	$(CC) $(C2_CFLAGS) -o $@ src/checker2/untrusted/c2sat.c

$(C2_DIR)/test_c2_lrat: tests/checker2/test_c2_lrat.c src/checker2/c2_lrat.c $(C2_HDRS)
	@mkdir -p $(C2_DIR)
	$(CC) $(C2_CFLAGS) -o $@ tests/checker2/test_c2_lrat.c src/checker2/c2_lrat.c

$(C2_DIR)/test_c2_lrat_asan: tests/checker2/test_c2_lrat.c src/checker2/c2_lrat.c $(C2_HDRS)
	@mkdir -p $(C2_DIR)
	$(CC) $(C2_CFLAGS) $(C2_ASAN) -o $@ tests/checker2/test_c2_lrat.c src/checker2/c2_lrat.c

$(C2_DIR)/test_c2_blast: tests/checker2/test_c2_blast.c $(C2_CORE) $(C2_HDRS)
	@mkdir -p $(C2_DIR)
	$(CC) $(C2_CFLAGS) -o $@ tests/checker2/test_c2_blast.c $(C2_CORE)

# 20 bounded programs proved by bit-blasting + c2sat + checker2 LRAT replay, plus negative controls
test-checker2-lrat: $(C2_DIR)/osc_emit_ir $(C2_DIR)/checker2 $(C2_DIR)/checker2_other $(C2_DIR)/c2sat $(C2_DIR)/test_c2_lrat $(C2_DIR)/test_c2_lrat_asan $(C2_DIR)/test_c2_blast test-checker2-differential
	./$(C2_DIR)/test_c2_lrat
	./$(C2_DIR)/test_c2_lrat_asan
	sh tests/checker2/lrat.sh $(C2_DIR)/osc_emit_ir $(C2_DIR)/checker2 $(C2_DIR)/c2sat $(C2_DIR)/lrat $(C2_DIR)/checker2_other
	./$(C2_DIR)/test_c2_blast $(C2_IRDIR) $(C2_DIR)/lrat

# blaster semantics against oscc's compiled AArch64 code, run as a black box (no ASan build:
# the harness jumps into mapped machine code and leaves traps with siglongjmp)
$(C2_DIR)/test_c2_blast_native: tests/checker2/test_c2_blast_native.c $(C2_CORE) $(C2_HDRS)
	@mkdir -p $(C2_DIR)
	$(CC) $(C2_CFLAGS) -o $@ tests/checker2/test_c2_blast_native.c $(C2_CORE)

test-checker2-blast-native: $(OUT_DIR)/compiler/oscc $(C2_DIR)/test_c2_blast_native
	sh tests/checker2/blast_native.sh $(OUT_DIR)/compiler/oscc $(C2_DIR)/test_c2_blast_native $(C2_DIR)/blast_native

test-checker2-unit: $(C2_DIR)/test_c2_receipt $(C2_DIR)/test_c2_receipt_asan $(C2_DIR)/test_c2_ir $(C2_DIR)/test_c2_ir_asan test-checker2-differential test-checker2-lrat test-checker2-blast-native test-checker2-owner-receipt
	./$(C2_DIR)/test_c2_receipt tests/checker2/fixtures
	./$(C2_DIR)/test_c2_receipt_asan tests/checker2/fixtures
	./$(C2_DIR)/test_c2_ir $(C2_IRDIR)
	./$(C2_DIR)/test_c2_ir_asan $(C2_IRDIR)

# positive production case: the receipt signed on Drake's YubiKey (spec section 11; fails PENDING until signed)
test-checker2-owner-receipt: $(C2_DIR)/checker2
	sh tests/checker2/owner_receipt.sh $(C2_DIR)/checker2 tests/checker2/fixtures/owner-receipt $(C2_DIR)/owner-receipt

test-checker2-mutants: test-checker2-differential test-checker2-lrat
	sh tests/checker2/mutants.sh "$(CC)" "$(C2_DIR)/mutants" "$(C2_IRDIR)" "$(C2_DIR)/lrat"

test-checker2: test-checker2-unit test-checker2-mutants

endif
