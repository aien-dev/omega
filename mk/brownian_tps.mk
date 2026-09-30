# Lane T phases 1-2: TPS1 binariser with qint.v1 proof, hooked to the frozen EXP-001 coders.
# New files only: tools/brownian/brw_tps_adapter.*, tests/brownian/test_brw_tps.c.
# Picked up by `-include mk/*.mk`. Frozen coder sources are linked, never edited.
#
# test-brownian-tps: builds the test plain and with ASan/UBSan and runs both.
ifndef BROWNIAN_TPS_MK
BROWNIAN_TPS_MK := 1
.PHONY: test-brownian-tps
BRWT_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -O2 -ffp-contract=off -D_POSIX_C_SOURCE=200809L -Isrc -Itools
BRWT_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
BRWT_SRCS = tools/brownian/brw_tps_adapter.c src/turing/ty_qcont.c src/turing/ty_math.c src/turing/tc_pstream.c src/turing/tc_range.c src/turing/tc_rans.c src/sha256.c
BRWT_HDRS = tools/brownian/brw_tps_adapter.h src/turing/ty_qcont.h src/turing/ty_math.h src/turing/tc_pstream.h src/turing/tc_range.h src/turing/tc_rans.h src/sha256.h
BRWT_DIR = $(OUT_DIR)/tests-brownian-tps

$(BRWT_DIR)/test_brw_tps: tests/brownian/test_brw_tps.c $(BRWT_SRCS) $(BRWT_HDRS)
	@mkdir -p $(BRWT_DIR)
	$(CC) $(BRWT_CFLAGS) -o $@ tests/brownian/test_brw_tps.c $(BRWT_SRCS) -lm

$(BRWT_DIR)/test_brw_tps_asan: tests/brownian/test_brw_tps.c $(BRWT_SRCS) $(BRWT_HDRS)
	@mkdir -p $(BRWT_DIR)
	$(CC) $(BRWT_CFLAGS) $(BRWT_ASAN) -o $@ tests/brownian/test_brw_tps.c $(BRWT_SRCS) -lm

test-brownian-tps: $(BRWT_DIR)/test_brw_tps $(BRWT_DIR)/test_brw_tps_asan
	./$(BRWT_DIR)/test_brw_tps
	./$(BRWT_DIR)/test_brw_tps_asan
	@echo "test-brownian-tps: TPS1 binariser proofs and refusals pass in plain and ASan/UBSan builds"
endif
