# Omega Visor lane 2 (console / REPL).
# `make test-visor-console` runs the console test twice:
#  1. lane-only: linked against ONLY the core objects, visor.o and the console
#     objects (-DVISOR_CONSOLE_LANE_ONLY skips the end-to-end part), so the
#     console framework can be tested even while other lanes are broken;
#  2. full: the generic rule build/tests-visor/test_visor_console (all visor +
#     language objects), which also runs the end-to-end session through the real
#     hook wiring in tools/omega.c.
VISOR_CONSOLE_OBJS = $(OUT_DIR)/visor/visor.o $(OUT_DIR)/visor/visor_console.o $(OUT_DIR)/visor/visor_parse_command.o

$(OUT_DIR)/lane2:
	mkdir -p $@

$(OUT_DIR)/lane2/test_visor_console: tests/visor/test_visor_console.c $(VISOR_CORE_OBJS) $(VISOR_CONSOLE_OBJS) | $(OUT_DIR)/lane2
	$(CC) $(VISOR_CFLAGS) -DVISOR_CONSOLE_LANE_ONLY -Itests/visor -o $@ $< $(VISOR_CORE_OBJS) $(VISOR_CONSOLE_OBJS)

# The full test #includes tools/omega.c (with OMEGA_TOOL_NO_MAIN) for the wiring.
$(OUT_DIR)/tests-visor/test_visor_console: tools/omega.c

.PHONY: test-visor-console
test-visor-console: $(OUT_DIR)/lane2/test_visor_console $(OUT_DIR)/tests-visor/test_visor_console
	$(OUT_DIR)/lane2/test_visor_console
	$(OUT_DIR)/tests-visor/test_visor_console
