# POLYGLOT-0 lane B3: Omega-X in Mojo 1.0 (experiment). spec/polyglot-0.md
# Owner: lane B3. Correctness only; timed benchmarks belong to lane F.
#
# Route (a): `mojo build --emit object` produces a plain ELF object whose
# exported functions use the C ABI; it is linked statically into C. It needs no
# Mojo runtime .so and no libpython (only libc memset).
#
# If mojo is not installed, OMX_MOJO_AVAILABLE is empty, OMX_MOJO_OBJS is empty
# and test-polyglot-mojo prints a SKIP line and succeeds, so the main build is
# unaffected. This file is included twice (mk/*.mk and mk/polyglot.mk), hence
# the guard.
ifndef OMX_MOJO_MK_INCLUDED
OMX_MOJO_MK_INCLUDED := 1

MOJO ?= $(HOME)/.pixi/bin/mojo
OMX_MOJO_AVAILABLE := $(shell test -x '$(MOJO)' && '$(MOJO)' --version >/dev/null 2>&1 && echo 1)
OMX_MOJO_DIR = $(OUT_DIR)/polyglot-mojo
OMX_MOJO_SRC = polyglot/mojo/omx_mojo.mojo
OMX_MOJO_KOBJ = $(OMX_MOJO_DIR)/omx_mojo_kernels.o
OMX_MOJO_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 -march=armv8.6-a+dotprod+i8mm+sve -Isrc
OMX_MOJO_C = src/polyglot/omx_mojo.c
OMX_MOJO_TEST = $(OMX_MOJO_DIR)/test_mojo
OMX_MOJO_TEST_ASAN = $(OMX_MOJO_DIR)/test_mojo_asan
OMX_MOJO_MA3_SRCS = src/algebra/realize_common.c src/algebra/realize_binary.c \
	src/algebra/realize_bitplane.c src/algebra/realize_sparse.c \
	src/algebra/realize_rns.c src/algebra/realize_dense.c
OMX_MOJO_TEST_DEPS = tests/polyglot/test_mojo.c $(OMX_MOJO_C) src/polyglot/omx_lang.h \
	$(OMX_MOJO_MA3_SRCS) src/algebra/realize_common.h

.PHONY: test-polyglot-mojo

ifeq ($(OMX_MOJO_AVAILABLE),1)
# For other lanes (verifier/bench): link these to get omx_lane_mojo[].
OMX_MOJO_OBJS = $(OMX_MOJO_KOBJ)
OMX_MOJO_SRCS = $(OMX_MOJO_C)

# Mojo compiles for the host CPU by default (--target-cpu defaults to host).
$(OMX_MOJO_KOBJ): $(OMX_MOJO_SRC)
	@mkdir -p $(dir $@)
	$(MOJO) build --emit object -o $@ $<

$(OMX_MOJO_TEST): $(OMX_MOJO_TEST_DEPS) $(OMX_MOJO_KOBJ)
	@mkdir -p $(dir $@)
	$(CC) $(OMX_MOJO_CFLAGS) -o $@ tests/polyglot/test_mojo.c $(OMX_MOJO_C) \
		$(OMX_MOJO_MA3_SRCS) $(OMX_MOJO_KOBJ) -lm

$(OMX_MOJO_TEST_ASAN): $(OMX_MOJO_TEST_DEPS) $(OMX_MOJO_KOBJ)
	@mkdir -p $(dir $@)
	$(CC) $(OMX_MOJO_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-o $@ tests/polyglot/test_mojo.c $(OMX_MOJO_C) $(OMX_MOJO_MA3_SRCS) $(OMX_MOJO_KOBJ) -lm

test-polyglot-mojo: $(OMX_MOJO_TEST) $(OMX_MOJO_TEST_ASAN)
	@if readelf -d $(OMX_MOJO_TEST) | grep -qiE 'NEEDED.*(python|mojo|KGEN|MSupport|AsyncRT)'; then \
		echo "POLYGLOT_MOJO_FAIL unexpected runtime dependency"; readelf -d $(OMX_MOJO_TEST) | grep NEEDED; exit 1; fi
	./$(OMX_MOJO_TEST)
	./$(OMX_MOJO_TEST_ASAN)
else
OMX_MOJO_OBJS =
OMX_MOJO_SRCS =
test-polyglot-mojo:
	@echo "POLYGLOT_MOJO_SKIP mojo not found at $(MOJO) (optional lane, spec section 10.8)"
endif

endif # OMX_MOJO_MK_INCLUDED
