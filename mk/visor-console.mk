# Omega Visor lane 2 (console / REPL).
# `make test-visor-console` builds the console test against ONLY the core objects,
# visor.o and the console objects, so it is not blocked by other lanes' work in
# progress. The generic rule build/tests-visor/test_visor_console (all visor +
# language objects) also works once every lane compiles.
VISOR_CONSOLE_OBJS = $(OUT_DIR)/visor/visor.o $(OUT_DIR)/visor/visor_console.o $(OUT_DIR)/visor/visor_parse_command.o

$(OUT_DIR)/lane2:
	mkdir -p $@

$(OUT_DIR)/lane2/test_visor_console: tests/visor/test_visor_console.c $(VISOR_CORE_OBJS) $(VISOR_CONSOLE_OBJS) | $(OUT_DIR)/lane2
	$(CC) $(VISOR_CFLAGS) -Itests/visor -o $@ $< $(VISOR_CORE_OBJS) $(VISOR_CONSOLE_OBJS)

# Console-only `omega` (all hooks NULL) for lane-local smoke tests.
$(OUT_DIR)/lane2/omega: $(VISOR_CORE_OBJS) $(VISOR_CONSOLE_OBJS) $(OUT_DIR)/omega_main.o | $(OUT_DIR)/lane2
	$(CC) $(VISOR_CFLAGS) -o $@ $^

.PHONY: test-visor-console
test-visor-console: $(OUT_DIR)/lane2/test_visor_console $(OUT_DIR)/lane2/omega
	$(OUT_DIR)/lane2/test_visor_console
