# TURING quantized continuous scorer (qint.v1) and PRD1 reader.
# New files only: src/turing/ty_qcont.*, src/turing/ty_prd.*,
# tests/turing/test_ty_qcont.c, tests/turing/qcont_kat_gen.c. Picked up by
# `-include mk/*.mk`; no Makefile edit; redefines nothing that exists.
#
# test-turing-qcont: builds the known-answer generator (long double Simpson,
# also linked in-process into the test) and the test, plain + ASan/UBSan.
ifndef TURING_QCONT_MK
TURING_QCONT_MK := 1
.PHONY: test-turing-qcont
TYQC_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Isrc -Itests/turing -ffp-contract=off -fno-fast-math
TYQC_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
TYQC_SRCS = src/turing/ty_qcont.c src/turing/ty_prd.c src/turing/ty_math.c
TYQC_HDRS = src/turing/ty_qcont.h src/turing/ty_prd.h src/turing/ty_math.h
TYQC_DIR = $(OUT_DIR)/tests-turing-qcont

$(TYQC_DIR)/qcont_kat_gen: tests/turing/qcont_kat_gen.c
	@mkdir -p $(TYQC_DIR)
	$(CC) $(TYQC_CFLAGS) -DQKAT_MAIN -o $@ tests/turing/qcont_kat_gen.c -lm

$(TYQC_DIR)/test_ty_qcont: tests/turing/test_ty_qcont.c tests/turing/qcont_kat_gen.c $(TYQC_SRCS) $(TYQC_HDRS)
	@mkdir -p $(TYQC_DIR)
	$(CC) $(TYQC_CFLAGS) -o $@ tests/turing/test_ty_qcont.c $(TYQC_SRCS) -lm

$(TYQC_DIR)/test_ty_qcont_asan: tests/turing/test_ty_qcont.c tests/turing/qcont_kat_gen.c $(TYQC_SRCS) $(TYQC_HDRS)
	@mkdir -p $(TYQC_DIR)
	$(CC) $(TYQC_CFLAGS) $(TYQC_ASAN) -o $@ tests/turing/test_ty_qcont.c $(TYQC_SRCS) -lm

test-turing-qcont: $(TYQC_DIR)/qcont_kat_gen $(TYQC_DIR)/test_ty_qcont $(TYQC_DIR)/test_ty_qcont_asan
	./$(TYQC_DIR)/qcont_kat_gen > $(TYQC_DIR)/kat_table.txt
	! grep -q REFUSED $(TYQC_DIR)/kat_table.txt
	./$(TYQC_DIR)/test_ty_qcont
	./$(TYQC_DIR)/test_ty_qcont_asan
	@echo "test-turing-qcont: qint.v1 KATs, refusals and PRD1 cases pass in plain and ASan/UBSan builds"
endif
