# PD-0 learner v2 (adaptive-intelligence milestone 3, gap G3). Copied from mk/physics0-learner.mk; the seal-1
# targets and the frozen learner/ladder/score/controls trees are untouched.
#   test-physics0-learner-v2   isolation of the v2 learner objects + unit tests (plain + ASan/UBSan) + dev-driver smoke
#   physics0-learner-v2-dev    C1: hidden-state flag and prediction quality, v1 baseline vs v2, public dev seeds
#   physics0-regime-dev        C2: regime switch between episodes, FIXED vs UPDATING vs FRESH learner
#   physics0-prior-dev         C3: wrong / stale prior injected into the updating learner
#   physics0-shift-dev         v2.1 C2: change detection + bounded forgetting; arms fixed / updating-retain / updating-v2.1 (and fresh) after a regime switch, plus a no-switch false-alarm run
#   physics0-prior21-dev       v2.1 C3: wrong / stale prior, v2 arm then v2.1 arm
# Receipts (text, deterministic, seeded) are written to tests/physics0/learner_v2/receipts/ (new files only for a new commit).
ifndef PHYSICS0_LEARNER_V2_MK
PHYSICS0_LEARNER_V2_MK := 1
.PHONY: test-physics0-learner-v2 physics0-learner-v2-dev physics0-regime-dev physics0-prior-dev p0l2-isolation physics0-shift-dev physics0-prior21-dev
P0L2_DIR = $(OUT_DIR)/physics0-learner-v2
P0L2_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -Wno-misleading-indentation -O2 -D_POSIX_C_SOURCE=200809L -ffp-contract=off -fno-fast-math \
  -Isrc -Isrc/physics0/ladder -Isrc/physics0/score -Isrc/physics0/controls -Isrc/physics0/learner_v2
P0L2_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
P0L2_LEARNER_SRCS = src/physics0/learner_v2/pd0_learner.c src/physics0/ladder/pd0_fmt.c
P0L2_HDRS = src/physics0/learner_v2/pd0_learner.h src/physics0/ladder/pd0_fmt.h src/physics0/ladder/pd0_rng.h src/physics0/ladder/pd0_ladder.h src/physics0/ladder/pd0_codes.h src/physics0/score/pd0_score.h src/physics0/controls/pd0_controls.h
# world side of the dev drivers: the generator, world and regime switch, partially linked and with every symbol but four made local,
# so the verifier-side byte formats (pd0_fmt.c) and the substrate's own pd0_wire.c can coexist in one process image
P0L2_WORLD_SRCS = src/physics0/pd0_gen.c src/physics0/pd0_world.c src/physics0/pd0_guard.c src/physics0/pd0_wire.c src/physics0/pd0_rng.c src/physics0/pd0_relation.c src/physics0/pd0_chan.c src/physics0/regime/pd0_regime.c src/sha256.c
P0L2_WORLD_OBJS = $(patsubst %.c,$(P0L2_DIR)/world/%.o,$(notdir $(P0L2_WORLD_SRCS))) $(P0L2_DIR)/world/pd0_dev_world.o
P0L2_REC = tests/physics0/learner_v2/receipts

$(P0L2_DIR)/obj/pd0_learner.o: src/physics0/learner_v2/pd0_learner.c $(P0L2_HDRS)
	@mkdir -p $(P0L2_DIR)/obj
	$(CC) $(P0L2_CFLAGS) -c -o $@ $<
$(P0L2_DIR)/obj/pd0_fmt.o: src/physics0/ladder/pd0_fmt.c $(P0L2_HDRS)
	@mkdir -p $(P0L2_DIR)/obj
	$(CC) $(P0L2_CFLAGS) -c -o $@ $<
$(P0L2_DIR)/obj/pd0_gen.o: src/physics0/pd0_gen.c
	@mkdir -p $(P0L2_DIR)/obj
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Isrc -c -o $@ $<
p0l2-isolation: $(P0L2_DIR)/obj/pd0_learner.o $(P0L2_DIR)/obj/pd0_fmt.o $(P0L2_DIR)/obj/pd0_gen.o
	PD0_GEN_OBJ=$(P0L2_DIR)/obj/pd0_gen.o sh tests/physics0/isolation.sh $(P0L2_DIR)/obj/pd0_learner.o $(P0L2_DIR)/obj/pd0_fmt.o | tee $(P0L2_DIR)/isolation.out | tail -1; grep -q '^PHYSICS0_ISOLATION: PASS' $(P0L2_DIR)/isolation.out
	@if nm $(P0L2_DIR)/obj/pd0_learner.o $(P0L2_DIR)/obj/pd0_fmt.o | grep -E 'truth_|oracle|rx_|aienos_|argus_|forge_|mmap|fork|exec|socket|getenv|system|regime'; then echo "v2 learner object references forbidden symbol"; exit 1; fi
	@echo "p0l2-isolation: v2 learner objects carry no generator, world, regime, file, process or authority symbol"

$(P0L2_DIR)/test_pd0_learner2: tests/physics0/learner_v2/test_pd0_learner2.c $(P0L2_LEARNER_SRCS) src/physics0/ladder/pd0_ladder.c src/physics0/ladder/pd0_codes.c src/physics0/score/pd0_score.c src/physics0/controls/pd0_controls.c src/sha256.c $(P0L2_HDRS)
	@mkdir -p $(P0L2_DIR)
	$(CC) $(P0L2_CFLAGS) -o $@ $< $(P0L2_LEARNER_SRCS) src/physics0/ladder/pd0_codes.c src/physics0/ladder/pd0_ladder.c src/physics0/score/pd0_score.c src/physics0/controls/pd0_controls.c src/sha256.c -lm
$(P0L2_DIR)/test_pd0_learner2_asan: tests/physics0/learner_v2/test_pd0_learner2.c $(P0L2_LEARNER_SRCS) src/physics0/ladder/pd0_ladder.c src/physics0/ladder/pd0_codes.c src/physics0/score/pd0_score.c src/physics0/controls/pd0_controls.c src/sha256.c $(P0L2_HDRS)
	@mkdir -p $(P0L2_DIR)
	$(CC) $(P0L2_CFLAGS) $(P0L2_ASAN) -o $@ $< $(P0L2_LEARNER_SRCS) src/physics0/ladder/pd0_codes.c src/physics0/ladder/pd0_ladder.c src/physics0/score/pd0_score.c src/physics0/controls/pd0_controls.c src/sha256.c -lm

$(P0L2_DIR)/world/%.o: src/physics0/%.c $(wildcard src/physics0/*.h) src/physics0/regime/pd0_regime.h
	@mkdir -p $(P0L2_DIR)/world
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Isrc -c -o $@ $<
$(P0L2_DIR)/world/pd0_regime.o: src/physics0/regime/pd0_regime.c src/physics0/regime/pd0_regime.h $(wildcard src/physics0/*.h)
	@mkdir -p $(P0L2_DIR)/world
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Isrc -c -o $@ $<
$(P0L2_DIR)/world/sha256.o: src/sha256.c
	@mkdir -p $(P0L2_DIR)/world
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Isrc -c -o $@ $<
$(P0L2_DIR)/world/pd0_dev_world.o: tests/physics0/learner_v2/pd0_dev_world.c tests/physics0/learner_v2/pd0_dev_world.h src/physics0/regime/pd0_regime.h
	@mkdir -p $(P0L2_DIR)/world
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Isrc -Itests/physics0/learner_v2 -c -o $@ $<
$(P0L2_DIR)/world_sealed.o: $(P0L2_WORLD_OBJS)
	ld -r -o $(P0L2_DIR)/world_all.o $(P0L2_WORLD_OBJS)
	printf 'dev_world_open\ndev_world_call\ndev_world_switch\ndev_world_close\n' > $(P0L2_DIR)/keep.txt
	objcopy --keep-global-symbols=$(P0L2_DIR)/keep.txt $(P0L2_DIR)/world_all.o $@

P0L2_DRV_SRCS = tests/physics0/learner_v2/pd0_dev2.c src/physics0/ladder/pd0_fmt.c src/sha256.c $(P0L2_DIR)/world_sealed.o
$(P0L2_DIR)/pd0-dev2: $(P0L2_DRV_SRCS) src/physics0/learner_v2/pd0_learner.c $(P0L2_HDRS)
	$(CC) $(P0L2_CFLAGS) -Itests/physics0/learner_v2 -o $@ $(P0L2_DRV_SRCS) src/physics0/learner_v2/pd0_learner.c -lm
$(P0L2_DIR)/pd0-dev1: $(P0L2_DRV_SRCS) src/physics0/learner/pd0_learner.c src/physics0/learner/pd0_learner.h
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -Wno-misleading-indentation -O2 -D_POSIX_C_SOURCE=200809L -ffp-contract=off -fno-fast-math -Isrc -Isrc/physics0/ladder -Isrc/physics0/learner -Itests/physics0/learner_v2 -DDEV_V1 -o $@ $(P0L2_DRV_SRCS) src/physics0/learner/pd0_learner.c -lm

test-physics0-learner-v2: p0l2-isolation $(P0L2_DIR)/test_pd0_learner2 $(P0L2_DIR)/test_pd0_learner2_asan $(P0L2_DIR)/pd0-dev2
	./$(P0L2_DIR)/test_pd0_learner2 | tail -1 | grep -q '^PHYSICS0_LEARNER2_UNIT: PASS'
	./$(P0L2_DIR)/test_pd0_learner2_asan | tail -1 | grep -q '^PHYSICS0_LEARNER2_UNIT: PASS'
	./$(P0L2_DIR)/pd0-dev2 flag 1 1 > $(P0L2_DIR)/smoke.out
	grep -q '^P0V2F-SUM learner=v2 level=L6 seeds=1 hidden_fired=1' $(P0L2_DIR)/smoke.out
	./$(P0L2_DIR)/pd0-dev2 detect 1 1 > $(P0L2_DIR)/smoke-detect.out
	grep -q "^P0V21D-SUM level=L1 false_alarms=0/1 switch_detected=1/1" $(P0L2_DIR)/smoke-detect.out
	@echo "test-physics0-learner-v2: PASS (isolation, unit tests plain + ASan/UBSan, dev driver smoke)"

# C1: one line per level and seed for the seal-1 learner (baseline) and v2, then the acceptance check. Seeds 1-5 are the acceptance seeds; 6-25 are extra dev seeds.
physics0-learner-v2-dev: $(P0L2_DIR)/pd0-dev1 $(P0L2_DIR)/pd0-dev2
	@mkdir -p $(P0L2_REC)
	{ echo "receipt: PD0_LEARNER_V2_C1"; echo "commit: $$(git rev-parse HEAD)"; echo "kind: development evidence, public dev seeds only; not a sealed run, not scientific verification"; \
	  ./$(P0L2_DIR)/pd0-dev1 flag 1 25; ./$(P0L2_DIR)/pd0-dev2 flag 1 25; } > $(P0L2_DIR)/c1.txt
	bash tests/physics0/learner_v2/c1_check.sh $(P0L2_DIR)/c1.txt | tee -a $(P0L2_DIR)/c1.txt
	cp $(P0L2_DIR)/c1.txt $(P0L2_REC)/c1-$$(git rev-parse --short=12 HEAD).txt
physics0-regime-dev: $(P0L2_DIR)/pd0-dev2
	@mkdir -p $(P0L2_REC)
	{ echo "receipt: PD0_LEARNER_V2_C2"; echo "commit: $$(git rev-parse HEAD)"; echo "kind: development evidence, public dev seeds only; not a sealed run, not scientific verification"; ./$(P0L2_DIR)/pd0-dev2 regime 1 5; } > $(P0L2_DIR)/c2.txt
	cp $(P0L2_DIR)/c2.txt $(P0L2_REC)/c2-$$(git rev-parse --short=12 HEAD).txt
physics0-prior-dev: $(P0L2_DIR)/pd0-dev2
	@mkdir -p $(P0L2_REC)
	{ echo "receipt: PD0_LEARNER_V2_C3"; echo "commit: $$(git rev-parse HEAD)"; echo "kind: development evidence, public dev seeds only; not a sealed run, not scientific verification"; ./$(P0L2_DIR)/pd0-dev2 prior 1 5; } > $(P0L2_DIR)/c3.txt
	cp $(P0L2_DIR)/c3.txt $(P0L2_REC)/c3-$$(git rev-parse --short=12 HEAD).txt

physics0-shift-dev: $(P0L2_DIR)/pd0-dev2
	@mkdir -p $(P0L2_REC)
	{ echo "receipt: PD0_LEARNER_V21_SHIFT"; echo "commit: $$(git rev-parse HEAD)"; echo "kind: development evidence, public dev seeds 1-25 (detector thresholds tuned on seeds 101-150 and 201-250); not a sealed run, not scientific verification"; ./$(P0L2_DIR)/pd0-dev2 shift 1 25; } > $(P0L2_DIR)/shift.txt
	cp $(P0L2_DIR)/shift.txt $(P0L2_REC)/c4-shift-$$(git rev-parse --short=12 HEAD).txt
physics0-prior21-dev: $(P0L2_DIR)/pd0-dev2
	@mkdir -p $(P0L2_REC)
	{ echo "receipt: PD0_LEARNER_V21_PRIOR"; echo "commit: $$(git rev-parse HEAD)"; echo "kind: development evidence, public dev seeds 1-25; not a sealed run, not scientific verification"; ./$(P0L2_DIR)/pd0-dev2 prior21 1 25; } > $(P0L2_DIR)/prior21.txt
	cp $(P0L2_DIR)/prior21.txt $(P0L2_REC)/c5-prior21-$$(git rev-parse --short=12 HEAD).txt
endif
