# ---------------------------------------------------------------------------
# NEXT-PHASE-1 cut 1a: build/librx_compose.a, the COMPOSITION-2 runtime as a
# static library for a host program (sovereign-core crates/aien-omega-compose),
# plus its C ABI facade src/runtime/rxc_host_abi.{h,c}.
#
#   make build/librx_compose.a   the library (PRODUCTION flags only)
#   make test-rxc-host-abi       open, run, close, reopen + recall, torn tail
#
# Sources: the production living set RX_R13_SRCS (top-level Makefile) minus
# everything under tests/, plus rxc_host_abi.c. The variable is reused, not
# copied, so the library follows the living build. The AIENOS capability
# library pinned by aienos.lock ($(AIENOS_CAP_LIB)) is merged into the same
# archive, so a host links one file (plus -lpthread -lm).
#
# Production hygiene: the objects are compiled with $(CFLAGS) $(RXC_LIB_EXTRA)
# and the recipe refuses AIEN_TEST_BUILD / RXC_TEST_HOOKS in CFLAGS
# ($(RX_PROD_REFUSE_TEST), the same check as the production program).
# -fPIC so the archive can also go into a shared object.
#
# This file is read BEFORE the top-level Makefile defines RX_R13_SRCS,
# CFLAGS and AIENOS_CAP_LIB (mk/*.mk is included at its top), so every
# prerequisite naming them is written $$(...) and expanded in the second
# phase (.SECONDEXPANSION, as mk/polyglot.mk and mk/allen.mk do).
# ---------------------------------------------------------------------------
ifndef RX_COMPOSE_LIB_MK_LOADED
RX_COMPOSE_LIB_MK_LOADED := 1

.PHONY: rx-compose-lib test-rxc-host-abi
.SECONDEXPANSION:

RXC_LIB = $(OUT_DIR)/librx_compose.a
RXC_LIB_DIR = $(OUT_DIR)/rxc_lib
RXC_LIB_EXTRA = -fPIC
RXC_LIB_SRCS = $(filter-out tests/%,$(RX_R13_SRCS)) src/runtime/rxc_host_abi.c
RXC_LIB_OBJS = $(patsubst %.c,$(RXC_LIB_DIR)/%.o,$(RXC_LIB_SRCS))
RXC_HOST_ABI_TEST = $(OUT_DIR)/rxc_host_abi_test

rx-compose-lib: $(RXC_LIB)

$(RXC_LIB_DIR)/%.o: %.c src/runtime/rxc_host_abi.h
	$(RX_PROD_REFUSE_TEST)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(RXC_LIB_EXTRA) -c $< -o $@

# One archive: the runtime objects plus the pinned AIENOS capability library
# (GNU ar MRI script; built into a temp name, then moved, so a failed build
# never leaves a half archive).
$(RXC_LIB): $$(RXC_LIB_OBJS) $$(AIENOS_CAP_LIB)
	$(RX_PROD_REFUSE_TEST)
	@mkdir -p $(dir $@)
	@rm -f $@.tmp
	@{ echo "create $@.tmp"; echo "addlib $(AIENOS_CAP_LIB)"; \
	   for o in $(RXC_LIB_OBJS); do echo "addmod $$o"; done; echo "save"; echo "end"; } | ar -M
	@ranlib $@.tmp && mv -f $@.tmp $@
	@echo "librx_compose: $@ ($(words $(RXC_LIB_OBJS)) objects + $(notdir $(AIENOS_CAP_LIB)))"

$(RXC_HOST_ABI_TEST): tests/runtime/rxc_host_abi_test.c src/runtime/rxc_host_abi.h $(RXC_LIB)
	$(CC) $(CFLAGS) -o $@ tests/runtime/rxc_host_abi_test.c $(RXC_LIB) -pthread -lm

test-rxc-host-abi: $(RXC_HOST_ABI_TEST)
	./$(RXC_HOST_ABI_TEST)

endif
