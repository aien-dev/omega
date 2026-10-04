# PD-0 learner (Physics-0 Discovery Engine, Direction 4): src/physics0/learner,
# tests/physics0/learner. The learner links only the verifier's byte formats
# (ladder/pd0_fmt.c, header-only pd0_rng.h) and sha256; never the generators,
# the world, or any file I/O (isolation check below). The harness drives the
# world process and a test-only truth process (links the generators) over pipes.
#
# test-physics0-learner     isolation + unit tests (plain + ASan/UBSan)
# physics0-learner-dev      development run: levels L0..L6 + null, seeds 1..5
# physics0-learner-evidence copy new receipts into evidence/physics0/learner/ (cp -n)
# physics0-pd0b-run / physics0-pd0b-freeze  PD-0b harness against an external world (see the end of this file)
ifndef PHYSICS0_LEARNER_MK
PHYSICS0_LEARNER_MK := 1
.PHONY: test-physics0-learner physics0-learner-dev physics0-learner-evidence p0l-isolation
P0L_DIR = $(OUT_DIR)/physics0-learner
P0L_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -Wno-misleading-indentation -O2 -D_POSIX_C_SOURCE=200809L -ffp-contract=off -fno-fast-math \
  -Isrc -Isrc/physics0/ladder -Isrc/physics0/score -Isrc/physics0/controls -Isrc/physics0/learner
P0L_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
P0L_LEARNER_SRCS = src/physics0/learner/pd0_learner.c src/physics0/ladder/pd0_fmt.c
P0L_VERIFIER_SRCS = src/physics0/ladder/pd0_codes.c src/physics0/ladder/pd0_ladder.c src/physics0/score/pd0_score.c src/physics0/controls/pd0_controls.c
P0L_HDRS = src/physics0/learner/pd0_learner.h src/physics0/ladder/pd0_fmt.h src/physics0/ladder/pd0_rng.h src/physics0/ladder/pd0_ladder.h src/physics0/ladder/pd0_codes.h src/physics0/score/pd0_score.h src/physics0/controls/pd0_controls.h
# substrate side (generators): the truth process only
P0L_TRUTH_SRCS = src/physics0/pd0_chan.c src/physics0/pd0_wire.c src/physics0/pd0_rng.c src/physics0/pd0_relation.c src/physics0/pd0_gen.c src/physics0/pd0_world.c src/physics0/pd0_guard.c src/sha256.c
P0L_LEVELS ?= 0 1 2 3 4 5 6 null
P0L_SEEDS ?= 1 2 3 4 5

$(P0L_DIR)/obj/%.o: src/physics0/learner/%.c $(P0L_HDRS)
	@mkdir -p $(P0L_DIR)/obj
	$(CC) $(P0L_CFLAGS) -c -o $@ $<
$(P0L_DIR)/obj/pd0_fmt.o: src/physics0/ladder/pd0_fmt.c $(P0L_HDRS)
	@mkdir -p $(P0L_DIR)/obj
	$(CC) $(P0L_CFLAGS) -c -o $@ $<
$(P0L_DIR)/obj/pd0_gen.o: src/physics0/pd0_gen.c
	@mkdir -p $(P0L_DIR)/obj
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Isrc -c -o $@ $<
P0L_OBJS = $(P0L_DIR)/obj/pd0_learner.o $(P0L_DIR)/obj/pd0_learner_main.o $(P0L_DIR)/obj/pd0_fmt.o
# G5 isolation (spec I10) on the learner objects, plus: no truth/oracle symbol, no process or socket call
p0l-isolation: $(P0L_OBJS) $(P0L_DIR)/obj/pd0_gen.o
	PD0_GEN_OBJ=$(P0L_DIR)/obj/pd0_gen.o sh tests/physics0/isolation.sh $(P0L_OBJS) | tee $(P0L_DIR)/isolation.out | tail -1; grep -q '^PHYSICS0_ISOLATION: PASS' $(P0L_DIR)/isolation.out
	@if nm $(P0L_OBJS) | grep -E 'truth_|oracle|rx_|aienos_|argus_|forge_|mmap|fork|exec|socket|getenv|system'; then echo "learner object references forbidden symbol"; exit 1; fi
	@echo "p0l-isolation: learner objects carry no generator, world, file, process or authority symbol"

$(P0L_DIR)/pd0-learner: src/physics0/learner/pd0_learner_main.c $(P0L_LEARNER_SRCS) src/sha256.c $(P0L_HDRS)
	@mkdir -p $(P0L_DIR)
	$(CC) $(P0L_CFLAGS) -o $@ src/physics0/learner/pd0_learner_main.c $(P0L_LEARNER_SRCS) src/sha256.c -lm
$(P0L_DIR)/test_pd0_learner: tests/physics0/learner/test_pd0_learner.c $(P0L_LEARNER_SRCS) $(P0L_VERIFIER_SRCS) src/sha256.c $(P0L_HDRS)
	@mkdir -p $(P0L_DIR)
	$(CC) $(P0L_CFLAGS) -o $@ $< $(P0L_LEARNER_SRCS) $(P0L_VERIFIER_SRCS) src/sha256.c -lm
$(P0L_DIR)/test_pd0_learner_asan: tests/physics0/learner/test_pd0_learner.c $(P0L_LEARNER_SRCS) $(P0L_VERIFIER_SRCS) src/sha256.c $(P0L_HDRS)
	@mkdir -p $(P0L_DIR)
	$(CC) $(P0L_CFLAGS) $(P0L_ASAN) -o $@ $< $(P0L_LEARNER_SRCS) $(P0L_VERIFIER_SRCS) src/sha256.c -lm
$(P0L_DIR)/pd0-harness: tests/physics0/learner/pd0_harness.c $(P0L_LEARNER_SRCS) $(P0L_VERIFIER_SRCS) src/sha256.c $(P0L_HDRS)
	@mkdir -p $(P0L_DIR)
	$(CC) $(P0L_CFLAGS) -o $@ $< $(P0L_LEARNER_SRCS) $(P0L_VERIFIER_SRCS) src/sha256.c -lm
$(P0L_DIR)/pd0-truth: tests/physics0/learner/pd0_truth_main.c $(P0L_TRUTH_SRCS) $(wildcard src/physics0/*.h)
	@mkdir -p $(P0L_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Isrc -o $@ $< $(P0L_TRUTH_SRCS) -lm

test-physics0-learner: p0l-isolation $(P0L_DIR)/test_pd0_learner $(P0L_DIR)/test_pd0_learner_asan $(P0L_DIR)/pd0-learner
	./$(P0L_DIR)/test_pd0_learner
	./$(P0L_DIR)/test_pd0_learner_asan
	@echo "test-physics0-learner: PASS (isolation, unit tests plain + ASan/UBSan)"

# development run on the world process; results in $(P0L_DIR)/dev/ (one line per instance + ledger/law/json per instance)
physics0-learner-dev: $(P0L_DIR)/pd0-harness $(P0L_DIR)/pd0-truth $(OUT_DIR)/physics0/pd0-world
	@mkdir -p $(P0L_DIR)/dev
	@: > $(P0L_DIR)/dev/results.txt
	for l in $(P0L_LEVELS); do for s in $(P0L_SEEDS); do ./$(P0L_DIR)/pd0-harness $(OUT_DIR)/physics0/pd0-world $(P0L_DIR)/pd0-truth $$l $$s $(P0L_DIR)/dev | tee -a $(P0L_DIR)/dev/results.txt; done; done
	@echo "physics0-learner-dev: done, $$(wc -l < $(P0L_DIR)/dev/results.txt) instances in $(P0L_DIR)/dev/results.txt"

# receipts are immutable: new files only (cp -n)
physics0-learner-evidence: physics0-learner-dev test-physics0-learner
	@mkdir -p evidence/physics0/learner
	{ echo "receipt: PD0_LEARNER_DEV"; echo "commit: $$(git rev-parse HEAD)"; echo "date_utc: $$(date -u +%Y-%m-%dT%H:%M:%SZ)"; echo "levels: $(P0L_LEVELS)"; echo "seeds: $(P0L_SEEDS) (development seeds; HOLDOUT/TRIAL/REP withheld by the harness)"; echo "--- isolation"; cat $(P0L_DIR)/isolation.out; echo "--- results"; cat $(P0L_DIR)/dev/results.txt; } > $(P0L_DIR)/dev/receipt.txt
	cp -n $(P0L_DIR)/dev/receipt.txt evidence/physics0/learner/pd0-learner-dev-$$(git rev-parse --short=12 HEAD).txt

# PD-0b run harness (docs/physics0/PD0_PROTOCOL_V2.md): the frozen learner against an EXTERNAL world binary.
# physics0-pd0b-run     PD0_WORLD_BIN=<world> PD0B_SEEDS=<file of "level_index seed", five per level>
#                       writes one NEW receipt evidence/physics0/pd0b/PD0B_RUN-<commit>-<world sha12>.txt (never overwrites)
# physics0-pd0b-freeze  prints the frozen-directory tree hashes, file hashes and harness digest (print only)
.PHONY: physics0-pd0b-run physics0-pd0b-freeze
physics0-pd0b-run: $(P0L_DIR)/pd0-harness
	@test -n "$(PD0_WORLD_BIN)" || { echo "physics0-pd0b-run: set PD0_WORLD_BIN=<world binary>"; exit 2; }
	@test -n "$(PD0B_SEEDS)" || { echo "physics0-pd0b-run: set PD0B_SEEDS=<seeds file>"; exit 2; }
	sh tests/physics0/learner/pd0b_run.sh $(P0L_DIR)/pd0-harness "$(PD0_WORLD_BIN)" "$(PD0B_SEEDS)" "$(PD0B_NOTE)"
physics0-pd0b-freeze: $(P0L_DIR)/pd0-harness
	@sh tests/physics0/learner/pd0b_freeze.sh $(P0L_DIR)/pd0-harness
endif
