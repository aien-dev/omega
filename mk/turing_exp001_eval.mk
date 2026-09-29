# Turing calibration EXP-001: the primary evaluator turing-cal-eval.
# Spec: calibration/docs/EVALUATOR.md. Picked up by `-include mk/*.mk`. Variables are TE_-prefixed.
#
# turing-cal-eval: build the evaluator (build/turing-exp001-eval/turing-cal-eval).
# test-turing-exp001-eval: fail-closed tests on the small committed fixture (seconds, low memory)
#   plus a small dry run end to end (run + gate) on the same fixture.
# turing-exp001-eval-dry TE_DATA=<dev run dir>: the full dev dry run (group 1 = seed 7, group 2 = seed 6,
#   all 7 candidates, ~few GB RAM, writes only under build/turing-exp001-eval/dry).
.PHONY: turing-cal-eval test-turing-exp001-eval turing-exp001-eval-dry
TE_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Wno-format-truncation -Isrc
TE_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
TE_SRCS = src/turing/tc_pstream.c src/turing/tc_range.c src/turing/tc_rans.c src/turing/ty_math.c src/sha256.c \
	src/turing/tc_produce.c src/turing/ty_model.c src/turing/ty_ctr1.c
TE_HDRS = src/turing/tc_pstream.h src/turing/tc_range.h src/turing/tc_rans.h src/turing/ty_math.h src/sha256.h \
	src/turing/tc_produce.h src/turing/ty_model.h src/turing/ty_ctr1.h
TE_DIR = $(OUT_DIR)/turing-exp001-eval
TE_BIN = $(TE_DIR)/turing-cal-eval
TE_BIN_ASAN = $(TE_DIR)/turing-cal-eval_asan
TE_DATA ?= $(HOME)/aien-data/crumbline/exp-20260927-rep10
TE_BIG ?= $(HOME)/aien-data/turing-cal/candidates

$(TE_BIN): tools/turing_cal_eval.c $(TE_SRCS) $(TE_HDRS)
	@mkdir -p $(TE_DIR)
	$(CC) $(TE_CFLAGS) -o $@ tools/turing_cal_eval.c $(TE_SRCS) -lm
$(TE_BIN_ASAN): tools/turing_cal_eval.c $(TE_SRCS) $(TE_HDRS)
	@mkdir -p $(TE_DIR)
	$(CC) $(TE_CFLAGS) $(TE_ASAN) -o $@ tools/turing_cal_eval.c $(TE_SRCS) -lm

turing-cal-eval: $(TE_BIN)

test-turing-exp001-eval: $(TE_BIN) $(TE_BIN_ASAN)
	bash tests/turing/test_tc_eval.sh $(TE_BIN) $(TE_DIR)/t_plain
	bash tests/turing/test_tc_eval.sh $(TE_BIN_ASAN) $(TE_DIR)/t_asan
	cmp $(TE_DIR)/t_plain/det.txt $(TE_DIR)/t_asan/det.txt

turing-exp001-eval-dry: $(TE_BIN)
	@test ! -e $(HOME)/workspace/.spark-quiet || { echo ".spark-quiet is set; not starting a heavy run"; exit 1; }
	bash tests/turing/test_tc_eval_dry.sh $(TE_BIN) $(TE_DIR)/dry $(TE_DATA) $(TE_BIG)
