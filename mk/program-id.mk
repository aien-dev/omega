# Program identity v2 (spec/program-identity.md). Physics-free; links the Visor core
# object set + discovery + src/visor + src/language. Run from the repo root.
PROGRAM_ID_TEST = $(OUT_DIR)/tests-program/test_program_id

$(PROGRAM_ID_TEST): tests/program/test_program_id.c $(VISOR_CORE_OBJS) $(OUT_DIR)/omega_discovery.o \
		$(VISOR_OBJS) $(LANG_OBJS)
	mkdir -p $(dir $@)
	$(CC) $(VISOR_CFLAGS) -o $@ $< $(VISOR_CORE_OBJS) $(OUT_DIR)/omega_discovery.o $(VISOR_OBJS) $(LANG_OBJS)

.PHONY: test-program-id
test-program-id: $(PROGRAM_ID_TEST)
	./$(PROGRAM_ID_TEST)
