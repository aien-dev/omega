# TURING Yield TY-4/TY-5: energy measurement reuse and concurrent attribution.
# docs/turing/TURING_YIELD_ENERGY_PROTOCOL_V0.md
#
# Picked up by the top-level `-include mk/*.mk` (no Makefile edit). This file is
# read before most of the Makefile, so it uses only its own variables plus
# CC and OUT_DIR (both set before the include).
#
# Targets:
#   test-turing-energy  hostile tests, plain + ASan/UBSan; no meter, no timing,
#                       runs in CI on recorded fixtures (tests/turing/fixtures/ty_energy)
#   ty-energy-tools     the workload, window tool and reducer
#   ty-energy-run       the timed stage 1 + stage 2 run, detached (setsid nohup);
#                       waits for an idle machine and holds its own quiet flag
ifndef TURING_YIELD_ENERGY_MK
TURING_YIELD_ENERGY_MK := 1

TYE_DIR = $(OUT_DIR)/ty
TYE_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc -Itests/runtime
TYE_LIB_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 -Isrc
TYE_ARCH = -march=armv8.6-a+dotprod+i8mm+sve
TYE_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
TYE_ALG_SRCS = src/algebra/realize_common.c src/algebra/realize_binary.c src/algebra/realize_bitplane.c \
	src/algebra/realize_sparse.c src/algebra/realize_rns.c src/algebra/realize_dense.c
TYE_LIB_SRCS = src/turing/ty_energy.c src/omega_canonical.c src/sha256.c
TYE_LIB_HDRS = src/turing/ty_energy.h src/turing/field.h src/omega_canonical.h src/omega_types.h src/sha256.h
TYE_WORKLOAD = $(TYE_DIR)/ty_workload
TYE_WINDOW = $(TYE_DIR)/ty_energy_window
TYE_REDUCE = $(TYE_DIR)/ty_energy_reduce
TYE_REDUCE_ASAN = $(TYE_DIR)/ty_energy_reduce_asan
TYE_TEST = $(TYE_DIR)/test_ty_energy
TYE_TEST_ASAN = $(TYE_DIR)/test_ty_energy_asan
TYE_FIXTURE = $(TYE_DIR)/ty_energy_fixture

$(TYE_WORKLOAD): tests/turing/ty_workload.c tests/runtime/r15_measure.c tests/runtime/r15_measure.h $(TYE_ALG_SRCS) src/algebra/realize_common.h
	@mkdir -p $(dir $@)
	$(CC) $(TYE_CFLAGS) $(TYE_ARCH) -o $@ tests/turing/ty_workload.c tests/runtime/r15_measure.c $(TYE_ALG_SRCS) -lpthread -ldl

$(TYE_WINDOW): tests/turing/ty_energy_window.c tests/runtime/r15_measure.c tests/runtime/r15_measure.h
	@mkdir -p $(dir $@)
	$(CC) $(TYE_CFLAGS) -o $@ tests/turing/ty_energy_window.c tests/runtime/r15_measure.c -lpthread -ldl

# tools/r15_reduce.c is compiled inside the reducer's translation unit (its
# main renamed); -Wno-unused-function for the r15 helpers this tool does not call.
$(TYE_REDUCE): tools/ty_energy_reduce.c tools/r15_reduce.c $(TYE_LIB_SRCS) $(TYE_LIB_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TYE_CFLAGS) -Itools -Wno-unused-function -o $@ tools/ty_energy_reduce.c $(TYE_LIB_SRCS) -lm

$(TYE_REDUCE_ASAN): tools/ty_energy_reduce.c tools/r15_reduce.c $(TYE_LIB_SRCS) $(TYE_LIB_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TYE_CFLAGS) $(TYE_ASAN) -Itools -Wno-unused-function -o $@ tools/ty_energy_reduce.c $(TYE_LIB_SRCS) -lm

$(TYE_TEST): tests/turing/test_ty_energy.c $(TYE_LIB_SRCS) $(TYE_LIB_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TYE_LIB_CFLAGS) -o $@ tests/turing/test_ty_energy.c $(TYE_LIB_SRCS)

$(TYE_TEST_ASAN): tests/turing/test_ty_energy.c $(TYE_LIB_SRCS) $(TYE_LIB_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(TYE_LIB_CFLAGS) $(TYE_ASAN) -o $@ tests/turing/test_ty_energy.c $(TYE_LIB_SRCS)

.PHONY: test-turing-energy ty-energy-tools ty-energy-run
$(TYE_FIXTURE): tests/turing/ty_energy_fixture.c
	@mkdir -p $(dir $@)
	$(CC) $(TYE_LIB_CFLAGS) -o $@ $<

ty-energy-tools: $(TYE_WORKLOAD) $(TYE_WINDOW) $(TYE_REDUCE)

test-turing-energy: $(TYE_TEST) $(TYE_TEST_ASAN) $(TYE_REDUCE) $(TYE_REDUCE_ASAN) $(TYE_FIXTURE)
	./$(TYE_TEST)
	./$(TYE_TEST_ASAN)
	sh tests/turing/test_ty_energy_fixtures.sh ./$(TYE_REDUCE) $(TYE_DIR)/fixtures ./$(TYE_FIXTURE)
	sh tests/turing/test_ty_energy_fixtures.sh ./$(TYE_REDUCE_ASAN) $(TYE_DIR)/fixtures_asan ./$(TYE_FIXTURE)

TYE_RUN_ID ?= $(shell date -u +%Y%m%dT%H%M%SZ)
ty-energy-run: ty-energy-tools
	@mkdir -p evidence/TURING_YIELD/energy/$(TYE_RUN_ID)
	setsid nohup sh tools/ty_energy_run.sh all evidence/TURING_YIELD/energy/$(TYE_RUN_ID) $(TYE_DIR) \
		> evidence/TURING_YIELD/energy/$(TYE_RUN_ID)/run.log 2>&1 < /dev/null &
	@echo "detached: evidence/TURING_YIELD/energy/$(TYE_RUN_ID)/run.log"

endif
