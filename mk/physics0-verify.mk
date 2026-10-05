# PD-0 independent verifier (Direction 3): ladder checker, scorer, negative
# controls, append-only receipts. Standalone under src/physics0/{ladder,score,
# controls} and tests/physics0/verify. Not part of `all` or `test`.
# Test-only truth generators (tests/physics0/verify/truth.h) copy the spec
# equations; the verifier objects never see them (p0v-purity checks it).
#
# test-physics0-verify   formats + ladder + scorer/controls, plain and ASan/UBSan
ifndef PHYSICS0_VERIFY_MK
PHYSICS0_VERIFY_MK := 1
.PHONY: test-physics0-verify p0v-purity
P0V_COMMIT := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
P0V_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -Wno-misleading-indentation -O2 -D_POSIX_C_SOURCE=200809L -ffp-contract=off -fno-fast-math \
  -Isrc -Isrc/physics0/ladder -Isrc/physics0/score -Isrc/physics0/controls -Itests/physics0/verify -DPD0_VERIFIER_COMMIT=\"$(P0V_COMMIT)\" -DGOLDEN_PATH=\"tests/physics0/verify/golden_pd0rec1_pr243.bin\"
P0V_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
P0V_SRCS = src/physics0/ladder/pd0_fmt.c src/physics0/ladder/pd0_codes.c src/physics0/ladder/pd0_ladder.c src/physics0/score/pd0_score.c src/physics0/controls/pd0_controls.c src/physics0/controls/pd0_receipt.c src/sha256.c
P0V_HDRS = src/physics0/ladder/pd0_fmt.h src/physics0/ladder/pd0_codes.h src/physics0/ladder/pd0_rng.h src/physics0/ladder/pd0_ladder.h src/physics0/score/pd0_score.h src/physics0/controls/pd0_controls.h src/physics0/controls/pd0_receipt.h tests/physics0/verify/truth.h tests/physics0/verify/bundle.h
P0V_DIR = $(OUT_DIR)/tests-physics0-verify
# the verifier must not contain generator knowledge: no symbol or string from the test truth header
P0V_FORBIDDEN = 'truth_|oracle_rel|rx_|aienos_|argus_|forge_|mmap|fork|exec|socket'
$(P0V_DIR)/%: tests/physics0/verify/%.c $(P0V_SRCS) $(P0V_HDRS)
	@mkdir -p $(P0V_DIR)/receipts
	$(CC) $(P0V_CFLAGS) -o $@ $< $(P0V_SRCS) -lm
$(P0V_DIR)/%_asan: tests/physics0/verify/%.c $(P0V_SRCS) $(P0V_HDRS)
	@mkdir -p $(P0V_DIR)/receipts
	$(CC) $(P0V_CFLAGS) $(P0V_ASAN) -o $@ $< $(P0V_SRCS) -lm
p0v-purity:
	@mkdir -p $(P0V_DIR)/purity
	@for f in $(filter-out src/sha256.c,$(P0V_SRCS)); do $(CC) $(P0V_CFLAGS) -c -o $(P0V_DIR)/purity/$$(basename $$f .c).o $$f || exit 1; done
	@if nm $(P0V_DIR)/purity/*.o | grep -E $(P0V_FORBIDDEN); then echo "verifier object references generator or authority symbols"; exit 1; fi
	@echo "p0v-purity: verifier objects carry no generator or authority symbol"
test-physics0-verify: p0v-purity $(P0V_DIR)/test_pd0_fmt $(P0V_DIR)/test_pd0_fmt_asan $(P0V_DIR)/test_pd0_ladder $(P0V_DIR)/test_pd0_ladder_asan $(P0V_DIR)/test_pd0_score $(P0V_DIR)/test_pd0_score_asan
	./$(P0V_DIR)/test_pd0_fmt
	./$(P0V_DIR)/test_pd0_fmt_asan
	./$(P0V_DIR)/test_pd0_ladder
	./$(P0V_DIR)/test_pd0_ladder_asan
	./$(P0V_DIR)/test_pd0_score
	./$(P0V_DIR)/test_pd0_score_asan
	@echo "test-physics0-verify: PD-0 verifier formats, ladder, scorer and negative controls pass (plain + ASan/UBSan); receipts in $(P0V_DIR)/receipts"
endif
