# Omega Visor V1, lane 6 (World view). Test-only runtime link.
# The runtime core (rx_caproot/rx_world/rx_coherent + sha256) includes no physics
# header; only omega_evidence.c does, and it is NOT linked here. The recipe still
# uses the Makefile's $(CFLAGS) (same flags as RX_TEST) so the build matches the
# other runtime tests; run with PHYSICS_DIR=<physics checkout>.
# src/visor/rx/ is outside the src/visor/*.c wildcard, so build/omega never links
# visor_world_rx.c or any runtime object.
VISOR_WORLD_RX_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/sha256.c src/visor/rx/visor_world_rx.c tests/visor/test_visor_world.c
VISOR_WORLD_TEST = $(OUT_DIR)/tests-visor-world/test_visor_world

$(OUT_DIR)/tests-visor-world:
	mkdir -p $@

$(VISOR_WORLD_TEST): $(VISOR_WORLD_RX_SRCS) $(OUT_DIR)/visor/visor_world.o src/visor/visor_world.h \
		src/runtime/rx_world.h src/runtime/rx_caproot.h | $(OUT_DIR)/tests-visor-world
	$(CC) $(CFLAGS) -Isrc/visor -pthread -o $@ $(VISOR_WORLD_RX_SRCS) $(OUT_DIR)/visor/visor_world.o

.PHONY: test-visor-world
test-visor-world: $(VISOR_WORLD_TEST)
	./$(VISOR_WORLD_TEST)
