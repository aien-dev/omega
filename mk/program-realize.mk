# Program realization compiler (spec/program-realization.md). Physics-free; links the
# Visor core object set. Executes generated code natively: run on an AArch64 host.
PROGRAM_REALIZE_TEST = $(OUT_DIR)/tests-realize/test_program_realize

$(PROGRAM_REALIZE_TEST): tests/realize/test_program_realize.c $(VISOR_CORE_OBJS)
	mkdir -p $(dir $@)
	$(CC) $(VISOR_CFLAGS) -o $@ $< $(VISOR_CORE_OBJS)

.PHONY: test-program-realize
test-program-realize: $(PROGRAM_REALIZE_TEST)
	./$(PROGRAM_REALIZE_TEST)
