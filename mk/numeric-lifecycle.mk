# Pure host lifecycle tests. The fake M16 header is confined to this target.
.PHONY: test-numeric-lifecycle
test-numeric-lifecycle:
	@mkdir -p $(OUT_DIR)/numeric-lifecycle
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Isrc -Itests/numeric_native tests/test_numeric_lifecycle.c tests/numeric_native/other_launcher.c -o $(OUT_DIR)/numeric-lifecycle/test
	$(OUT_DIR)/numeric-lifecycle/test
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -Isrc -Itests/numeric_native tests/test_numeric_lifecycle.c tests/numeric_native/other_launcher.c -o $(OUT_DIR)/numeric-lifecycle/test-sanitize
	$(OUT_DIR)/numeric-lifecycle/test-sanitize
	CC='$(CC)' sh tests/numeric_native/mutants.sh $(OUT_DIR)/numeric-lifecycle/mutants
	@for f in src/omega_numeric_gb10.c src/omega_numeric_reduce_gb10.c src/omega_numeric_divsqrt_gb10.c; do \
	  if grep -Eq 'm16_native_(open|close|wait_marker)[[:space:]]*\(' $$f; then echo "FAIL: lifecycle bypass in $$f"; exit 1; fi; \
	done
