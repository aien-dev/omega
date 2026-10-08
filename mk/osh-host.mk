# osh Linux host adapter (aien-architecture#158): execution service. Picked up by `-include mk/*.mk`; not part of `all` or `test`.
# Host code is replaceable scaffolding (ADR 0024); the shell language core lives in src/osh/*.osc.
#   make test-osh-host   build + run tests/osh/host/test_osh_host.c plain and under ASan/UBSan
ifndef OSH_HOST_MK
OSH_HOST_MK := 1
.PHONY: test-osh-host
OSH_HOST_DIR = $(OUT_DIR)/osh-host
OSH_HOST_SRCS = tests/osh/host/test_osh_host.c src/osh/host/osh_req.c src/osh/host/osh_vars.c \
	src/osh/host/osh_builtin.c src/osh/host/osh_exec.c
OSH_HOST_HDRS = src/osh/host/osh_host.h src/osh/host/osh_priv.h
OSH_HOST_FLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -Isrc/osh/host
OSH_HOST_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all

$(OSH_HOST_DIR)/test_osh_host: $(OSH_HOST_SRCS) $(OSH_HOST_HDRS)
	@mkdir -p $(OSH_HOST_DIR)
	$(CC) $(OSH_HOST_FLAGS) -O2 -o $@ $(OSH_HOST_SRCS)

$(OSH_HOST_DIR)/test_osh_host_asan: $(OSH_HOST_SRCS) $(OSH_HOST_HDRS)
	@mkdir -p $(OSH_HOST_DIR)
	$(CC) $(OSH_HOST_FLAGS) $(OSH_HOST_ASAN) -o $@ $(OSH_HOST_SRCS)

test-osh-host: $(OSH_HOST_DIR)/test_osh_host $(OSH_HOST_DIR)/test_osh_host_asan
	$(abspath $(OSH_HOST_DIR)/test_osh_host) > $(OSH_HOST_DIR)/host.out; rc=$$?; tail -5 $(OSH_HOST_DIR)/host.out; test $$rc -eq 0 && tail -1 $(OSH_HOST_DIR)/host.out | grep -q '^OSH_HOST_PASS$$'
	$(abspath $(OSH_HOST_DIR)/test_osh_host_asan) > $(OSH_HOST_DIR)/host_asan.out; rc=$$?; tail -3 $(OSH_HOST_DIR)/host_asan.out; test $$rc -eq 0 && tail -1 $(OSH_HOST_DIR)/host_asan.out | grep -q '^OSH_HOST_PASS$$'
	@echo "test-osh-host: PASS (execution service; plain and ASan/UBSan)"

# capability enforcement (src/osh/host/osh_caps.c on omega's rx_caproot): allowed/denied effects with negative controls,
# revoke, stale and 64-bit generations, forged principals, and the same properties through `osh --caps`.
#   make test-osh-caps   plain and under ASan/UBSan
.PHONY: test-osh-caps
OSH_CAPS_SRCS = tests/osh/host/test_osh_caps.c src/osh/host/osh_caps.c src/runtime/rx_caproot.c src/osh/host/osh_req.c \
	src/osh/host/osh_vars.c src/osh/host/osh_builtin.c src/osh/host/osh_exec.c
OSH_CAPS_HDRS = $(OSH_HOST_HDRS) src/osh/host/osh_caps.h src/runtime/rx_caproot.h
OSH_CAPS_FLAGS = $(OSH_HOST_FLAGS) -Isrc/runtime -Wno-format-truncation

$(OSH_HOST_DIR)/test_osh_caps: $(OSH_CAPS_SRCS) $(OSH_CAPS_HDRS)
	@mkdir -p $(OSH_HOST_DIR)
	$(CC) $(OSH_CAPS_FLAGS) -O2 -o $@ $(OSH_CAPS_SRCS)

$(OSH_HOST_DIR)/test_osh_caps_asan: $(OSH_CAPS_SRCS) $(OSH_CAPS_HDRS)
	@mkdir -p $(OSH_HOST_DIR)
	$(CC) $(OSH_CAPS_FLAGS) $(OSH_HOST_ASAN) -o $@ $(OSH_CAPS_SRCS)

test-osh-caps: $(OSH_HOST_DIR)/test_osh_caps $(OSH_HOST_DIR)/test_osh_caps_asan $(OUT_DIR)/osh/osh
	$(abspath $(OSH_HOST_DIR)/test_osh_caps) $(abspath $(OUT_DIR)/osh/osh) > $(OSH_HOST_DIR)/caps.out; rc=$$?; tail -5 $(OSH_HOST_DIR)/caps.out; test $$rc -eq 0 && tail -1 $(OSH_HOST_DIR)/caps.out | grep -q '^OSH_CAPS_PASS$$'
	$(abspath $(OSH_HOST_DIR)/test_osh_caps_asan) $(abspath $(OUT_DIR)/osh/osh) > $(OSH_HOST_DIR)/caps_asan.out; rc=$$?; tail -3 $(OSH_HOST_DIR)/caps_asan.out; test $$rc -eq 0 && tail -1 $(OSH_HOST_DIR)/caps_asan.out | grep -q '^OSH_CAPS_PASS$$'
	@echo "test-osh-caps: PASS (capability enforcement; plain and ASan/UBSan)"
endif
