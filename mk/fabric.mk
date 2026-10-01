# Fabric interface F5-0 (src/fabric/fabric.h): membership, capability
# advertisement, leases and loss detection keyed by AienMachineId, over the
# in-process loopback transport. Standalone: new files only under src/fabric/
# and tests/fabric/. Picked up by `-include mk/*.mk`; NOT part of `all` or
# `test` until COMPOSITION-2 integrates it. It uses the Capability Graph only
# through src/runtime/rx_capq.h and links the same objects test-capability-graph
# links (rx_capq.o, rx_graph.o, the pinned AIENOS capability library).
#
# test-fabric         purity check, then the exit-gate test plain and with ASan/UBSan
# fabric-purity       fabric objects reference no authority, World, socket or process call
# fabric-receipt      digest-named receipt under evidence/F5-0 (clean tree only)
ifndef FABRIC_MK
FABRIC_MK := 1
.PHONY: test-fabric fabric-purity fabric-receipt
FAB_DIR = $(OUT_DIR)/fabric
FAB_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc -Isrc/runtime -Isrc/fabric
FAB_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
FAB_SRCS = src/fabric/fabric.c src/fabric/fab_hmac.c src/fabric/fab_loopback.c
FAB_HDRS = src/fabric/fabric.h src/fabric/fab_hmac.h src/fabric/fab_loopback.h \
	src/runtime/rx_capq.h src/runtime/aien_machine_id.h src/runtime/rx_jspace.h
# Routing discovers and carries records; it never grants authority, never
# touches the World, and F5-0 opens no socket and starts no process.
FAB_FORBIDDEN = ' U (aienos_|rx_caproot_|rx_world_|rx_aegis|argus_|ag_cap|socket$$|connect$$|bind$$|listen$$|accept$$|sendto$$|recvfrom$$|fork$$|exec|system$$|dlopen$$|mmap$$)'
# Same support set test-capability-graph links for rx_capq.o.
FAB_LINK_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/sha256.c src/omega_evidence.c \
	src/omega_core.c src/omega_canonical.c src/runtime/aien_machine_id.c

$(FAB_DIR)/%.o: src/fabric/%.c $(FAB_HDRS)
	@mkdir -p $(FAB_DIR)
	$(CC) $(FAB_CFLAGS) -c -o $@ $<

fabric-purity: $(FAB_DIR)/fabric.o $(FAB_DIR)/fab_hmac.o $(FAB_DIR)/fab_loopback.o
	@if nm -u $^ | grep -E $(FAB_FORBIDDEN) ; then \
		echo "fabric object references an operation the Fabric must not have"; exit 1; fi
	@echo "fabric-purity: fabric.o, fab_hmac.o and fab_loopback.o reference no forbidden symbol"

test-fabric: fabric-purity
	@$(MAKE) --no-print-directory $(RX_CAPQ_OBJ) $(RX_GRAPH_OBJ) $(AIENOS_CAP_LIB)
	$(CC) $(CFLAGS) $(FAB_CFLAGS) -pthread -o $(FAB_DIR)/fabric_test tests/fabric/fabric_test.c \
		$(FAB_SRCS) $(FAB_LINK_SRCS) $(RX_CAPQ_OBJ) $(RX_GRAPH_OBJ) $(AIENOS_CAP_LIB) -lm
	$(CC) $(CFLAGS) $(FAB_CFLAGS) $(FAB_ASAN) -pthread -o $(FAB_DIR)/fabric_test_asan \
		tests/fabric/fabric_test.c $(FAB_SRCS) $(FAB_LINK_SRCS) $(RX_CAPQ_OBJ) $(RX_GRAPH_OBJ) \
		$(AIENOS_CAP_LIB) -lm
	$(abspath $(FAB_DIR)/fabric_test)
	$(abspath $(FAB_DIR)/fabric_test_asan) > $(FAB_DIR)/asan.out
	@grep -q '^F5_0_FABRIC_LOOPBACK_PASS$$' $(FAB_DIR)/asan.out && echo "test-fabric: ASan/UBSan run PASS"

fabric-receipt:
	tests/fabric/fabric_receipt.sh
endif
