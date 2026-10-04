# PD-0 hidden-equation benchmark substrate (Physics-0 Discovery Engine,
# spec aien-dev/physics docs/PD0_HIDDEN_EQUATION_BENCHMARK.md @ 5bd2b04, revision 3).
# Files under src/physics0/ and tests/physics0/. Picked up by `-include mk/*.mk`;
# not part of `all` or `test`. Direction 2 lane only: wire records, world
# process (generators), STAND_IN recorder and range guard, oracle + reference
# sparse solver (test-only), G0/G1 gates, Omega-language helpers. No ladder
# checker, scorer, controls or learner here (other lanes).
#
# physics0-test      G0 self-test (plain + ASan/UBSan), G5 isolation, G1 oracle
#                    calibration (V1 + V3 per level), mutant check, OSC differential
# physics0-evidence  copy the G0/G1 receipts into evidence/physics0/ (new names per cut, never overwritten)
# pd0-world          build/physics0/pd0-world <level|null> <seed>: the world process
ifndef PHYSICS0_MK
PHYSICS0_MK := 1
.PHONY: physics0-test physics0-evidence pd0-world
P0_DIR = $(OUT_DIR)/physics0
P0_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Isrc -Itests/physics0
P0_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
# learner-side (no generator, no file I/O): the G5 isolation check runs on these objects
P0_LEARNER_SRCS = src/physics0/pd0_chan.c src/physics0/pd0_wire.c src/physics0/pd0_rng.c src/physics0/pd0_relation.c
# world side
P0_WORLD_SRCS = src/physics0/pd0_gen.c src/physics0/pd0_world.c src/physics0/pd0_guard.c
P0_SRCS = $(P0_LEARNER_SRCS) $(P0_WORLD_SRCS) src/sha256.c
P0_HDRS = $(wildcard src/physics0/*.h) src/sha256.h
P0_CALIB = tests/physics0/pd0_calib.c tests/physics0/pd0_sparse.c
P0_CALIB_HDRS = tests/physics0/pd0_calib.h tests/physics0/pd0_sparse.h
P0_SPLITMIX_REF = src/turing/history_selector.c
# OSC differential: compiler sources exactly as mk/compiler.mk builds them
P0_OSC_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc -Isrc/compiler -Isrc/compiler/model
P0_OSC_LIB = $(filter-out src/compiler/oscc_main.c,$(wildcard src/compiler/*.c))

$(P0_DIR)/pd0-world: src/physics0/pd0_world_main.c $(P0_SRCS) $(P0_HDRS)
	@mkdir -p $(P0_DIR)
	$(CC) $(P0_CFLAGS) -o $@ src/physics0/pd0_world_main.c $(P0_SRCS) -lm

pd0-world: $(P0_DIR)/pd0-world

$(P0_DIR)/learner/%.o: src/physics0/%.c $(P0_HDRS)
	@mkdir -p $(P0_DIR)/learner
	$(CC) $(P0_CFLAGS) -c -o $@ $<
$(P0_DIR)/learner/pd0_gen.o: src/physics0/pd0_gen.c $(P0_HDRS)
	@mkdir -p $(P0_DIR)/learner
	$(CC) $(P0_CFLAGS) -c -o $@ $<
P0_LEARNER_OBJS = $(patsubst src/physics0/%.c,$(P0_DIR)/learner/%.o,$(P0_LEARNER_SRCS))

$(P0_DIR)/test_pd0_world: tests/physics0/test_pd0_world.c $(P0_SRCS) $(P0_SPLITMIX_REF) $(P0_HDRS)
	@mkdir -p $(P0_DIR)
	$(CC) $(P0_CFLAGS) -o $@ tests/physics0/test_pd0_world.c $(P0_SRCS) $(P0_SPLITMIX_REF) -lm
$(P0_DIR)/test_pd0_world_asan: tests/physics0/test_pd0_world.c $(P0_SRCS) $(P0_SPLITMIX_REF) $(P0_HDRS)
	@mkdir -p $(P0_DIR)
	$(CC) $(P0_CFLAGS) $(P0_ASAN) -o $@ tests/physics0/test_pd0_world.c $(P0_SRCS) $(P0_SPLITMIX_REF) -lm

$(P0_DIR)/pd0-oracle: tests/physics0/pd0_oracle.c $(P0_CALIB) $(P0_SRCS) $(P0_HDRS) $(P0_CALIB_HDRS)
	@mkdir -p $(P0_DIR)
	$(CC) $(P0_CFLAGS) -o $@ tests/physics0/pd0_oracle.c $(P0_CALIB) $(P0_SRCS) -lm
$(P0_DIR)/pd0-oracle_asan: tests/physics0/pd0_oracle.c $(P0_CALIB) $(P0_SRCS) $(P0_HDRS) $(P0_CALIB_HDRS)
	@mkdir -p $(P0_DIR)
	$(CC) $(P0_CFLAGS) $(P0_ASAN) -o $@ tests/physics0/pd0_oracle.c $(P0_CALIB) $(P0_SRCS) -lm
# NC-3 style mutant: wrong sign in the L1 generator; the oracle check must FAIL
$(P0_DIR)/pd0-oracle-mutant: tests/physics0/pd0_oracle.c $(P0_CALIB) $(P0_SRCS) $(P0_HDRS) $(P0_CALIB_HDRS)
	@mkdir -p $(P0_DIR)
	$(CC) $(P0_CFLAGS) -DPD0_MUTANT_SIGN -o $@ tests/physics0/pd0_oracle.c $(P0_CALIB) $(P0_SRCS) -lm

# wrong-latent-sign mutant: L6 generator uses -m*h; oracle_fit must FAIL (its fitted m*q leaves the declared range)
$(P0_DIR)/pd0-oracle-mutant-latent: tests/physics0/pd0_oracle.c $(P0_CALIB) $(P0_SRCS) $(P0_HDRS) $(P0_CALIB_HDRS)
	@mkdir -p $(P0_DIR)
	$(CC) $(P0_CFLAGS) -DPD0_MUTANT_LATENT_SIGN -o $@ tests/physics0/pd0_oracle.c $(P0_CALIB) $(P0_SRCS) -lm

$(P0_DIR)/test_pd0_osc: tests/physics0/test_pd0_osc.c $(P0_OSC_LIB) $(P0_LEARNER_SRCS) src/sha256.c $(P0_HDRS)
	@mkdir -p $(P0_DIR)
	$(CC) $(P0_OSC_CFLAGS) -o $@ tests/physics0/test_pd0_osc.c $(P0_OSC_LIB) $(P0_LEARNER_SRCS) src/sha256.c -lm
$(P0_DIR)/test_pd0_osc_asan: tests/physics0/test_pd0_osc.c $(P0_OSC_LIB) $(P0_LEARNER_SRCS) src/sha256.c $(P0_HDRS)
	@mkdir -p $(P0_DIR)
	$(CC) $(P0_OSC_CFLAGS) $(P0_ASAN) -o $@ tests/physics0/test_pd0_osc.c $(P0_OSC_LIB) $(P0_LEARNER_SRCS) src/sha256.c -lm

physics0-test: $(P0_DIR)/test_pd0_world $(P0_DIR)/test_pd0_world_asan $(P0_LEARNER_OBJS) $(P0_DIR)/learner/pd0_gen.o \
		$(P0_DIR)/pd0-oracle $(P0_DIR)/pd0-oracle_asan $(P0_DIR)/pd0-oracle-mutant $(P0_DIR)/pd0-oracle-mutant-latent \
		$(P0_DIR)/test_pd0_osc $(P0_DIR)/test_pd0_osc_asan $(P0_DIR)/pd0-world
	@mkdir -p $(P0_DIR)/receipts
	./$(P0_DIR)/test_pd0_world > $(P0_DIR)/g0.out; tail -1 $(P0_DIR)/g0.out; grep -q '^PHYSICS0_G0: PASS$$' $(P0_DIR)/g0.out
	./$(P0_DIR)/test_pd0_world_asan > $(P0_DIR)/g0_asan.out; tail -1 $(P0_DIR)/g0_asan.out; grep -q '^PHYSICS0_G0: PASS$$' $(P0_DIR)/g0_asan.out
	PD0_GEN_OBJ=$(P0_DIR)/learner/pd0_gen.o sh tests/physics0/isolation.sh $(P0_LEARNER_OBJS) | tee $(P0_DIR)/isolation.out | tail -1; grep -q '^PHYSICS0_ISOLATION: PASS' $(P0_DIR)/isolation.out
	./$(P0_DIR)/pd0-oracle all $(P0_DIR)/receipts > $(P0_DIR)/g1.out; cat $(P0_DIR)/g1.out; grep -q '^PHYSICS0_ORACLE: PASS$$' $(P0_DIR)/g1.out
	./$(P0_DIR)/pd0-oracle_asan 1 $(P0_DIR) -asan > $(P0_DIR)/g1_asan.out; tail -1 $(P0_DIR)/g1_asan.out; grep -q '^PHYSICS0_ORACLE-asan: PASS$$' $(P0_DIR)/g1_asan.out
	cmp $(P0_DIR)/receipts/pd0-oracle-L1.json $(P0_DIR)/pd0-oracle-L1-asan.json
	-./$(P0_DIR)/pd0-oracle-mutant 1 $(P0_DIR) -mutant > $(P0_DIR)/mutant.out
	@grep -q '^PHYSICS0_G1_V1_L1-mutant: FAIL' $(P0_DIR)/mutant.out && echo "PHYSICS0_MUTANT_SIGN: PASS (wrong generator sign is caught by the oracle check)" || { echo "PHYSICS0_MUTANT_SIGN: FAIL"; exit 1; }
	-./$(P0_DIR)/pd0-oracle-mutant-latent 6 $(P0_DIR) -mutlat > $(P0_DIR)/mutant_latent.out
	@grep -q "^PHYSICS0_G1_V1_L6-mutlat: FAIL (oracle_exact FAIL, oracle_fit FAIL)" $(P0_DIR)/mutant_latent.out && echo "PHYSICS0_MUTANT_LATENT_SIGN: PASS (wrong latent sign makes oracle_fit FAIL)" || { echo "PHYSICS0_MUTANT_LATENT_SIGN: FAIL"; exit 1; }
	./$(P0_DIR)/test_pd0_osc tests/physics0/osc/pd0_helpers.osc 20000 > $(P0_DIR)/osc.out; tail -2 $(P0_DIR)/osc.out; grep -q '^PHYSICS0_OSC_DIFF: PASS$$' $(P0_DIR)/osc.out
	./$(P0_DIR)/test_pd0_osc_asan tests/physics0/osc/pd0_helpers.osc 2000 > $(P0_DIR)/osc_asan.out; tail -1 $(P0_DIR)/osc_asan.out; grep -q '^PHYSICS0_OSC_DIFF: PASS$$' $(P0_DIR)/osc_asan.out
	@echo "physics0-test: PASS (G0, G5 isolation, G1 V1 oracle all levels, V3 breach rates recorded, mutant caught, OSC differential; substrate only, no learner)"

# Receipts are immutable: existing evidence files are never rewritten (cp -n); each cut adds new files.
physics0-evidence: physics0-test
	mkdir -p evidence/physics0
	for f in $(P0_DIR)/receipts/pd0-oracle-*.json; do b=$$(basename "$$f" .json); cp -n "$$f" "evidence/physics0/$$b-rev3b.json"; done
	{ echo "receipt: PD0_G0"; echo "spec: aien-dev/physics docs/PD0_HIDDEN_EQUATION_BENCHMARK.md @ 5bd2b04 (rev 3, PD0DESC2)"; echo "recorder: STAND_IN"; echo "range_guard: STAND_IN"; \
	  echo "commit: $$(git rev-parse HEAD)"; echo "date_utc: $$(date -u +%Y-%m-%dT%H:%M:%SZ)"; echo "--- test_pd0_world"; cat $(P0_DIR)/g0.out; \
	  echo "--- isolation"; cat $(P0_DIR)/isolation.out; echo "--- G1 oracle"; cat $(P0_DIR)/g1.out; echo "--- osc differential"; cat $(P0_DIR)/osc.out; \
	  echo "--- mutant"; cat $(P0_DIR)/mutant.out; echo "--- mutant latent sign"; cat $(P0_DIR)/mutant_latent.out; } > evidence/physics0/pd0-g0-receipt-cut3b.txt
endif
