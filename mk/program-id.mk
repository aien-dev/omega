# Program identity v2 (spec/program-identity.md). Physics-free; links the Visor core
# object set + discovery + src/visor + src/language. Run from the repo root.
# The test admits discovery candidates through the Verified Crumb bridge (VC1 stage 6), so it also
# links the bridge, the resolver, the receipt reader and the store.
PROGRAM_ID_TEST = $(OUT_DIR)/tests-program/test_program_id
PROGRAM_ID_BRIDGE_OBJS = $(addprefix $(OUT_DIR)/,$(addsuffix .o,omega_program_ir omega_vc_bridge omega_resolve omega_receipt omega_blake3 omega_vcstore))

$(PROGRAM_ID_TEST): tests/program/test_program_id.c $(VISOR_CORE_OBJS) $(OUT_DIR)/omega_discovery.o \
		$(VISOR_OBJS) $(LANG_OBJS) $(PROGRAM_ID_BRIDGE_OBJS)
	mkdir -p $(dir $@)
	$(CC) $(VISOR_CFLAGS) -o $@ $< $(VISOR_CORE_OBJS) $(OUT_DIR)/omega_discovery.o $(VISOR_OBJS) $(LANG_OBJS) $(PROGRAM_ID_BRIDGE_OBJS)

.PHONY: test-program-id
test-program-id: $(PROGRAM_ID_TEST)
	./$(PROGRAM_ID_TEST)
