# TURING companion records for continuous-value evidence (qprofile, qworld,
# qfreeze, qgain) and their sidecar manifest formats.
# New files only: src/turing/ty_qrecord.*, tests/turing/test_ty_qrecord.c,
# docs/turing/TURING_QCONT_RECORDS_V0.md. Picked up by `-include mk/*.mk`; no
# Makefile edit; redefines nothing that exists.
#
# test-turing-qrecord: builds the test plain and with ASan/UBSan and runs both.
ifndef TURING_QRECORD_MK
TURING_QRECORD_MK := 1
.PHONY: test-turing-qrecord
TYQR_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Isrc
TYQR_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
TYQR_SRCS = src/turing/ty_qrecord.c src/turing/ty_record.c src/turing/ty_math.c src/turing/ty_ctr1.c \
	src/turing/ty_model.c src/turing/ty_profile.c src/omega_canonical.c src/sha256.c
TYQR_HDRS = src/turing/ty_qrecord.h src/turing/ty_record.h src/turing/ty_math.h src/turing/ty_ctr1.h \
	src/turing/ty_model.h src/omega_canonical.h src/omega_types.h src/sha256.h
TYQR_DIR = $(OUT_DIR)/tests-turing-qrecord

$(TYQR_DIR)/test_ty_qrecord: tests/turing/test_ty_qrecord.c $(TYQR_SRCS) $(TYQR_HDRS)
	@mkdir -p $(TYQR_DIR)
	$(CC) $(TYQR_CFLAGS) -o $@ tests/turing/test_ty_qrecord.c $(TYQR_SRCS) -lm

$(TYQR_DIR)/test_ty_qrecord_asan: tests/turing/test_ty_qrecord.c $(TYQR_SRCS) $(TYQR_HDRS)
	@mkdir -p $(TYQR_DIR)
	$(CC) $(TYQR_CFLAGS) $(TYQR_ASAN) -o $@ tests/turing/test_ty_qrecord.c $(TYQR_SRCS) -lm

test-turing-qrecord: $(TYQR_DIR)/test_ty_qrecord $(TYQR_DIR)/test_ty_qrecord_asan
	./$(TYQR_DIR)/test_ty_qrecord
	./$(TYQR_DIR)/test_ty_qrecord_asan
	@echo "test-turing-qrecord: record builders, verifiers, refusals and manifests pass in plain and ASan/UBSan builds"
endif
