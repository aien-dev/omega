# Turing Yield TY-1 (math contract) + TY-2 (held-out predictive compression).
# docs/turing/TURING_YIELD_PROFILE_V0.md. Picked up by `-include mk/*.mk` in the
# Makefile. New files only: src/turing/ty_*, tests/turing/test_ty_math.c,
# tools/turing_yield.c. Nothing here touches src/runtime, src/algebra or
# src/polyglot, and no existing turing.* record or golden.
#
# test-turing-yield: plain + ASan/UBSan suites, then the fixture printout from
# both builds must be byte-identical (fixed-point determinism).
.PHONY: test-turing-yield turing-yield
TY_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Isrc
TY_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
TY_SRCS = src/turing/ty_math.c src/turing/ty_ctr1.c src/turing/ty_model.c src/turing/ty_record.c \
	src/turing/ty_profile.c src/omega_canonical.c src/sha256.c
TY_HDRS = src/turing/ty_math.h src/turing/ty_ctr1.h src/turing/ty_model.h src/turing/ty_record.h \
	src/omega_canonical.h src/omega_types.h src/sha256.h
TY_DIR = $(OUT_DIR)/tests-turing-yield
TY_TEST = $(TY_DIR)/test_ty_math
TY_TEST_ASAN = $(TY_DIR)/test_ty_math_asan
TY_TOOL = $(TY_DIR)/turing-yield
TY_TOOL_ASAN = $(TY_DIR)/turing-yield_asan
TY_FIX_A = tests/turing/fixtures/ctr1_seed1_crumbs05-07.ctr
TY_FIX_B = tests/turing/fixtures/ctr1_seed1_crumbs10-12.ctr

$(TY_TEST): tests/turing/test_ty_math.c $(TY_SRCS) $(TY_HDRS)
	@mkdir -p $(TY_DIR)
	$(CC) $(TY_CFLAGS) -o $@ tests/turing/test_ty_math.c $(TY_SRCS) -lm

$(TY_TEST_ASAN): tests/turing/test_ty_math.c $(TY_SRCS) $(TY_HDRS)
	@mkdir -p $(TY_DIR)
	$(CC) $(TY_CFLAGS) $(TY_ASAN) -o $@ tests/turing/test_ty_math.c $(TY_SRCS) -lm

$(TY_TOOL): tools/turing_yield.c $(TY_SRCS) $(TY_HDRS)
	@mkdir -p $(TY_DIR)
	$(CC) $(TY_CFLAGS) -o $@ tools/turing_yield.c $(TY_SRCS) -lm

$(TY_TOOL_ASAN): tools/turing_yield.c $(TY_SRCS) $(TY_HDRS)
	@mkdir -p $(TY_DIR)
	$(CC) $(TY_CFLAGS) $(TY_ASAN) -o $@ tools/turing_yield.c $(TY_SRCS) -lm

turing-yield: $(TY_TOOL)

test-turing-yield: $(TY_TEST) $(TY_TEST_ASAN) $(TY_TOOL) $(TY_TOOL_ASAN)
	./$(TY_TEST) $(TY_DIR)/plain
	./$(TY_TEST_ASAN) $(TY_DIR)/asan
	cmp $(TY_DIR)/plain.det $(TY_DIR)/asan.det
	./$(TY_TOOL) fixture $(TY_FIX_A) $(TY_FIX_B) > $(TY_DIR)/fixture_plain.txt
	./$(TY_TOOL_ASAN) fixture $(TY_FIX_A) $(TY_FIX_B) > $(TY_DIR)/fixture_asan.txt
	cmp $(TY_DIR)/fixture_plain.txt $(TY_DIR)/fixture_asan.txt
	@echo "test-turing-yield: T arithmetic, records and fixture scores identical across plain and ASan/UBSan builds"
