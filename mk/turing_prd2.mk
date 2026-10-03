# PRD2 mixture-capable prediction format + mixture scorer (EXP-002D Stage A).
# New files only: src/turing/ty_prd2.*, tests/turing/test_ty_prd2.c. Reads
# ty_qcont/ty_prd/ty_math; changes none of them. Picked up by `-include mk/*.mk`.
ifndef TURING_PRD2_MK
TURING_PRD2_MK := 1
.PHONY: test-turing-prd2
TYP2_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Isrc -ffp-contract=off -fno-fast-math
TYP2_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
TYP2_SRCS = src/turing/ty_prd2.c src/turing/ty_qcont.c src/turing/ty_prd.c src/turing/ty_math.c
TYP2_HDRS = src/turing/ty_prd2.h src/turing/ty_qcont.h src/turing/ty_prd.h src/turing/ty_math.h
TYP2_DIR = $(OUT_DIR)/tests-turing-prd2

$(TYP2_DIR)/test_ty_prd2: tests/turing/test_ty_prd2.c $(TYP2_SRCS) $(TYP2_HDRS)
	@mkdir -p $(TYP2_DIR)
	$(CC) $(TYP2_CFLAGS) -o $@ tests/turing/test_ty_prd2.c $(TYP2_SRCS) -lm

$(TYP2_DIR)/test_ty_prd2_asan: tests/turing/test_ty_prd2.c $(TYP2_SRCS) $(TYP2_HDRS)
	@mkdir -p $(TYP2_DIR)
	$(CC) $(TYP2_CFLAGS) $(TYP2_ASAN) -o $@ tests/turing/test_ty_prd2.c $(TYP2_SRCS) -lm

test-turing-prd2: $(TYP2_DIR)/test_ty_prd2 $(TYP2_DIR)/test_ty_prd2_asan
	./$(TYP2_DIR)/test_ty_prd2
	./$(TYP2_DIR)/test_ty_prd2_asan > /dev/null
	@echo "test-turing-prd2: PRD2 wire, R28-v2 and controls N1-N4 pass in plain and ASan/UBSan builds"
endif
