# Turing calibration EXP-001 lane C: sealed-data tooling.
# calibration/docs/BLINDING_PROTOCOL.md. Picked up by `-include mk/*.mk`.
# New files only: tools/turing_cal_overlap.c, calibration/scripts/*.sh.
#
# turing-cal-overlap: dev vs sealed CTR1 overlap audit helper (C; only
#   src/sha256.c + the ty_ctr1.h record size).
# test-turing-cal-blinding: proves the candidate jail cannot read the sealed
#   root and the evaluator jail cannot write candidate artifacts or reach the
#   network (bubblewrap; no sudo).
.PHONY: turing-cal-overlap test-turing-cal-blinding
TCO_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Isrc
TCO_DIR = $(OUT_DIR)/turing-cal
TCO_TOOL = $(TCO_DIR)/turing-cal-overlap
TCO_SRCS = tools/turing_cal_overlap.c src/sha256.c

$(TCO_TOOL): $(TCO_SRCS) src/turing/ty_ctr1.h src/sha256.h
	@mkdir -p $(TCO_DIR)
	$(CC) $(TCO_CFLAGS) -o $@ $(TCO_SRCS)

turing-cal-overlap: $(TCO_TOOL)

test-turing-cal-blinding:
	./calibration/scripts/test_blinding.sh
