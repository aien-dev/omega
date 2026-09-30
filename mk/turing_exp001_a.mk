# EXP-001 lane A (calibration profile, candidate set, power simulation).
# Picked up by `-include mk/*.mk`. New files only; every variable is TXA_-prefixed
# and every target turing-exp001-a-* so no other mk/turing_exp001*.mk collides.
#
#   make turing-exp001-a-build        build the candidate tool and the power simulation
#   make turing-exp001-a-candidates   write the six TYM0 candidates (dev seeds 1-7) + print L(M), dev L(D|M)
#   make turing-exp001-a-power        run the power simulation (fit 1-6, score 7 per crumb)
#   make turing-exp001-a-check        profile TOML vs code constants + field list (pre-freeze mode)
#   make test-turing-exp001-a         build + check + B3/consts self-test (no dev data needed)
.PHONY: turing-exp001-a-build turing-exp001-a-candidates turing-exp001-a-power turing-exp001-a-check test-turing-exp001-a
TXA_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Isrc -Itools
TXA_SRCS = src/turing/ty_math.c src/turing/ty_ctr1.c src/turing/ty_model.c src/turing/ty_record.c \
	src/turing/ty_profile.c src/omega_canonical.c src/sha256.c
TXA_HDRS = src/turing/ty_math.h src/turing/ty_ctr1.h src/turing/ty_model.h src/turing/ty_record.h tools/turing_cal_dev.h
TXA_DIR = $(OUT_DIR)/turing-exp001-a
TXA_CAND = $(TXA_DIR)/turing-cal-candidates
TXA_POWER = $(TXA_DIR)/power-simulation
TXA_MANIFEST = evidence/TURING_YIELD/trace_manifest_rep10_control.sha256
TXA_DATA_ROOT ?= $(HOME)/aien-data/crumbline
TXA_CAND_OUT ?= $(TXA_DIR)/candidates

$(TXA_CAND): tools/turing_cal_candidates.c $(TXA_SRCS) $(TXA_HDRS)
	@mkdir -p $(TXA_DIR)
	$(CC) $(TXA_CFLAGS) -o $@ tools/turing_cal_candidates.c $(TXA_SRCS) -lm

$(TXA_POWER): calibration/scripts/power_simulation.c $(TXA_SRCS) $(TXA_HDRS)
	@mkdir -p $(TXA_DIR)
	$(CC) $(TXA_CFLAGS) -o $@ calibration/scripts/power_simulation.c $(TXA_SRCS) -lm

turing-exp001-a-build: $(TXA_CAND) $(TXA_POWER)

turing-exp001-a-candidates: $(TXA_CAND)
	@mkdir -p $(TXA_CAND_OUT)
	./$(TXA_CAND) write $(TXA_MANIFEST) $(TXA_DATA_ROOT) evidence/TURING_YIELD $(TXA_CAND_OUT)

turing-exp001-a-power: $(TXA_POWER)
	./$(TXA_POWER) $(TXA_MANIFEST) $(TXA_DATA_ROOT)

turing-exp001-a-check: $(TXA_CAND)
	TXA_CAND=./$(TXA_CAND) sh calibration/scripts/check_profile.sh

test-turing-exp001-a: turing-exp001-a-build turing-exp001-a-check
	@echo "test-turing-exp001-a: tools build clean, profile TOML agrees with code constants"
