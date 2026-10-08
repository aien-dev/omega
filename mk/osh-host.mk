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
endif
