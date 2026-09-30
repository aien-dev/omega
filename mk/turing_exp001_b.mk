# Turing calibration EXP-001, lane B: probability stream (TPS1/TSY1) + two
# reference coders (range, rANS). Spec: calibration/docs/CODER_SPEC.md.
# Picked up by `-include mk/*.mk`. All variables are TC_-prefixed.
#
# test-turing-exp001-coders: coder suite (built WITHOUT ty_model.c, so the
# coders cannot call the model), producer suite on the committed fixtures, CLI
# accept/refuse checks; each under plain and ASan/UBSan, determinism
# printouts compared byte for byte.
# turing-exp001-envelope TC_DATA=<dir with seed-N/control/trace.ctr>: overhead
# measurement on dev seeds 1-7 (not part of the test target; ~10 GB RAM peak).
.PHONY: test-turing-exp001-coders turing-exp001-envelope turing-coder
TC_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Isrc
TC_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
TC_CODER_SRCS = src/turing/tc_pstream.c src/turing/tc_range.c src/turing/tc_rans.c src/turing/ty_math.c src/sha256.c
TC_CODER_HDRS = src/turing/tc_pstream.h src/turing/tc_range.h src/turing/tc_rans.h src/turing/ty_math.h src/sha256.h
TC_PROD_SRCS = $(TC_CODER_SRCS) src/turing/tc_produce.c src/turing/ty_model.c src/turing/ty_ctr1.c
TC_PROD_HDRS = $(TC_CODER_HDRS) src/turing/tc_produce.h src/turing/ty_model.h src/turing/ty_ctr1.h
TC_DIR = $(OUT_DIR)/tests-turing-exp001-b
TC_TOOL = $(TC_DIR)/turing-coder
TC_TOOL_ASAN = $(TC_DIR)/turing-coder_asan
TC_FIX = tests/turing/fixtures/ctr1_seed1_crumbs05-07.ctr
TC_CAND = evidence/TURING_YIELD/ty2_candidate.tym
TC_ZERO = 0000000000000000000000000000000000000000000000000000000000000000
TC_DATA ?= $(HOME)/aien-data/crumbline/exp-20260927-rep10

$(TC_DIR)/test_tc: tests/turing/test_tc.c $(TC_CODER_SRCS) $(TC_CODER_HDRS)
	@mkdir -p $(TC_DIR)
	$(CC) $(TC_CFLAGS) -o $@ tests/turing/test_tc.c $(TC_CODER_SRCS) -lm
$(TC_DIR)/test_tc_asan: tests/turing/test_tc.c $(TC_CODER_SRCS) $(TC_CODER_HDRS)
	@mkdir -p $(TC_DIR)
	$(CC) $(TC_CFLAGS) $(TC_ASAN) -o $@ tests/turing/test_tc.c $(TC_CODER_SRCS) -lm
$(TC_DIR)/test_tc_produce: tests/turing/test_tc_produce.c $(TC_PROD_SRCS) $(TC_PROD_HDRS)
	@mkdir -p $(TC_DIR)
	$(CC) $(TC_CFLAGS) -o $@ tests/turing/test_tc_produce.c $(TC_PROD_SRCS) -lm
$(TC_DIR)/test_tc_produce_asan: tests/turing/test_tc_produce.c $(TC_PROD_SRCS) $(TC_PROD_HDRS)
	@mkdir -p $(TC_DIR)
	$(CC) $(TC_CFLAGS) $(TC_ASAN) -o $@ tests/turing/test_tc_produce.c $(TC_PROD_SRCS) -lm
$(TC_TOOL): src/turing/tc_tool.c $(TC_PROD_SRCS) $(TC_PROD_HDRS)
	@mkdir -p $(TC_DIR)
	$(CC) $(TC_CFLAGS) -o $@ src/turing/tc_tool.c $(TC_PROD_SRCS) -lm
$(TC_TOOL_ASAN): src/turing/tc_tool.c $(TC_PROD_SRCS) $(TC_PROD_HDRS)
	@mkdir -p $(TC_DIR)
	$(CC) $(TC_CFLAGS) $(TC_ASAN) -o $@ src/turing/tc_tool.c $(TC_PROD_SRCS) -lm

turing-coder: $(TC_TOOL)

test-turing-exp001-coders: $(TC_DIR)/test_tc $(TC_DIR)/test_tc_asan $(TC_DIR)/test_tc_produce \
		$(TC_DIR)/test_tc_produce_asan $(TC_TOOL) $(TC_TOOL_ASAN)
	@if grep -n '#include.*ty_model\|ty_model_' src/turing/tc_pstream.[ch] src/turing/tc_range.[ch] src/turing/tc_rans.[ch]; then \
		echo "coder sources reference the model"; exit 1; fi
	./$(TC_DIR)/test_tc $(TC_DIR)/coders_plain
	./$(TC_DIR)/test_tc_asan $(TC_DIR)/coders_asan
	cmp $(TC_DIR)/coders_plain.det $(TC_DIR)/coders_asan.det
	./$(TC_DIR)/test_tc_produce $(TC_DIR)/produce_plain
	./$(TC_DIR)/test_tc_produce_asan $(TC_DIR)/produce_asan
	cmp $(TC_DIR)/produce_plain.det $(TC_DIR)/produce_asan.det
	@set -e; for T in $(TC_TOOL) $(TC_TOOL_ASAN); do \
		D=$(TC_DIR)/cli_$$(basename $$T); mkdir -p $$D; \
		./$$T pstream $(TC_CAND) $(TC_ZERO) $(TC_FIX) $$D/f.tps $$D/f.tsy > $$D/pstream.txt; \
		M=$$(sed -n 's/^model_digest=//p' $$D/pstream.txt); \
		for C in range rans; do \
			./$$T encode $$C $$D/f.tps $$D/f.tsy $$D/f.$$C > /dev/null; \
			./$$T verify $$C $$D/f.tps $$D/f.tsy $$D/f.$$C --profile $(TC_ZERO) --model $$M > $$D/verify_$$C.txt; \
			./$$T decode $$C $$D/f.tps $$D/f.$$C $$D/dec_$$C.tsy; cmp $$D/f.tsy $$D/dec_$$C.tsy; \
			if ./$$T verify $$C $$D/f.tps $$D/f.tsy $$D/f.$$C --model $(TC_ZERO) 2>/dev/null; then \
				echo "CLI accepted a wrong model digest"; exit 1; fi; \
			head -c -1 $$D/f.$$C > $$D/trunc.$$C; \
			if ./$$T decode $$C $$D/f.tps $$D/trunc.$$C $$D/x.tsy 2>/dev/null; then \
				echo "CLI accepted a truncated bitstream"; exit 1; fi; \
		done; \
	done
	cmp $(TC_DIR)/cli_turing-coder/f.range $(TC_DIR)/cli_turing-coder_asan/f.range
	cmp $(TC_DIR)/cli_turing-coder/f.rans $(TC_DIR)/cli_turing-coder_asan/f.rans
	@echo "test-turing-exp001-coders: TPS1/TSY1, range + rANS round trips and refusals pass; plain and ASan/UBSan identical"

# All seven EXP-001 candidates (profile section "envelope"), per file and per crumb, both coders.
# One model at a time (M_mem needs several GB): never run in parallel.
TC_ENV_CANDS = B0_uniform B1_order0 B2_order1 B3_heuristic M_candidate M_mem M_mem_seed1
TC_ENV_CDIRS ?= calibration/experiments/EXP-001/candidates $(HOME)/aien-data/turing-cal/candidates
turing-exp001-envelope: $(TC_TOOL)
	@mkdir -p $(TC_DIR)/envelope
	@for S in 1 2 3 4 5 6 7; do for C in $(TC_ENV_CANDS); do \
		F=""; for D in $(TC_ENV_CDIRS); do [ -f "$$D/$$C.tym" ] && { F="$$D/$$C.tym"; break; }; done; \
		[ -n "$$F" ] || { echo "envelope: $$C.tym not found"; exit 1; }; \
		./$(TC_TOOL) envelope $(TC_ZERO) $(TC_DIR)/envelope/seed-$$S.$$C.csv $(TC_DATA)/seed-$$S/control/trace.ctr \
			"$$F" > $(TC_DIR)/envelope/seed-$$S.$$C.txt || { echo "envelope: seed $$S $$C refused"; exit 1; }; \
		test $$(grep -c "^file " $(TC_DIR)/envelope/seed-$$S.$$C.txt) -eq 1 || { echo "envelope: seed $$S $$C incomplete"; exit 1; }; \
	done; done
	sh calibration/scripts/envelope_summary.sh $(TC_DIR)/envelope | tee $(TC_DIR)/envelope/summary.txt
