# Omega Visor V1 lane 5 (machine + realization lab) test targets.
# The Blackwell realize objects compile and link physics-free (only sha256 +
# omega_core deps). visor_machine.c references omega_blackwell_get_machine_id
# WEAKLY, so build/omega links with or without them; these lane tests link
# them explicitly so the declared Blackwell target shows linked=yes.
VISOR_BW_CORE = omega_vector omega_blackwell_qmd omega_blackwell_encoder omega_blackwell_realize
VISOR_BW_OBJS = $(addprefix $(OUT_DIR)/,$(addsuffix .o,$(filter-out $(VISOR_CORE),$(VISOR_BW_CORE))))

$(OUT_DIR)/tests-visor/test_visor_machine: tests/visor/test_visor_machine.c $(VISOR_CORE_OBJS) $(VISOR_BW_OBJS) $(VISOR_OBJS) $(LANG_OBJS) | $(OUT_DIR)/tests-visor
	$(CC) $(VISOR_CFLAGS) -Itests/visor -o $@ $< $(VISOR_CORE_OBJS) $(VISOR_BW_OBJS) $(VISOR_OBJS) $(LANG_OBJS)

$(OUT_DIR)/tests-visor/test_visor_realization: tests/visor/test_visor_realization.c $(VISOR_CORE_OBJS) $(VISOR_BW_OBJS) $(VISOR_OBJS) $(LANG_OBJS) | $(OUT_DIR)/tests-visor
	$(CC) $(VISOR_CFLAGS) -Itests/visor -o $@ $< $(VISOR_CORE_OBJS) $(VISOR_BW_OBJS) $(VISOR_OBJS) $(LANG_OBJS)

.PHONY: test-visor-machine test-visor-realization
test-visor-machine: $(OUT_DIR)/tests-visor/test_visor_machine
	./$(OUT_DIR)/tests-visor/test_visor_machine

test-visor-realization: $(OUT_DIR)/tests-visor/test_visor_realization
	./$(OUT_DIR)/tests-visor/test_visor_realization
