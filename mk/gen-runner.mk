# tools/gen/runner + tools/gen/reducer: the GEN experiment driver and reducer (omega#477, GEN-2).
# Picked up by `-include mk/*.mk`; not part of `all` or `test`. CPU only: the tests use fakegen, a local
# stand-in endpoint on 127.0.0.1, never a model server.
#
# test-gen-runner   tools/gen/runner/test_gen_runner.sh: genstat against Agent A's stats vectors and its own,
#                   the driver against fakegen and the stub provider, the reducer on harness experiment runs,
#                   negative tests and mutants
ifndef GEN_RUNNER_MK
GEN_RUNNER_MK := 1
.PHONY: gen-runner test-gen-runner
GEN_DIR = $(OUT_DIR)/gen
GEN_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -O2

$(GEN_DIR)/genstat: tools/gen/reducer/genstat.c
	@mkdir -p $(GEN_DIR)
	$(CC) $(GEN_CFLAGS) -o $@ tools/gen/reducer/genstat.c -lm

$(GEN_DIR)/fakegen: tools/gen/runner/fakegen.c
	@mkdir -p $(GEN_DIR)
	$(CC) $(GEN_CFLAGS) -D_GNU_SOURCE -o $@ tools/gen/runner/fakegen.c

gen-runner: $(GEN_DIR)/genstat $(GEN_DIR)/fakegen

test-gen-runner: gen-runner omega-emit omegese one-layer-acceptance-tools
	bash tools/gen/runner/test_gen_runner.sh
endif
