# PD-0 experiment planner (Direction 5): contract tests, purity check, passive baseline
# and the information-efficiency comparison (development evidence, not EXP-003).
ifndef PHYSICS0_PLANNER_MK
PHYSICS0_PLANNER_MK := 1
.PHONY: test-physics0-planner p0p-purity physics0-planner-eff physics0-planner-eff2

P0P_COMMIT := $(shell git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
P0P_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -ffp-contract=off -fno-fast-math \
  -Isrc -Isrc/physics0/ladder -Isrc/physics0/planner -Itests/physics0/planner
P0P_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
# planner objects: verifier formats only, no generator, no world
P0P_SRCS = src/physics0/planner/pd0_planner.c src/physics0/ladder/pd0_fmt.c src/sha256.c
P0P_HDRS = src/physics0/planner/pd0_planner.h src/physics0/ladder/pd0_fmt.h src/physics0/ladder/pd0_rng.h
# bridge object: substrate formats, links the generator on purpose (harness side)
P0P_BRIDGE_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Isrc -Itests/physics0/planner
P0P_BRIDGE_SRCS = tests/physics0/planner/pd0p_bridge.c src/physics0/pd0_gen.c src/physics0/pd0_world.c src/physics0/pd0_guard.c \
  src/physics0/pd0_wire.c src/physics0/pd0_rng.c src/physics0/pd0_relation.c src/physics0/pd0_chan.c
P0P_DIR = $(OUT_DIR)/tests-physics0-planner
P0P_FORBIDDEN = 'pd0_gen|pd0_world|oracle|truth_|forge_|argus_|mmap|fork|exec|socket'

$(P0P_DIR)/test_pd0_planner: tests/physics0/planner/test_pd0_planner.c $(P0P_SRCS) $(P0P_HDRS)
	@mkdir -p $(P0P_DIR)
	$(CC) $(P0P_CFLAGS) -o $@ $< $(P0P_SRCS) -lm
$(P0P_DIR)/test_pd0_planner_asan: tests/physics0/planner/test_pd0_planner.c $(P0P_SRCS) $(P0P_HDRS)
	@mkdir -p $(P0P_DIR)
	$(CC) $(P0P_CFLAGS) $(P0P_ASAN) -o $@ $< $(P0P_SRCS) -lm

$(P0P_DIR)/bridge/%.o: tests/physics0/planner/%.c tests/physics0/planner/pd0p_bridge.h $(wildcard src/physics0/*.h)
	@mkdir -p $(P0P_DIR)/bridge
	$(CC) $(P0P_BRIDGE_CFLAGS) -c -o $@ $<
$(P0P_DIR)/bridge/%.o: src/physics0/%.c $(wildcard src/physics0/*.h)
	@mkdir -p $(P0P_DIR)/bridge
	$(CC) $(P0P_BRIDGE_CFLAGS) -c -o $@ $<
P0P_BRIDGE_OBJS = $(patsubst %.c,$(P0P_DIR)/bridge/%.o,$(notdir $(P0P_BRIDGE_SRCS)))

# the substrate and the verifier both define the LE helpers and pd0_mul; the bridge side is merged into one
# relocatable object and those seven symbols are renamed there so the two families link side by side
P0P_CLASH = pd0_put_u16 pd0_put_u32 pd0_put_u64 pd0_get_u16 pd0_get_u32 pd0_get_u64 pd0_mul
$(P0P_DIR)/bridge.o: $(P0P_BRIDGE_OBJS)
	$(LD) -r -o $@ $(P0P_BRIDGE_OBJS)
	objcopy $(foreach s,$(P0P_CLASH),--redefine-sym $(s)=pb_sub_$(s)) $@
$(P0P_DIR)/pd0_plan_eff: tests/physics0/planner/pd0_plan_eff.c $(P0P_SRCS) $(P0P_HDRS) $(P0P_DIR)/bridge.o
	@mkdir -p $(P0P_DIR)
	$(CC) $(P0P_CFLAGS) -o $@ $< $(P0P_SRCS) $(P0P_DIR)/bridge.o -lm
$(P0P_DIR)/pd0_plan_eff2: tests/physics0/planner/pd0_plan_eff2.c $(P0P_SRCS) $(P0P_HDRS) $(P0P_DIR)/bridge.o
	@mkdir -p $(P0P_DIR)
	$(CC) $(P0P_CFLAGS) -o $@ $< $(P0P_SRCS) $(P0P_DIR)/bridge.o -lm

p0p-purity:
	@mkdir -p $(P0P_DIR)/purity
	@$(CC) $(P0P_CFLAGS) -c -o $(P0P_DIR)/purity/pd0_planner.o src/physics0/planner/pd0_planner.c
	@if nm $(P0P_DIR)/purity/pd0_planner.o | grep -E $(P0P_FORBIDDEN); then echo "planner object references generator, world or authority symbols"; exit 1; fi
	@echo "p0p-purity: planner object carries no generator, world or authority symbol"

physics0-planner-eff: $(P0P_DIR)/pd0_plan_eff
	@rm -f $(P0P_DIR)/pd0-planner-eff.json
	./$(P0P_DIR)/pd0_plan_eff $(P0P_DIR)/pd0-planner-eff.json $(P0P_COMMIT) | tee $(P0P_DIR)/eff.out | tail -3
	@grep -q '^PD0_PLANNER_EFF: PASS$$' $(P0P_DIR)/eff.out

physics0-planner-eff2: $(P0P_DIR)/pd0_plan_eff2
	@rm -f $(P0P_DIR)/pd0-planner-eff2.json
	./$(P0P_DIR)/pd0_plan_eff2 $(P0P_DIR)/pd0-planner-eff2.json $(P0P_COMMIT) | tee $(P0P_DIR)/eff2.out | tail -6
	@grep -q '^PD0_PLANNER_EFF2: PASS' $(P0P_DIR)/eff2.out

test-physics0-planner: p0p-purity $(P0P_DIR)/test_pd0_planner $(P0P_DIR)/test_pd0_planner_asan physics0-planner-eff physics0-planner-eff2
	./$(P0P_DIR)/test_pd0_planner
	./$(P0P_DIR)/test_pd0_planner_asan
	@echo "test-physics0-planner: PD-0 planner contract (behaviours 1-6), mutants, passive baseline and efficiency comparison pass; receipt in $(P0P_DIR)/pd0-planner-eff.json"
endif
