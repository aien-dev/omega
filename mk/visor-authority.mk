# Omega Visor V1, lane 7 (authority / effect safety). "The Visor can ask. The Visor cannot grant."
#
# test-visor-authority   hostile test: Visor effect requests vs the EXISTING authority code
#                        (capability root, World publish check, AEGIS policy, generation record).
#                        Links the runtime, so it is a test-only binary; build/omega never links it.
#                        Uses the Makefile's $(CFLAGS) (same as RX_TEST); run with
#                        PHYSICS_DIR=<physics checkout>. rx_aegis.c needs the native AIENOS
#                        capability library; point VISOR_AUTH_CAP_LIB (default $(AIENOS_CAP_LIB))
#                        at a built libaienos_capability.a.
# visor-authority-check  link check: no Visor object references/defines an authority-mutating symbol.
# mk/*.mk is included before the Makefile defines AIENOS_R7_DIR/AIENOS_CAP_LIB, and a
# prerequisite list is expanded when the rule is read, so the defaults are repeated here
# (same values as the Makefile's R7 block, the pinned aienos.lock extraction; its later
# definitions are then identical). The library is a prerequisite of the link, so a changed
# libaienos_capability.a relinks the test. The old default ../aienos-r9 overrode the
# Makefile's pinned default for every authority target (R7 to R16).
AIENOS_LOCK_REPO ?= ../aienos-argus-cap
AIENOS_LOCK = $(shell head -n 1 aienos.lock)
AIENOS_R7_DEFAULT = $(OUT_DIR)/aienos-authority/$(shell echo $(AIENOS_LOCK) | cut -c1-7)
AIENOS_R7_DIR ?= $(AIENOS_R7_DEFAULT)
AIENOS_CAP_LIB ?= $(AIENOS_R7_DIR)/native/capability/out/libaienos_capability.a
VISOR_AUTH_CAP_LIB ?= $(AIENOS_CAP_LIB)
VISOR_AUTH_RX_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_generation.c src/runtime/rx_aegis.c
VISOR_AUTH_TEST = $(OUT_DIR)/tests-visor-authority/test_visor_authority

$(OUT_DIR)/tests-visor-authority:
	mkdir -p $@

$(VISOR_AUTH_TEST): tests/visor/test_visor_authority.c $(VISOR_AUTH_RX_SRCS) \
		$(OUT_DIR)/visor/visor_effect_request.o $(VISOR_CORE_OBJS) src/visor/visor_effect_request.h \
		src/runtime/rx_aegis.h src/runtime/rx_caproot.h src/runtime/rx_world.h \
		src/runtime/rx_generation.h $(VISOR_AUTH_CAP_LIB) | $(OUT_DIR)/tests-visor-authority
	@test -f "$(VISOR_AUTH_CAP_LIB)" || { echo "error: VISOR_AUTH_CAP_LIB=$(VISOR_AUTH_CAP_LIB) not found (needed by rx_aegis.c); pass VISOR_AUTH_CAP_LIB=<path to libaienos_capability.a>" >&2; exit 1; }
	$(CC) $(CFLAGS) -Isrc/visor -pthread -o $@ tests/visor/test_visor_authority.c $(VISOR_AUTH_RX_SRCS) \
		$(OUT_DIR)/visor/visor_effect_request.o $(VISOR_CORE_OBJS) $(VISOR_AUTH_CAP_LIB) -lm

.PHONY: test-visor-authority visor-authority-check
test-visor-authority: $(VISOR_AUTH_TEST)
	./$(VISOR_AUTH_TEST)

visor-authority-check: $(OUT_DIR)/visor/visor_effect_request.o
	sh tests/visor/check_authority_link.sh $(OUT_DIR)
