# Decode-graph performance receipt (aien-architecture#233, lane P1): tools/dgraph_receipt.c turns an
# existing decode-graph chip run (tools/run_gpu_decode_graph_chip.sh evidence) into one commit-bound
# receipt "omega-decode-graph-receipt/1" and checks such receipts. Host only: links nothing of
# libomega_gpu and no GPU header. Spec: docs/perf/DECODE_GRAPH_RECEIPT.md. Picked up by
# `-include mk/*.mk`; not part of `all` or `test`; run by tests/gates/run.sh chip-tool-selftests.
#   make dgraph-receipt        build/dgraph_receipt/dgraph_receipt
#   make test-dgraph-receipt   golden receipts regenerated from evidence/, checker accepts them,
#                              every negative fixture refused for its named reason (plain + ASan/UBSan)
ifndef DGRAPH_RECEIPT_MK
DGRAPH_RECEIPT_MK := 1
.PHONY: dgraph-receipt test-dgraph-receipt
DGR_DIR = $(OUT_DIR)/dgraph_receipt
DGR_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2
DGR_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all

dgraph-receipt: $(DGR_DIR)/dgraph_receipt

$(DGR_DIR)/dgraph_receipt: tools/dgraph_receipt.c
	@mkdir -p $(DGR_DIR)
	$(CC) $(DGR_CFLAGS) -o $@ tools/dgraph_receipt.c

$(DGR_DIR)/dgraph_receipt_asan: tools/dgraph_receipt.c
	@mkdir -p $(DGR_DIR)
	$(CC) $(DGR_CFLAGS) $(DGR_ASAN) -o $@ tools/dgraph_receipt.c

# independent strict-JSON check of the golden receipts (tools/json_canon.c, the M19R helper)
$(DGR_DIR)/json_canon: tools/json_canon.c src/sha256.c src/sha256.h
	@mkdir -p $(DGR_DIR)
	$(CC) $(DGR_CFLAGS) -Isrc -o $@ tools/json_canon.c src/sha256.c

test-dgraph-receipt: $(DGR_DIR)/dgraph_receipt $(DGR_DIR)/dgraph_receipt_asan $(DGR_DIR)/json_canon
	sh tests/dgraph_receipt/run.sh $(DGR_DIR)/dgraph_receipt $(DGR_DIR)/json_canon $(DGR_DIR)/work
	sh tests/dgraph_receipt/run.sh $(DGR_DIR)/dgraph_receipt_asan $(DGR_DIR)/json_canon $(DGR_DIR)/work-asan

endif
