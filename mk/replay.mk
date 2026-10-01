# Lane LD (hardening): independent transcript verifier + World replayer.
# New files only: tools/replay/, tests/replay/, this file. Host only, CPU,
# no GPU. Picked up by the shared Makefile's `-include mk/*.mk`; also runs
# alone with `make -f mk/replay.mk test-replay`.
#
# test-replay      build, then tests/replay/run_replay_suite.sh: record a
#                  World run, replay it from its log (1 and 4 workers),
#                  negative controls, the mutation suites (World crumb log
#                  and M22 dispatch.log), host-labelled receipt
# replay-asan      the same suite with ASan/UBSan builds
# replay-purity    the verifier links nothing from src/runtime or src/train
#
# The verifier (rx_replay) is built from tools/replay/rx_replay.c,
# tools/replay/rxlog.c and src/sha256.c only.
ifndef REPLAY_MK
REPLAY_MK := 1
CC ?= gcc
OUT_DIR ?= build
.PHONY: test-replay replay-asan replay-purity replay-tools
REPLAY_DIR = $(OUT_DIR)/replay
# TRN1 shared corpus (aien-protocols specs/execution-transcript/vectors); empty = NOT_RUN
TRN1_VECTORS ?=
REPLAY_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc -Itools
REPLAY_SAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
REPLAY_VERIFIER_SRCS = tools/replay/rx_replay.c tools/replay/rxlog.c tools/replay/trn1.c src/sha256.c
REPLAY_MUTATE_SRCS = tools/replay/rxlog_mutate.c tools/replay/rxlog.c src/sha256.c
REPLAY_WORLD_SRCS = tests/replay/rx_world_replay.c tools/replay/rx_crumb_export.c tools/replay/rxlog.c \
	src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c src/sha256.c src/omega_evidence.c
REPLAY_M22_SRCS = tests/replay/m22_dispatch_run.c src/train/tg_store.c src/train/tg_sgd.c src/sha256.c
REPLAY_HDRS = tools/replay/rxlog.h tools/replay/trn1.h tools/replay/rx_crumb_export.h src/runtime/rx_world.h \
	src/runtime/rx_caproot.h src/train/tg_store.h src/train/tg_sgd.h src/sha256.h

# $(1) = output suffix, $(2) = extra flags
define REPLAY_BUILD
$(REPLAY_DIR)/rx_replay$(1): $(REPLAY_VERIFIER_SRCS) $(REPLAY_HDRS)
	@mkdir -p $(REPLAY_DIR)
	$(CC) $(REPLAY_CFLAGS) $(2) -o $$@ $(REPLAY_VERIFIER_SRCS)
$(REPLAY_DIR)/rxlog_mutate$(1): $(REPLAY_MUTATE_SRCS) $(REPLAY_HDRS)
	@mkdir -p $(REPLAY_DIR)
	$(CC) $(REPLAY_CFLAGS) $(2) -o $$@ $(REPLAY_MUTATE_SRCS)
$(REPLAY_DIR)/rx_world_replay$(1): $(REPLAY_WORLD_SRCS) $(REPLAY_HDRS)
	@mkdir -p $(REPLAY_DIR)
	$(CC) $(REPLAY_CFLAGS) $(2) -pthread -o $$@ $(REPLAY_WORLD_SRCS)
$(REPLAY_DIR)/m22_dispatch_run$(1): $(REPLAY_M22_SRCS) $(REPLAY_HDRS)
	@mkdir -p $(REPLAY_DIR)
	$(CC) $(REPLAY_CFLAGS) -ffp-contract=off $(2) -o $$@ $(REPLAY_M22_SRCS) -lm
endef
$(eval $(call REPLAY_BUILD,,))
$(eval $(call REPLAY_BUILD,_asan,$(REPLAY_SAN)))

REPLAY_BINS = $(REPLAY_DIR)/rx_replay $(REPLAY_DIR)/rxlog_mutate $(REPLAY_DIR)/rx_world_replay \
	$(REPLAY_DIR)/m22_dispatch_run
replay-tools: $(REPLAY_BINS)

replay-purity:
	@bad=$$(grep -hE '^[[:space:]]*#[[:space:]]*include' tools/replay/rx_replay.c tools/replay/rxlog.c tools/replay/rxlog.h tools/replay/trn1.c tools/replay/trn1.h | \
		grep -vE '#[[:space:]]*include[[:space:]]+(<[a-z/]+\.h>|"sha256\.h"|"replay/(rxlog|trn1)\.h")' || true); \
	if [ -n "$$bad" ]; then echo "replay-purity: FAIL, verifier includes:"; echo "$$bad"; exit 1; fi; \
	echo "replay-purity: verifier depends only on libc, sha256, rxlog and trn1"

test-replay: replay-purity $(REPLAY_BINS)
	REPLAY_BIN=$(REPLAY_DIR) REPLAY_SUFFIX= TRN1_VECTORS=$(TRN1_VECTORS) sh tests/replay/run_replay_suite.sh $(REPLAY_DIR)/run

replay-asan: replay-purity $(REPLAY_BINS:%=%_asan)
	REPLAY_BIN=$(REPLAY_DIR) REPLAY_SUFFIX=_asan TRN1_VECTORS=$(TRN1_VECTORS) sh tests/replay/run_replay_suite.sh $(REPLAY_DIR)/run-asan
endif
