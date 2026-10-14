# One-layer stage 3 (omega#446): independent checker2 and signed realization receipts.
# Files: src/checker2/*, tests/checker2/*, spec/one-layer-receipts.md. Picked up by
# `-include mk/*.mk`; not part of `all` or `test`. checker2 links none of src/compiler/*.c: it
# includes no compiler header and shares no parser, evaluator or instruction table with oscc.
# Links only src/sha256.c and the vendored in-house Ed25519 (src/searchtrace/sig, PROVENANCE.txt).
#   make checker2                build/checker2/checker2
#   make test-checker2           receipt, checker and LRAT tests (plain and ASan/UBSan), corpus +
#                                black-box differential against oscc, then the mutant negative controls
#   make test-checker2-mutants   negative controls only: each GUARD line removed must fail a test
ifndef CHECKER2_MK
CHECKER2_MK := 1
.PHONY: checker2 test-checker2 test-checker2-unit test-checker2-mutants
C2_DIR = $(OUT_DIR)/checker2
C2_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc -Isrc/checker2
C2_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
C2_SIG = src/searchtrace/sig/ed25519.c src/searchtrace/sig/sha512.c src/searchtrace/sig/ct.c
C2_RECEIPT = src/checker2/c2_receipt.c src/sha256.c $(C2_SIG)
C2_HDRS = $(wildcard src/checker2/*.h) src/sha256.h src/searchtrace/sig/aienos_sig.h src/searchtrace/sig/sig_internal.h

$(C2_DIR)/test_c2_receipt: tests/checker2/test_c2_receipt.c $(C2_RECEIPT) $(C2_HDRS)
	@mkdir -p $(C2_DIR)
	$(CC) $(C2_CFLAGS) -o $@ tests/checker2/test_c2_receipt.c $(C2_RECEIPT)

$(C2_DIR)/test_c2_receipt_asan: tests/checker2/test_c2_receipt.c $(C2_RECEIPT) $(C2_HDRS)
	@mkdir -p $(C2_DIR)
	$(CC) $(C2_CFLAGS) $(C2_ASAN) -o $@ tests/checker2/test_c2_receipt.c $(C2_RECEIPT)

test-checker2-unit: $(C2_DIR)/test_c2_receipt $(C2_DIR)/test_c2_receipt_asan
	./$(C2_DIR)/test_c2_receipt tests/checker2/fixtures
	./$(C2_DIR)/test_c2_receipt_asan tests/checker2/fixtures

test-checker2-mutants:
	sh tests/checker2/mutants.sh "$(CC)" "$(C2_DIR)/mutants"

test-checker2: test-checker2-unit test-checker2-mutants

endif
