CC ?= gcc
.DEFAULT_GOAL := all
PHYSICS_DIR ?= ../physics
OUT_DIR ?= build
-include mk/*.mk
PHYSICS_LOCK_CHECK ?= 1

CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -MMD -MP -D_GNU_SOURCE -O2 -Isrc \
	-I$(PHYSICS_DIR)/m16 -I$(PHYSICS_DIR)/nvrm \
	-I$(PHYSICS_DIR)/third_party/nvidia-open-580.173.02/src/common/sdk/nvidia/inc \
	-I$(PHYSICS_DIR)/third_party/nvidia-open-580.173.02/kernel-open/common/inc \
	-I$(PHYSICS_DIR)/third_party/nvidia-open-580.173.02/kernel-open/nvidia-uvm \
	-I$(PHYSICS_DIR)/third_party/nvidia-open-580.173.02/src/nvidia/arch/nvalloc/unix/include

SRCS = src/sha256.c src/omega_canonical.c src/omega_validate.c src/omega_core.c src/omega_codec.c \
	src/aarch64_encoder.c src/aarch64_decoder.c src/omega_realize.c src/omega_exec.c \
	src/omega_self_host.c src/omega_verify.c src/omega_program.c src/omega_synthesis.c \
	src/omega_library.c src/omega_discovery.c src/omega_machine.c src/omega_realize_synth.c \
	src/omega_matvec.c src/omega_accelerator.c src/omega_accelerator_world.c \
	src/omega_vector.c src/omega_blackwell_encoder.c src/omega_blackwell_qmd.c \
	src/omega_blackwell_realize.c src/omega_blackwell_submit.c src/omega_blackwell_engine.c src/omega_gpu_engine.c src/omega_blackwell_gates.c src/omega_blackwell_matmul.c src/omega_blackwell_codegen.c src/omega_gpu_session.c src/omega_gpu_matmul_api.c src/omega_gpu_elementwise_api.c src/omega_gpu_attention_api.c src/omega_world_gates.c src/omega_gpu_wait.c \
	src/omega_evidence.c \
	$(PHYSICS_DIR)/m16/m16_native.c $(PHYSICS_DIR)/nvrm/nvrm.c \
	tools/omegatool.c

OBJS = $(patsubst %.c,$(OUT_DIR)/%.o,$(notdir $(SRCS)))
DEPS = $(OBJS:.o=.d)
-include $(DEPS)
TARGET = $(OUT_DIR)/omegatool

# Crumbline learner: a separate executable linked ONLY from learner-side code
# (visible reader, Omega program/realize/verify stack, synthesis vocabulary,
# OmegaLibrary, Crumbline search). It links no physics, no gate suites and no
# sealed-side code; the sealed side lives in the crumbs crate.
CL_SRCS = src/crumbline/cl_common.c src/crumbline/cl_crumb.c src/crumbline/cl_program.c src/crumbline/cl_search.c
LEARNER_CORE = sha256 omega_canonical omega_validate omega_core omega_codec aarch64_encoder aarch64_decoder \
	omega_realize omega_realize_synth omega_machine omega_exec omega_verify omega_program omega_synthesis omega_library
LEARNER_OBJS = $(addprefix $(OUT_DIR)/,$(addsuffix .o,$(LEARNER_CORE))) \
	$(patsubst src/crumbline/%.c,$(OUT_DIR)/crumbline/%.o,$(CL_SRCS)) $(OUT_DIR)/crumbline_learner.o
LEARNER = $(OUT_DIR)/crumbline-learner

.PHONY: all clean check-physics-lock crumbline-learner test-crumbline test-m19 test test-m5 test-m6 test-m7 test-m8 test-m9 test-m10 test-m11 test-m12 test-m13 test-m14 test-m15 test-m17 test-r3 test-i11-wake-merge test-action-graph test-state-projection test-capability-query test-capability-graph test-skillroute-compose test-semantic-comm test-cognitive-routing test-sem-incremental test-branch-reuse test-jspace-prod test-plan-reuse test-cortex

all: $(TARGET)

check-physics-lock:
	@if [ "$(PHYSICS_LOCK_CHECK)" != "0" ]; then \
		if [ ! -f physics.lock ]; then \
			echo "error: physics.lock not found at repo root" >&2; exit 1; \
		fi; \
		locked=$$(tr -d '[:space:]' < physics.lock); \
		actual=$$(git -C $(PHYSICS_DIR) rev-parse HEAD 2>/dev/null); \
		if [ -z "$$actual" ]; then \
			echo "error: could not read HEAD of PHYSICS_DIR=$(PHYSICS_DIR) (git -C $(PHYSICS_DIR) rev-parse HEAD failed)" >&2; \
			exit 1; \
		fi; \
		if [ "$$actual" != "$$locked" ]; then \
			echo "error: PHYSICS_DIR=$(PHYSICS_DIR) is at commit $$actual but physics.lock pins $$locked." >&2; \
			echo "       Checkout the pinned physics commit, or override with PHYSICS_LOCK_CHECK=0." >&2; \
			exit 1; \
		fi; \
	fi

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(OUT_DIR)/%.o: src/%.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/m16_native.o: $(PHYSICS_DIR)/m16/m16_native.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/nvrm.o: $(PHYSICS_DIR)/nvrm/nvrm.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/crumbline/%.o: src/crumbline/%.c | $(OUT_DIR)
	mkdir -p $(OUT_DIR)/crumbline
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/crumbline_learner.o: tools/crumbline_learner.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

crumbline-learner: $(LEARNER)

test-crumbline: $(LEARNER)
	./tests/crumbline/run_conformance.sh $(LEARNER) tests/crumbline/vectors
	./tests/crumbline/run_crb1_conformance.sh $(LEARNER) tests/crumbline/crb1 tests/crumbline/crb1_findings.txt

$(LEARNER): $(LEARNER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(LEARNER_OBJS)

$(OUT_DIR)/omegatool.o: tools/omegatool.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): check-physics-lock $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

test: $(TARGET)
	./$(TARGET) --run-gates
	./$(TARGET) --reference-demonstrate-arithmetic
	./$(TARGET) --reference-demonstrate-physics

test-m5: $(TARGET)
	./$(TARGET) --run-m5-gates
	./$(TARGET) --reference-demonstrate-realization

test-m6: $(TARGET)
	./$(TARGET) --run-m6-gates
	./$(TARGET) --reference-demonstrate-self-host

test-m7: $(TARGET)
	./$(TARGET) --run-m7-gates
	./$(TARGET) --reference-demonstrate-verify

test-m8: $(TARGET)
	./$(TARGET) --run-m8-gates
	./$(TARGET) --reference-demonstrate-program

test-m9: $(TARGET)
	./$(TARGET) --run-m9-gates
	./$(TARGET) --reference-demonstrate-synthesis

test-m10: $(TARGET)
	./$(TARGET) --run-m10-gates
	./$(TARGET) --reference-demonstrate-library

test-m11: $(TARGET)
	./$(TARGET) --run-m11-gates
	./$(TARGET) --reference-demonstrate-discovery

test-m12: $(TARGET)
	./$(TARGET) --run-m12-gates
	./$(TARGET) --legacy-oracle-living-matvec

test-m13: $(TARGET)
	./$(TARGET) --run-m13-gates
	./$(TARGET) --reference-demonstrate-machine

test-m14: $(TARGET)
	./$(TARGET) --run-m14-gates
	./$(TARGET) --reference-demonstrate-realization-synthesis

test-m15: $(TARGET)
	./$(TARGET) --run-m15-gates
	./$(TARGET) --reference-demonstrate-accelerator

test-m17: $(TARGET)
	./$(TARGET) --run-m17-gates
	./$(TARGET) --reference-demonstrate-blackwell-vector

clean:
	rm -rf $(OUT_DIR)

test-m19: $(TARGET)
	./$(TARGET) --run-m19-gates

# Host-only tests of the M19R qualifier (tools/m19r_qualify.sh) and its
# canonical-JSON helper (tools/json_canon.c). No GPU.
.PHONY: test-m19r-qualify
test-m19r-qualify:
	tools/test_m19r_qualify.sh

# Host-only tests of the chipwait campaign runner (tools/chipwait_campaign.sh)
# against a stub qualifier. No GPU.
.PHONY: test-chipwait-campaign
test-chipwait-campaign:
	tools/test_chipwait_campaign.sh

# Host-only tests of the Gate 14 combiner (tools/gate14_combine.sh). No GPU.
.PHONY: test-gate14-combine
test-gate14-combine:
	tools/test_gate14_combine.sh

# Host-only self-test of the E1 closure combiner (tools/e1_combine.sh): good
# synthetic constituent set accepted, every hostile mutant refused. No GPU.
.PHONY: test-e1-combine
test-e1-combine:
	tools/test_e1_combine.sh

# Host-only self-test of the shared chip-run module (tools/chip_run.sh): every
# refusal path with fake binaries. No GPU.
.PHONY: test-chip-run
test-chip-run:
	tests/test_chip_run.sh

# Host-only check of the unwritten-trap manifest and its wrapper. No GPU.
.PHONY: test-chip-run-trap-manifest
test-chip-run-trap-manifest:
	tests/test_chip_run_trap_manifest.sh

# Host-only check of the transcendental-gate manifest and its wrapper (fake chip, old-vs-new receipt keys). No GPU.
.PHONY: test-chip-run-transc-manifest
test-chip-run-transc-manifest:
	tests/test_chip_run_transc_manifest.sh

# Gate 5 (OMEGA-NUMERIC-0), CPU tiers only: reference, CPU parity, provenance
# and negative tests. Opens no device. The GB10 tier and the receipt come
# from tests/run_numeric_gates.sh on the chip. Exits nonzero while any gate
# item fails.
.PHONY: test-numeric-cpu test-numeric-qualify
NUMERIC_CPU_SRCS = tests/test_omega_numeric.c src/omega_numeric.c src/omega_numeric_provenance.c src/omega_numeric_divsqrt_gb10.c \
                   $(NUMERIC_BW_SRCS)
NUMERIC_CPU_HDRS = src/omega_numeric.h src/omega_numeric_provenance.h src/omega_numeric_divsqrt_gb10.h tests/numeric_oracle.h \
                   $(NUMERIC_BW_HDRS)
build/test_omega_numeric_cpu: $(NUMERIC_CPU_SRCS) $(NUMERIC_CPU_HDRS)
	@mkdir -p build
	gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off -Isrc -DOMEGA_NUMERIC_CPU_ONLY -o $@ $(NUMERIC_CPU_SRCS)
# test-numeric-cpu exit status: 0 means no host test failed and the only SKIPs
# are the five declared chip-only IDs (CHIP_ONLY_IDS in the test; the last line
# prints "Gate 5 Verdict: PASS_EXCEPT_DECLARED_CHIP_ONLY"). Nonzero means a real
# host regression (verdict HOST_REGRESSION) or a SKIP nobody declared
# (UNDECLARED_SKIP). The chip build allows no SKIP at all.
test-numeric-cpu: build/test_omega_numeric_cpu
	./build/test_omega_numeric_cpu

# Host-only tests of the Gate 5 qualifier and receipt writer. No GPU.
test-numeric-qualify: build/test_omega_numeric_cpu
	tools/test_numeric_qualify.sh

# Deletes each CHECK-marked pre-submission check in src/omega_numeric.c in a
# scratch copy and proves a Gate 5 host test then fails. No GPU.
# Then applies each arithmetic mutation in tools/numeric_oracle_mutations.sh
# (broken EXP/LOG coefficients, wrong host instruction, wrong LDS index, an
# undeclared SKIP) and proves the CPU-only run exits nonzero. No GPU.
.PHONY: test-numeric-sweep
test-numeric-sweep:
	tools/numeric_check_sweep.sh
	tools/numeric_oracle_mutations.sh

# E1 scalar contract, exhaustive: every 2^32 input of each unary E1 op through
# the reference, the CPU tier and the integer oracle (one process per op,
# docs/numeric/E1_SCALAR_CONTRACT.md). Not part of Gate 5. No GPU.
.PHONY: test-numeric-e1-exhaustive
test-numeric-e1-exhaustive: build/test_omega_numeric_cpu
	./build/test_omega_numeric_cpu --e1-exhaustive

# E1 WP-D general reductions (docs/numeric/E1_REDUCTION_CONTRACT.md), CPU
# tiers only: reference, CPU realization, oracles, negative order test and the
# GB10 pre-submission checks. Opens no device. The chip parity run is
# tests/run_reduce_chip.sh. Last line: "E1 Reduce Verdict: PASS_EXCEPT_DECLARED_CHIP_ONLY".
.PHONY: test-numeric-reduce-cpu
REDUCE_CPU_SRCS = tests/test_omega_reduce.c src/omega_numeric_reduce.c src/omega_numeric_reduce_gb10.c \
                  src/omega_numeric.c src/omega_numeric_provenance.c src/omega_numeric_divsqrt_gb10.c \
                  $(NUMERIC_BW_SRCS)
build/test_omega_reduce_cpu: $(REDUCE_CPU_SRCS) src/omega_numeric_reduce.h $(NUMERIC_CPU_HDRS)
	@mkdir -p build
	gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off -Isrc -DOMEGA_NUMERIC_CPU_ONLY -o $@ $(REDUCE_CPU_SRCS)
test-numeric-reduce-cpu: build/test_omega_reduce_cpu
	./build/test_omega_reduce_cpu

# E1 WP-B: FP32 transcendental sequences (SIGMOID TANH RSQRT EXP2 LOG2 ERF SIN
# COS GELU), CPU tier, bounded contract (docs/numeric/E1_TRANSCENDENTAL_CONTRACT.md).
# No libm, no GPU. test-numeric-transc: special values, oracle self-checks,
# about 1.1M sampled inputs per op against the binary128 oracle, determinism,
# then one build per perturbed coefficient (OMEGA_TRANSC_MUTATE=1..10) which
# must FAIL. test-numeric-transc-full: all 2^32 inputs of every op (long; run
# it detached). test-numeric-transc-digest: full-domain outputs against the
# frozen digests.
.PHONY: test-numeric-transc test-numeric-transc-full test-numeric-transc-digest
TRANSC_SRCS = tests/test_omega_transc.c src/omega_numeric_transc.c src/sha256.c
TRANSC_HDRS = src/omega_numeric_transc.h src/omega_numeric.h src/sha256.h
TRANSC_CC = gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off -fno-fast-math -pthread -Isrc
TRANSC_MUTANTS = 1 2 3 4 5 6 7 8 9 10
build/test_omega_transc: $(TRANSC_SRCS) $(TRANSC_HDRS)
	@mkdir -p build
	$(TRANSC_CC) -o $@ $(TRANSC_SRCS)
test-numeric-transc: build/test_omega_transc
	./build/test_omega_transc fast
	@for m in $(TRANSC_MUTANTS); do \
	  $(TRANSC_CC) -DOMEGA_TRANSC_MUTATE=$$m -o build/test_omega_transc_mut$$m $(TRANSC_SRCS) || exit 1; \
	  if ./build/test_omega_transc_mut$$m fast > build/test_omega_transc_mut$$m.log 2>&1; then \
	    echo "MUTANT $$m SURVIVED (bound check did not fail)"; exit 1; \
	  else echo "mutant $$m killed: $$(grep -m1 '^FAIL' build/test_omega_transc_mut$$m.log)"; fi; \
	done
	@echo "test-numeric-transc: PASS (all $(words $(TRANSC_MUTANTS)) mutants killed)"
test-numeric-transc-full: build/test_omega_transc
	./build/test_omega_transc full
test-numeric-transc-digest: build/test_omega_transc
	./build/test_omega_transc digest

# Offline provenance of the GB10 MAX/MIN warp patch: nvdisasm -b SM121 must
# decode every word to the text the patch table records. No device opened;
# nvdisasm is a decoder only. Last line: "E1 Reduce nvdisasm: PASS".
.PHONY: test-numeric-reduce-nvdisasm
REDUCE_NVDISASM ?= /usr/local/cuda/bin/nvdisasm
test-numeric-reduce-nvdisasm: build/test_omega_reduce_cpu
	@[ -x "$(REDUCE_NVDISASM)" ] || { echo "E1 Reduce nvdisasm: NOT_RUN ($(REDUCE_NVDISASM) missing)"; exit 2; }
	@set -e; t=$$(mktemp -d); trap 'rm -rf "$$t"' EXIT; ./build/test_omega_reduce_cpu --dump "$$t"; \
	for op in max min; do \
	  "$(REDUCE_NVDISASM)" -b SM121 "$$t/$$op.bin" > "$$t/$$op.raw" || { echo "E1 Reduce nvdisasm: FAIL ($$op: nvdisasm error)"; exit 1; }; \
	  sed -n 's|^[[:space:]]*/\*\([0-9a-f]\{4\}\)\*/[[:space:]]*\(.*;\).*$$|\1 \2|p' "$$t/$$op.raw" \
	    | sed 's/[[:space:]]\{1,\}/ /g' > "$$t/$$op.got"; \
	  diff -u "$$t/$$op.lst" "$$t/$$op.got" || { echo "E1 Reduce nvdisasm: FAIL ($$op)"; exit 1; }; echo "$$op: $$(wc -l < "$$t/$$op.lst") words decode to the recorded text"; \
	done; echo "E1 Reduce nvdisasm: PASS ($$("$(REDUCE_NVDISASM)" --version | grep -o 'release [0-9.]*, V[0-9.]*'))"

# Resident reaction runtime heartbeat (ADR 0016, R3/R4 host reference).
# CPU only; links no PHYSICS/NVRM code (omega_evidence.c needs only the header).
RX_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/sha256.c src/omega_evidence.c tests/runtime/rx_heartbeat_test.c
RX_TEST = $(OUT_DIR)/rx_heartbeat_test

$(RX_TEST): $(RX_SRCS) src/runtime/rx_caproot.h src/runtime/rx_world.h \
	src/runtime/omega_shared_world_abi.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_SRCS)

test-r3: $(RX_TEST)
	./$(RX_TEST)

# HD-09 resource contract v0, first enforcement cut: a declared deadline is
# enforced before publishing (late reaction cancelled, nothing published,
# charge refunded once). CPU only, same links as R3 minus the heartbeat test.
RX_DEADLINE_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/sha256.c src/omega_evidence.c tests/runtime/rx_deadline_cancel.c
RX_DEADLINE_TEST = $(OUT_DIR)/rx_deadline_cancel

$(RX_DEADLINE_TEST): $(RX_DEADLINE_SRCS) src/runtime/rx_caproot.h src/runtime/rx_world.h \
	src/runtime/omega_shared_world_abi.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_DEADLINE_SRCS)

.PHONY: test-rx-deadline-cancel
test-rx-deadline-cancel: $(RX_DEADLINE_TEST)
	./$(RX_DEADLINE_TEST)

# I11: a dependent popped while a writer of its inputs is still computing is
# held so the writer's wake merges into it (one commit per stimulus, as with
# one worker). Deterministic forced interleaving; the mutant build has no hold
# and must FAIL. CPU only, same links as R3.
RX_I11_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/sha256.c src/omega_evidence.c tests/runtime/rx_i11_wake_merge.c
RX_I11_TEST = $(OUT_DIR)/rx_i11_wake_merge
RX_I11_MUTANT = $(OUT_DIR)/rx_i11_wake_merge_mutant

$(RX_I11_TEST): $(RX_I11_SRCS) src/runtime/rx_caproot.h src/runtime/rx_world.h \
	src/runtime/omega_shared_world_abi.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_I11_SRCS)

$(RX_I11_MUTANT): $(RX_I11_SRCS) src/runtime/rx_caproot.h src/runtime/rx_world.h \
	src/runtime/omega_shared_world_abi.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -DRX_WORLD_MUTATE_NO_UPSTREAM_HOLD -pthread -o $@ $(RX_I11_SRCS)

test-i11-wake-merge: $(RX_I11_TEST) $(RX_I11_MUTANT)
	./$(RX_I11_TEST)
	@if ./$(RX_I11_MUTANT); then \
	  echo "MUTANT SURVIVED (no upstream hold, test still passed)"; exit 1; \
	else echo "test-i11-wake-merge: PASS (mutant killed)"; fi

# Omega semantic variables and incremental recomputation
# (gate OMEGA_INCREMENTAL_SEMANTICS_PASS). CPU only, same links as R3.
RX_SEM_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_semantic.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_sem_incremental.c
RX_SEM_TEST = $(OUT_DIR)/rx_sem_incremental_test

$(RX_SEM_TEST): $(RX_SEM_SRCS) src/runtime/rx_semantic.h src/runtime/rx_caproot.h \
	src/runtime/rx_world.h src/runtime/omega_shared_world_abi.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_SEM_SRCS)

test-sem-incremental: $(RX_SEM_TEST)
	./$(RX_SEM_TEST)

# R7: native AIENOS authority versus the Linux oracle, then the world view.
# The authority source is the FULL commit in aienos.lock, proven by
# tools/aienos_lock_source.sh from git objects (a directory name, a short SHA or an
# override is not proof). Default: extracted with `git archive` from a repository holding
# that commit (AIENOS_LOCK_REPO, or the repository around AIENOS_R7_DIR) into
# $(OUT_DIR)/aienos-authority/<full lock>. An AIENOS_R7_DIR override is used only after
# the same proof: every tracked file under the subpaths built from has the locked blob
# and no unignored extra file sits beside them. The proof runs on every build (phony
# prerequisite), so a source edited after extraction stops the build.
AIENOS_LOCK_REPO ?= ../aienos-argus-cap
AIENOS_LOCK = $(shell head -n 1 aienos.lock)
AIENOS_R7_DEFAULT = $(OUT_DIR)/aienos-authority/$(AIENOS_LOCK)
AIENOS_R7_DIR ?= $(AIENOS_R7_DEFAULT)
AIENOS_CAP_LIB ?= $(OUT_DIR)/aienos-cap/$(AIENOS_LOCK)/libaienos_capability.a
# $(call aienos_source,<subpaths>): materialize into the default cache, or prove the override.
aienos_source = if [ "$(AIENOS_R7_DIR)" = "$(AIENOS_R7_DEFAULT)" ]; then \
		AIENOS_LOCK_REPO="$(AIENOS_LOCK_REPO)" AIENOS_R7_DIR= bash tools/aienos_lock_source.sh materialize "$(AIENOS_R7_DIR)" $(1); \
	else \
		AIENOS_LOCK_REPO="$(AIENOS_LOCK_REPO)" AIENOS_R7_DIR="$(AIENOS_R7_DIR)" bash tools/aienos_lock_source.sh verify-dir "$(AIENOS_R7_DIR)" $(1); \
	fi || { echo "error: the aienos authority source is not the aienos.lock commit $(AIENOS_LOCK) (see above)."; \
		echo "  pass AIENOS_LOCK_REPO=<an aienos clone holding that commit>, or AIENOS_R7_DIR=<a clean tree of it inside such a clone>;"; \
		echo "  a modified default cache under $(OUT_DIR)/aienos-authority must be deleted, not reused."; exit 1; }
RX_R7_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_r7_native.c
RX_R7_TEST = $(OUT_DIR)/rx_r7_native_test

.PHONY: aienos-authority-capability
aienos-authority-capability:
	@$(call aienos_source,native/capability)

# Built out of tree from the proven source into a fresh directory on every build; the
# library is replaced only when its bytes change (ar is deterministic), so nothing relinks
# when nothing changed and no stale or planted object is ever reused. The inner make is
# called by name with MAKEFLAGS cleared: the outer command line (CFLAGS=..., -n, -W)
# never reaches the authority build, and a dry run (make -n) only prints this recipe.
$(AIENOS_CAP_LIB): aienos-authority-capability
	@t=$$(mktemp -d "$${TMPDIR:-/tmp}/aienos-cap.XXXXXX") && \
	env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL make -s -C $(AIENOS_R7_DIR)/native/capability OUT=$$t $$t/libaienos_capability.a >/dev/null && \
	mkdir -p $(dir $@) && { cmp -s $$t/libaienos_capability.a $@ || cp $$t/libaienos_capability.a $@; }; \
	rc=$$?; rm -rf $$t; exit $$rc

# OMEGA_EFFECT_CAP64 (spec/effect-cap64-migration.md): effect objects carry the
# full 64-bit AIENOS capability generation. Physics-free: the Omega core, the
# Visor effect-request adapter, and the pinned AIENOS header + library
# (aienos.lock) used directly. Prints OMEGA_EFFECT_CAP64_{ROUNDTRIP,IDENTITY,
# STALE_REJECT,AUTHORITY}_PASS gate lines.
EFFECT_CAP64_CAP_LIB ?= $(AIENOS_CAP_LIB)
EFFECT_CAP64_CAP_INC ?= $(AIENOS_R7_DIR)/native/capability
EFFECT_CAP64_TEST = $(OUT_DIR)/tests-effect/test_effect_cap64
EFFECT_CAP64_OBJS = $(addprefix $(OUT_DIR)/,sha256.o omega_canonical.o omega_validate.o omega_core.o omega_codec.o)

$(EFFECT_CAP64_TEST): tests/effect/test_effect_cap64.c $(EFFECT_CAP64_OBJS) \
		$(OUT_DIR)/visor/visor_effect_request.o src/visor/visor_effect_request.h \
		src/omega_core.h src/omega_canonical.h src/omega_types.h $(EFFECT_CAP64_CAP_LIB)
	mkdir -p $(dir $@)
	$(CC) $(VISOR_CFLAGS) -I$(EFFECT_CAP64_CAP_INC) -pthread -o $@ tests/effect/test_effect_cap64.c \
		$(OUT_DIR)/visor/visor_effect_request.o $(EFFECT_CAP64_OBJS) $(EFFECT_CAP64_CAP_LIB) -lm

.PHONY: test-effect-cap64
test-effect-cap64: $(EFFECT_CAP64_TEST)
	./$(EFFECT_CAP64_TEST)

$(RX_R7_TEST): $(RX_R7_SRCS) src/runtime/rx_caproot.h src/runtime/rx_world.h \
	src/runtime/aienos_cap.h src/runtime/omega_shared_world_abi.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R7_SRCS) $(AIENOS_CAP_LIB) -lm

test-r7: $(RX_R7_TEST)
	./$(RX_R7_TEST)

# R9: generation barrier. The candidate is prepared beside the live world.
# A stop after each persistence step must recover one whole generation.
RX_R9_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_generation.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_r9_barrier.c
RX_R9_TEST = $(OUT_DIR)/rx_r9_barrier_test

$(RX_R9_TEST): $(RX_R9_SRCS) src/runtime/rx_generation.h src/runtime/rx_caproot.h \
	src/runtime/rx_world.h src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R9_SRCS) $(AIENOS_CAP_LIB) -lm

test-r9: $(RX_R9_TEST)
	./$(RX_R9_TEST)

# R12 host rules against the native AIENOS authority. The graphics processor is
# not started. A pass here does not claim the resident-seat gate.
RX_R12_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_r12_resident.c
RX_R12_TEST = $(OUT_DIR)/rx_r12_resident_test

$(RX_R12_TEST): $(RX_R12_SRCS) src/runtime/rx_caproot.h src/runtime/rx_world.h \
	src/runtime/aienos_cap.h src/runtime/omega_shared_world_abi.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R12_SRCS) $(AIENOS_CAP_LIB) -lm

test-r12: $(RX_R12_TEST)
	./$(RX_R12_TEST)

# R16 G6 operator emergency stop (spec/r16-operator-emergency-stop.md). Host only,
# real capability root: authority refusals, in-flight and racing stops, the
# generation store under a stop, the durable mark across a restart, and resume.
# The mutant script then removes each guard in turn from a temporary copy and
# requires the test to fail against every one. The resident-seat case lives in
# test-r12 (t_emergency_stop, native AIENOS authority).
RX_EMERGENCY_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_generation.c src/sha256.c tests/runtime/rx_emergency.c
RX_EMERGENCY_TEST = $(OUT_DIR)/rx_emergency_test

$(RX_EMERGENCY_TEST): $(RX_EMERGENCY_SRCS) src/runtime/rx_caproot.h src/runtime/rx_world.h \
	src/runtime/rx_generation.h src/runtime/rx_caller.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_EMERGENCY_SRCS) -lm

.PHONY: test-rx-emergency
test-rx-emergency: $(RX_EMERGENCY_TEST)
	./$(RX_EMERGENCY_TEST)
	CC="$(CC)" bash tests/runtime/rx_emergency_mutants.sh

# R10: Omega as a resident realization faculty. Omega's native AArch64 matvec
# realizations become ready from cost evidence, are verified in a sandbox,
# measured, and recorded; production picks the record up. AArch64 hosts only.
RX_R10_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_omega.c src/sha256.c src/omega_evidence.c \
	src/omega_canonical.c src/omega_validate.c src/omega_core.c src/omega_codec.c \
	src/aarch64_encoder.c src/aarch64_decoder.c src/omega_realize.c src/omega_realize_synth.c src/omega_program.c \
	src/omega_machine.c src/omega_exec.c src/omega_verify.c src/omega_matvec.c src/omega_matvec_quad.c \
	tests/runtime/rx_r10_omega.c
RX_R10_TEST = $(OUT_DIR)/rx_r10_omega_test

$(RX_R10_TEST): $(RX_R10_SRCS) src/runtime/rx_omega.h src/runtime/rx_caproot.h \
	src/runtime/rx_world.h src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R10_SRCS) $(AIENOS_CAP_LIB) -lm

test-r10: $(RX_R10_TEST)
	./$(RX_R10_TEST)

# R8: AEGIS resident authority. Capability slots in the world; AEGIS decides,
# only root.install mints, through the native AIENOS authority.
RX_R8_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_r8_aegis.c
RX_R8_TEST = $(OUT_DIR)/rx_r8_aegis_test

$(RX_R8_TEST): $(RX_R8_SRCS) src/runtime/rx_aegis.h src/runtime/rx_caproot.h \
	src/runtime/rx_world.h src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R8_SRCS) $(AIENOS_CAP_LIB) -lm

test-r8: $(RX_R8_TEST)
	./$(RX_R8_TEST)

# R11: AIEN as a resident cognitive faculty. Part A drives AIEN alone on any
# host; part B is the living run with Omega, on hosts that have both Cortex-X925
# and Cortex-A725 cores. rx_aien.o is built alone first and must not reference
# any Omega symbol: AIEN reaches Omega only through world objects.
RX_R11_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_omega.c src/sha256.c src/omega_evidence.c \
	src/omega_canonical.c src/omega_validate.c src/omega_core.c src/omega_codec.c \
	src/aarch64_encoder.c src/aarch64_decoder.c src/omega_realize.c src/omega_realize_synth.c src/omega_program.c \
	src/omega_machine.c src/omega_exec.c src/omega_verify.c src/omega_matvec.c src/omega_matvec_quad.c \
	tests/runtime/rx_r11_aien.c
RX_R11_TEST = $(OUT_DIR)/rx_r11_aien_test
RX_AIEN_OBJ = $(OUT_DIR)/rx_aien.o

$(RX_AIEN_OBJ): src/runtime/rx_aien.c src/runtime/rx_aien.h src/runtime/rx_world.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_aien.c
	@if nm -u $@ | grep -Ei 'omega' ; then \
		echo "rx_aien.o references Omega directly; AIEN must reach it only through the world"; \
		rm -f $@; exit 1; fi

$(RX_R11_TEST): $(RX_R11_SRCS) $(RX_AIEN_OBJ) src/runtime/rx_omega.h src/runtime/rx_caproot.h \
	src/runtime/rx_world.h src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R11_SRCS) $(RX_AIEN_OBJ) $(AIENOS_CAP_LIB) -lm

test-r11: $(RX_R11_TEST)
	./$(RX_R11_TEST)

# R11 living run allowed inside its own quietlock hold, refused under any other.
.PHONY: test-r11-own-hold
test-r11-own-hold: $(RX_R11_TEST)
	sh tests/runtime/r11_own_hold_test.sh ./$(RX_R11_TEST)

# OMEGA_STATE_PROJECTION: cognition gets a compiled state projection from
# Cortex, not everything Cortex knows. Part B drives the real R11 AIEN faculty
# under the native AIENOS authority. The projection objects are built alone
# first and may reference nothing but Cortex and SHA-256: no world, no faculty,
# no Omega realization code.
RX_SP_OBJS = $(OUT_DIR)/rx_cortex.o $(OUT_DIR)/rx_projection.o
RX_SP_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_sp_workloads.c tests/runtime/rx_state_projection.c
RX_SP_TEST = $(OUT_DIR)/rx_state_projection_test

$(OUT_DIR)/rx_cortex.o: src/runtime/rx_cortex.c src/runtime/rx_cortex.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_cortex.c

$(OUT_DIR)/rx_projection.o: src/runtime/rx_projection.c src/runtime/rx_projection.h \
	src/runtime/rx_cortex.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_projection.c
	@if nm -u $@ $(OUT_DIR)/rx_cortex.o | grep -E ' (rx_world|rx_aien|rx_omega|omega_)' ; then \
		echo "the projection references the world, a faculty or Omega realization code"; \
		rm -f $@; exit 1; fi

$(RX_SP_TEST): $(RX_SP_SRCS) $(RX_SP_OBJS) $(RX_AIEN_OBJ) tests/runtime/rx_sp_workloads.h \
	src/runtime/rx_world.h src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_SP_SRCS) $(RX_SP_OBJS) $(RX_AIEN_OBJ) $(AIENOS_CAP_LIB) -lm

test-state-projection: $(RX_SP_TEST)
	./$(RX_SP_TEST)

# M20 Cortex canonicalization: canonical Cortex contract, journal, single
# writer, typed recall, and World execution recorded through rx_cortex_record.
RX_CX_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_cortex_record.c src/sha256.c tests/runtime/rx_cortex_canon.c
RX_CX_TEST = $(OUT_DIR)/rx_cortex_canon_test

$(RX_CX_TEST): $(RX_CX_SRCS) $(OUT_DIR)/rx_cortex.o src/runtime/rx_cortex_record.h \
	src/runtime/rx_world.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_CX_SRCS) $(OUT_DIR)/rx_cortex.o

test-cortex: $(RX_CX_TEST)
	./$(RX_CX_TEST)

# Physical graphics seat against the native AIENOS authority. Not part of
# GitHub checks. A pass on this machine is the only run that may set
# silicon_observed.
RX_R12_SILICON_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c \
	src/runtime/rx_coherent.c src/runtime/rx_native_bind.c \
	src/runtime/rx_resident_gpu.c src/sha256.c src/omega_evidence.c \
	src/omega_blackwell_codegen.c src/omega_blackwell_encoder.c \
	src/omega_blackwell_qmd.c src/omega_blackwell_matmul.c \
	$(PHYSICS_DIR)/m16/m16_native.c $(PHYSICS_DIR)/nvrm/nvrm.c \
	tests/runtime/rx_r12_silicon.c
RX_R12_SILICON = $(OUT_DIR)/rx_r12_silicon_test

$(RX_R12_SILICON): $(RX_R12_SILICON_SRCS) src/runtime/rx_world.h \
	src/runtime/rx_resident_gpu.h src/runtime/rx_caproot.h src/runtime/aienos_cap.h \
	src/omega_blackwell_codegen.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R12_SILICON_SRCS) $(AIENOS_CAP_LIB) -ldl -lm

test-r12-silicon: $(RX_R12_SILICON)
	./$(RX_R12_SILICON)

# COMPOSITION-2 in the living system (WP-A): the composition modules are part
# of the canonical living build, so every R13/R14/R15/R16 binary links them.
# rx_graph.o, rx_capq.o and rx_skillroute.o keep their own no-mint symbol
# checks; the living host build depends on them so the checks run with it.
RX_COMPOSE_LIVING_SRCS = src/runtime/aien_machine_id.c src/runtime/rx_jspace.c \
	src/runtime/rx_cortex.c src/runtime/rx_cortex_record.c src/runtime/rx_graph.c \
	src/runtime/rx_capq.c src/runtime/rx_skillroute.c src/runtime/rx_compose.c
RX_COMPOSE_LIVING_CHECKS = $(OUT_DIR)/rx_graph.o $(OUT_DIR)/rx_capq.o $(OUT_DIR)/rx_skillroute.o \
	src/runtime/rx_compose.h

# Lane 32: ONE test-build flag, AIEN_TEST_BUILD, off by default. It alone
# admits the test-only pieces: the Fabric F5-0 loopback transport, the HMAC
# stand-in authenticator and the in-process dispatcher (src/fabric, Lane 13),
# the Fabric living phase with its fixed test keys (tests/fabric/
# fab_living_phase.h), the composition fixture and the rx_compose fault and
# rogue-candidate hooks (RXC_TEST_HOOKS, implied by the flag; alone it is an
# #error). Their headers refuse to compile without it. The PRODUCTION program
# (rx_r13_living_host/_silicon, test-r13-host/-silicon) never sets it, links
# ARGUS and is checked by test-prod-hygiene; the TEST BUILD variant
# (rx_r13_living_testbuild_*, test-r13-testbuild-*) carries the composition
# and Fabric phases and reports R13_LIVING_SYSTEM_TEST_BUILD, never the gate.
# docs/r16-production-entry-point.md has the table.
AIEN_TEST_FLAGS = -DAIEN_TEST_BUILD=1
RX_FABRIC_TEST_SRCS = src/fabric/fabric.c src/fabric/fab_hmac.c src/fabric/fab_loopback.c \
	src/fabric/fab_dispatch.c
RX_R13_TEST_CHECKS = tests/runtime/rx_compose_fixture.h src/fabric/fab_dispatch.h \
	src/fabric/fab_loopback.h src/fabric/fab_hmac.h tests/fabric/fab_living_phase.h
# The production build refuses the flag outright.
RX_PROD_REFUSE_TEST = $(if $(findstring AIEN_TEST_BUILD,$(CFLAGS))$(findstring RXC_TEST_HOOKS,$(CFLAGS)),\
	$(error the production program never builds with AIEN_TEST_BUILD or RXC_TEST_HOOKS in CFLAGS))

# R13 host uses the R12 processor stand-in and cannot claim the silicon gate.
# R13 silicon runs the same world against the physical resident GB10 seat.
# RX_R13_SRCS is the PRODUCTION source set (no test piece); the R14/R15 rigs
# filter rx_r13_living.c out of it.
RX_R13_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c \
	src/runtime/rx_coherent.c src/runtime/rx_native_bind.c \
	src/runtime/rx_aegis.c src/runtime/rx_aien.c src/runtime/rx_omega.c \
	src/runtime/rx_generation.c src/runtime/rx_living.c \
	src/runtime/rx_operator.c \
	src/sha256.c src/omega_evidence.c src/omega_canonical.c \
	src/omega_validate.c src/omega_core.c src/omega_codec.c \
	src/aarch64_encoder.c src/aarch64_decoder.c src/omega_realize.c \
	src/omega_realize_synth.c src/omega_program.c src/omega_machine.c src/omega_exec.c \
	src/omega_verify.c src/omega_matvec.c src/omega_matvec_quad.c \
	$(RX_COMPOSE_LIVING_SRCS) \
	tests/runtime/rx_r13_living.c
RX_R13_TEST_SRCS = $(RX_R13_SRCS) $(RX_FABRIC_TEST_SRCS)
RX_R13_SILICON_EXTRA = src/runtime/rx_resident_gpu.c src/omega_blackwell_codegen.c \
	src/omega_blackwell_encoder.c src/omega_blackwell_qmd.c src/omega_blackwell_matmul.c \
	$(PHYSICS_DIR)/m16/m16_native.c $(PHYSICS_DIR)/nvrm/nvrm.c
# Production program (rules after the ARGUS block below: they link ARGUS).
RX_R13_HOST = $(OUT_DIR)/rx_r13_living_host
RX_R13_SILICON = $(OUT_DIR)/rx_r13_living_silicon
# Test-build variant (no ARGUS).
RX_R13_TEST_HOST = $(OUT_DIR)/rx_r13_living_testbuild_host
RX_R13_TEST_SILICON = $(OUT_DIR)/rx_r13_living_testbuild_silicon

$(RX_R13_TEST_HOST): $(RX_R13_TEST_SRCS) src/runtime/rx_living.h $(RX_COMPOSE_LIVING_CHECKS) \
	$(RX_R13_TEST_CHECKS) $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) $(AIEN_TEST_FLAGS) -pthread -o $@ $(RX_R13_TEST_SRCS) $(AIENOS_CAP_LIB) -lm

# Lane 17: the Fabric phase runs in the silicon test build too; its Fabric part
# is CPU-only loopback. The composition phase stays host-only.
$(RX_R13_TEST_SILICON): $(RX_R13_TEST_SRCS) $(RX_R13_SILICON_EXTRA) src/runtime/rx_living.h \
	$(RX_COMPOSE_LIVING_CHECKS) $(RX_R13_TEST_CHECKS) $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) $(AIEN_TEST_FLAGS) -DR13_SILICON -pthread -o $@ $(RX_R13_TEST_SRCS) \
		$(RX_R13_SILICON_EXTRA) $(AIENOS_CAP_LIB) -ldl -lm

test-r13-host: $(RX_R13_HOST)
	./$(RX_R13_HOST)

test-r13-silicon: $(RX_R13_SILICON)
	./$(RX_R13_SILICON)

test-r13-testbuild-host: $(RX_R13_TEST_HOST)
	./$(RX_R13_TEST_HOST)

test-r13-testbuild-silicon: $(RX_R13_TEST_SILICON)
	./$(RX_R13_TEST_SILICON)

# OMEGA_BRANCH_STATE_REUSE: J-Space branches sharing one semantic prefix,
# shared-state realization versus independent recomputation, FORGE placement.
RX_BRANCH_REUSE_SRCS = src/runtime/rx_jspace.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_branch_reuse.c
RX_BRANCH_REUSE_TEST = $(OUT_DIR)/rx_branch_reuse

$(RX_BRANCH_REUSE_TEST): $(RX_BRANCH_REUSE_SRCS) src/runtime/rx_jspace.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ $(RX_BRANCH_REUSE_SRCS) -lm

test-branch-reuse: $(RX_BRANCH_REUSE_TEST)
	./$(RX_BRANCH_REUSE_TEST)

# M20 production J-Space mechanics: slab/extent reclamation, generation-safe
# references, limits, concurrency, durable reopen/crash/torn metadata, World
# commit compatibility. Fast; runs in CI.
RX_JSPACE_PROD_SRCS = src/runtime/rx_jspace.c src/runtime/rx_caproot.c src/runtime/rx_world.c \
	src/runtime/rx_coherent.c src/sha256.c src/omega_evidence.c tests/runtime/rx_jspace_prod.c
RX_JSPACE_PROD_TEST = $(OUT_DIR)/rx_jspace_prod

$(RX_JSPACE_PROD_TEST): $(RX_JSPACE_PROD_SRCS) src/runtime/rx_jspace.h src/runtime/rx_world.h \
	src/runtime/rx_caproot.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_JSPACE_PROD_SRCS) -lm

test-jspace-prod: $(RX_JSPACE_PROD_TEST)
	./$(RX_JSPACE_PROD_TEST)

# R14: the R13 organism attacked while alive. Host uses the R12 processor
# stand-in and cannot claim the gate; silicon runs D and E on the GB10 seat.
RX_R14_SRCS = $(filter-out tests/runtime/rx_r13_living.c src/runtime/rx_operator.c,$(RX_R13_SRCS)) \
	tests/runtime/rx_r14_recovery.c
RX_R14_HOST = $(OUT_DIR)/rx_r14_recovery_host
RX_R14_SILICON = $(OUT_DIR)/rx_r14_recovery_silicon

$(RX_R14_HOST): $(RX_R14_SRCS) src/runtime/rx_living.h $(RX_COMPOSE_LIVING_CHECKS) $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R14_SRCS) $(AIENOS_CAP_LIB) -lm

$(RX_R14_SILICON): $(RX_R14_SRCS) src/runtime/rx_resident_gpu.c \
	src/omega_blackwell_codegen.c src/omega_blackwell_encoder.c \
	src/omega_blackwell_qmd.c src/omega_blackwell_matmul.c \
	$(PHYSICS_DIR)/m16/m16_native.c $(PHYSICS_DIR)/nvrm/nvrm.c \
	$(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -DR14_SILICON -pthread -o $@ $(RX_R14_SRCS) \
		src/runtime/rx_resident_gpu.c src/omega_blackwell_codegen.c \
		src/omega_blackwell_encoder.c src/omega_blackwell_qmd.c \
		src/omega_blackwell_matmul.c $(PHYSICS_DIR)/m16/m16_native.c \
		$(PHYSICS_DIR)/nvrm/nvrm.c $(AIENOS_CAP_LIB) -ldl -lm

test-r14-host: $(RX_R14_HOST)
	./$(RX_R14_HOST)

test-r14-silicon: $(RX_R14_SILICON)
	./$(RX_R14_SILICON)

# R15: instrumentation checks (host). Every counter the R15 harness reduces
# is checked against a case whose true value is known in advance.
RX_R15_INSTR_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_generation.c src/sha256.c src/omega_evidence.c \
	src/runtime/rx_seq_reference.c tests/runtime/rx_r15_instr.c
RX_R15_INSTR = $(OUT_DIR)/rx_r15_instr_test

$(RX_R15_INSTR): $(RX_R15_INSTR_SRCS) src/runtime/rx_world.h src/runtime/rx_generation.h \
	src/runtime/rx_caproot.h src/runtime/rx_seq_reference.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R15_INSTR_SRCS)

test-r15-instr: $(RX_R15_INSTR)
	./$(RX_R15_INSTR)

# R15: SEQ semantic parity gate (spec §16 C1 item 9). The R13 body is built
# by tests/runtime/rx_r15_rig.c for RES-1 and for the sequential reference.
RX_R15_RIG_SRCS = $(filter-out tests/runtime/rx_r13_living.c src/runtime/rx_operator.c,$(RX_R13_SRCS)) \
	src/runtime/rx_seq_reference.c tests/runtime/rx_r15_rig.c
RX_R15_RIG_HDRS = src/runtime/rx_living.h src/runtime/rx_seq_reference.h \
	src/runtime/rx_world.h tests/runtime/rx_r15_rig.h
RX_R15_GPU_SRCS = src/runtime/rx_resident_gpu.c src/omega_blackwell_codegen.c \
	src/omega_blackwell_encoder.c src/omega_blackwell_qmd.c \
	src/omega_blackwell_matmul.c $(PHYSICS_DIR)/m16/m16_native.c $(PHYSICS_DIR)/nvrm/nvrm.c
RX_R15_PARITY_HOST = $(OUT_DIR)/rx_r15_parity_host
RX_R15_PARITY_SILICON = $(OUT_DIR)/rx_r15_parity_silicon

$(RX_R15_PARITY_HOST): $(RX_R15_RIG_SRCS) tests/runtime/rx_r15_parity.c $(RX_R15_RIG_HDRS) \
	$(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R15_RIG_SRCS) tests/runtime/rx_r15_parity.c \
		$(AIENOS_CAP_LIB) -lm

$(RX_R15_PARITY_SILICON): $(RX_R15_RIG_SRCS) tests/runtime/rx_r15_parity.c $(RX_R15_RIG_HDRS) \
	$(RX_R15_GPU_SRCS) $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -DR15_SILICON -pthread -o $@ $(RX_R15_RIG_SRCS) \
		tests/runtime/rx_r15_parity.c $(RX_R15_GPU_SRCS) $(AIENOS_CAP_LIB) -ldl -lm

test-r15-parity-host: $(RX_R15_PARITY_HOST)
	./$(RX_R15_PARITY_HOST)

# R15 G7: production keeps its worker through a generation promotion; the R9
# store's physical work runs on the store's durable executor (host seat).
RX_R15_G7_HOST = $(OUT_DIR)/rx_r15_g7_host
$(RX_R15_G7_HOST): $(RX_R15_RIG_SRCS) tests/runtime/rx_r15_g7.c $(RX_R15_RIG_HDRS) \
	$(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R15_RIG_SRCS) tests/runtime/rx_r15_g7.c \
		$(AIENOS_CAP_LIB) -lm
test-r15-g7-host: $(RX_R15_G7_HOST)
	./$(RX_R15_G7_HOST)

# R15 C2 sensor validation: bounded GB10 load through the resident seat.
R15_GPU_LOAD = $(OUT_DIR)/r15_gpu_load
R15_GPU_LOAD_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/sha256.c src/omega_evidence.c \
	tests/runtime/r15_gpu_load.c

$(R15_GPU_LOAD): $(R15_GPU_LOAD_SRCS) $(RX_R15_GPU_SRCS) src/runtime/rx_world.h \
	$(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(R15_GPU_LOAD_SRCS) $(RX_R15_GPU_SRCS) \
		$(AIENOS_CAP_LIB) -ldl -lm

r15-gpu-load: $(R15_GPU_LOAD)

test-r15-parity-silicon: $(RX_R15_PARITY_SILICON)
	./$(RX_R15_PARITY_SILICON)

# R15 harness (spec §4, §5, §12). Four binaries from the same sources and
# flags: production and the RES-1-NODIGEST measurement build (§3), host
# stand-in seat and GB10 silicon. tools/r15_qualify.sh drives them and
# tools/r15_reduce.c reduces their raw output.
RX_R15_PERF_SRCS = $(RX_R15_RIG_SRCS) tests/runtime/r15_measure.c tests/runtime/rx_r15_perf.c
RX_R15_PERF_HDRS = $(RX_R15_RIG_HDRS) tests/runtime/r15_measure.h
RX_R15_PERF_HOST = $(OUT_DIR)/rx_r15_perf_host
RX_R15_PERF_HOST_ND = $(OUT_DIR)/rx_r15_perf_host_nodigest
RX_R15_PERF_SILICON = $(OUT_DIR)/rx_r15_perf_silicon
RX_R15_PERF_SILICON_ND = $(OUT_DIR)/rx_r15_perf_silicon_nodigest
R15_REDUCE = $(OUT_DIR)/r15_reduce

$(RX_R15_PERF_HOST): $(RX_R15_PERF_SRCS) $(RX_R15_PERF_HDRS) $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R15_PERF_SRCS) $(AIENOS_CAP_LIB) -ldl -lm

$(RX_R15_PERF_HOST_ND): $(RX_R15_PERF_SRCS) $(RX_R15_PERF_HDRS) $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -DRX_MEASURE_NO_CAUSAL_DIGEST -pthread -o $@ $(RX_R15_PERF_SRCS) \
		$(AIENOS_CAP_LIB) -ldl -lm

$(RX_R15_PERF_SILICON): $(RX_R15_PERF_SRCS) $(RX_R15_PERF_HDRS) $(RX_R15_GPU_SRCS) \
	$(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -DR15_SILICON -pthread -o $@ $(RX_R15_PERF_SRCS) $(RX_R15_GPU_SRCS) \
		$(AIENOS_CAP_LIB) -ldl -lm

$(RX_R15_PERF_SILICON_ND): $(RX_R15_PERF_SRCS) $(RX_R15_PERF_HDRS) $(RX_R15_GPU_SRCS) \
	$(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -DR15_SILICON -DRX_MEASURE_NO_CAUSAL_DIGEST -pthread -o $@ \
		$(RX_R15_PERF_SRCS) $(RX_R15_GPU_SRCS) $(AIENOS_CAP_LIB) -ldl -lm

$(R15_REDUCE): tools/r15_reduce.c src/sha256.c src/sha256.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ tools/r15_reduce.c src/sha256.c -lm

r15-perf-host: $(RX_R15_PERF_HOST) $(RX_R15_PERF_HOST_ND) $(R15_REDUCE)
r15-perf-silicon: $(RX_R15_PERF_SILICON) $(RX_R15_PERF_SILICON_ND) $(R15_REDUCE)

# R15 receipt writer (spec §12/§13/§14): tools/r15_receipt.sh turns one run
# directory (summary.json + machine.json + SHA256SUMS) into
# evidence/R15/<sha256>.json. The host test runs it on attempt 1 (a FAIL).
r15-receipt:
	@test -n "$(RUN)" || { echo "usage: make r15-receipt RUN=evidence/R15/raw/<run-id> [CANDIDATE=<commit>] [RERUNS=<file>] [NOTES=<file>]"; exit 2; }
	tools/r15_receipt.sh $(RUN) evidence/R15 "$(CANDIDATE)" "$(RERUNS)" "$(NOTES)"

.PHONY: r15-receipt test-r15-receipt
test-r15-receipt:
	tests/r15_receipt_test.sh

# R15 preflight parser: counts only the CPU PMU cycles event (SMMU PMUs list
# "cycles" too on newer kernels). Host-only, fixtures, seconds.
.PHONY: test-r15-preflight
test-r15-preflight:
	sh tests/r15_preflight_test.sh
	sh tests/r15_energy_preflight_test.sh

# G15 diagnostics: sampler lateness / not-live-run record, its reducer report,
# and the readable-kernel-log Xid scan. Host-only, no chip.
.PHONY: test-r15-g15-diag
test-r15-g15-diag: $(R15_REDUCE) | $(OUT_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Itests/runtime -o $(OUT_DIR)/r15_seat_diag_test tests/runtime/r15_seat_diag_test.c
	$(OUT_DIR)/r15_seat_diag_test
	R15_REDUCE=$(R15_REDUCE) sh tests/r15_g15_diag_test.sh
	sh tests/seat_marker_uncached_test.sh

# R16-G2 code-search gate (spec/r16-orchestrator-retirement.md). Host-only C tool,
# seconds, no network. Scans the five repos (paths from R16_REPO_OMEGA,
# R16_REPO_SOVEREIGN_CORE, R16_REPO_AEGIS_RUNTIME, R16_REPO_AIENOS,
# R16_REPO_PHYSICS; defaults: this tree and ~/workspace/r16-survey/<repo>) and
# joins every loop-shaped site with spec/r16-orchestrator-retirement-map.md.
# Exit 0 only when every site is classified A-F, no omega class-A site is
# still under a production name, and all five repos were scanned.
R16_INVENTORY = $(OUT_DIR)/r16_loop_inventory
$(R16_INVENTORY): tools/r16_loop_inventory.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ tools/r16_loop_inventory.c

.PHONY: r16-inventory test-r16-inventory
r16-inventory: $(R16_INVENTORY)
	$(R16_INVENTORY) --map spec/r16-orchestrator-retirement-map.md --json $(OUT_DIR)/r16-inventory.json

test-r16-inventory: $(R16_INVENTORY)
	sh tests/r16_inventory/run.sh $(R16_INVENTORY)

# Lane 32 follow-up: dry-mode self-test of tools/r16_qualify.sh (no chip, no build).
.PHONY: test-r16-qualify-selftest
test-r16-qualify-selftest:
	bash tests/r16_qualify/run.sh

# The aienos.lock source proof (tools/aienos_lock_source.sh) and the authority build rule
# that uses it: fixture repositories, host only.
.PHONY: test-aienos-lock-source
test-aienos-lock-source:
	bash tests/aienos_lock_source/run.sh

# R16-G3: the authoritative path with the legacy orchestrators unavailable.
# Link map, shared libraries, embedded names and an exec trace of the R13
# living system and the R14 recovery run, legacy programs stubbed on PATH.
# The R13 binary is the PRODUCTION program (Lane 32: no test piece, ARGUS
# linked); its source list includes the pinned ARGUS sources.
# Host mode uses the processor stand-in and cannot claim the gate. The silicon
# target starts the GB10 seat: run it only as part of the qualification
# ladder, detached, never under `timeout` and never killed.
R16_STAMP := $(shell date -u +%Y%m%dT%H%M%SZ)
.PHONY: test-r16-authpath test-r16-authpath-silicon
test-r16-authpath: $(RX_R13_HOST) $(RX_R14_HOST)
	sh tools/r16_authpath.sh host $(RX_R13_HOST) $(RX_R14_HOST) \
		$(OUT_DIR)/r16/authpath/host-$(R16_STAMP) $(RX_R13_SRCS) $(RX_PROD_ARGUS_SRCS) $(AIENOS_CAP_LIB)

test-r16-authpath-silicon: $(RX_R13_SILICON) $(RX_R14_SILICON)
	sh tools/r16_authpath.sh silicon $(RX_R13_SILICON) $(RX_R14_SILICON) \
		$(OUT_DIR)/r16/authpath/silicon-$(R16_STAMP) $(RX_R13_SRCS) $(RX_PROD_ARGUS_SRCS) \
		src/runtime/rx_resident_gpu.c src/omega_blackwell_codegen.c \
		src/omega_blackwell_encoder.c src/omega_blackwell_qmd.c \
		src/omega_blackwell_matmul.c $(PHYSICS_DIR)/m16/m16_native.c \
		$(PHYSICS_DIR)/nvrm/nvrm.c $(AIENOS_CAP_LIB)

# R16-G3 checks 1-3 only (sources, link map, shared libraries, embedded names)
# on the current living-system build, for CI runners. Nothing is executed, so
# neither the GB10 seat nor the Spark core classes are needed; the gate line
# stays NOT_RUN. The -silicon variant builds the silicon binaries and never
# runs them. The gate itself is still test-r16-authpath(-silicon).
.PHONY: test-r16-authpath-linkmap test-r16-authpath-linkmap-silicon
test-r16-authpath-linkmap: $(RX_R13_HOST) $(RX_R14_HOST)
	R16_G3_STATIC_ONLY=1 sh tools/r16_authpath.sh host $(RX_R13_HOST) $(RX_R14_HOST) \
		$(OUT_DIR)/r16/authpath/linkmap-host-$(R16_STAMP) $(RX_R13_SRCS) $(RX_PROD_ARGUS_SRCS) $(AIENOS_CAP_LIB)

test-r16-authpath-linkmap-silicon: $(RX_R13_SILICON) $(RX_R14_SILICON)
	R16_G3_STATIC_ONLY=1 sh tools/r16_authpath.sh silicon $(RX_R13_SILICON) $(RX_R14_SILICON) \
		$(OUT_DIR)/r16/authpath/linkmap-silicon-$(R16_STAMP) $(RX_R13_SRCS) $(RX_PROD_ARGUS_SRCS) \
		src/runtime/rx_resident_gpu.c src/omega_blackwell_codegen.c \
		src/omega_blackwell_encoder.c src/omega_blackwell_qmd.c \
		src/omega_blackwell_matmul.c $(PHYSICS_DIR)/m16/m16_native.c \
		$(PHYSICS_DIR)/nvrm/nvrm.c $(AIENOS_CAP_LIB)

# R16-G4: legacy paths cannot bypass authority. The R13 body (R15 rig, RES-4,
# host seat) is started; a legacy context tries six acts and each must be
# refused with no change to authoritative state. Host only; no chip.
RX_R16_NEGATIVE = $(OUT_DIR)/rx_r16_negative
$(RX_R16_NEGATIVE): $(RX_R15_RIG_SRCS) tests/runtime/rx_r16_negative.c $(RX_R15_RIG_HDRS) \
	$(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R15_RIG_SRCS) tests/runtime/rx_r16_negative.c \
		$(AIENOS_CAP_LIB) -lm

.PHONY: test-r16-negative test-r16-negative-mutants
test-r16-negative: $(RX_R16_NEGATIVE)
	./$(RX_R16_NEGATIVE)

# "Removing any one guard turns the test red": rebuilds the G4 test against
# scratch copies with one guard removed at a time; each must FAIL. Minutes.
test-r16-negative-mutants: $(RX_R16_NEGATIVE)
	R16_CAP_LIB="$(AIENOS_CAP_LIB)" sh tests/r16_negative/mutate.sh "$(CC)" "$(CFLAGS)" "$(AIENOS_R7_DIR)" \
		$(RX_R15_RIG_SRCS) tests/runtime/rx_r16_negative.c

# R16-G5 API/build surface: legacy modes only under explicit names, the SEQ
# loop only in rx_seq_reference.*, no legacy default mode, production entry
# point documented (docs/r16-production-entry-point.md). Host only, seconds.
.PHONY: test-r16-surface
test-r16-surface: $(TARGET)
	sh tests/r16_surface/run.sh ./$(TARGET)


# OMEGA_ACTION_GRAPH_IR: goals compile to typed action graphs that run as
# resident reactions by readiness alone. rx_graph.o is built alone first and
# must not reference any AIENOS admin operation: compilation can find that
# authority is missing, never create it.
RX_GRAPH_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/sha256.c src/omega_evidence.c \
	src/omega_core.c src/omega_canonical.c tests/runtime/rx_action_graph.c
RX_GRAPH_TEST = $(OUT_DIR)/rx_action_graph_test
RX_GRAPH_OBJ = $(OUT_DIR)/rx_graph.o

$(RX_GRAPH_OBJ): src/runtime/rx_graph.c src/runtime/rx_graph.h src/runtime/rx_world.h \
	src/runtime/rx_aegis.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_graph.c
	@if nm -u $@ | grep -E 'aienos_cap_|rx_caproot_mint|rx_caproot_revoke' ; then \
		echo "rx_graph.o references an authority admin operation; compilation must not mint"; \
		rm -f $@; exit 1; fi

$(RX_GRAPH_TEST): $(RX_GRAPH_SRCS) $(RX_GRAPH_OBJ) src/runtime/rx_caproot.h \
	src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_GRAPH_SRCS) $(RX_GRAPH_OBJ) $(AIENOS_CAP_LIB) -lm

test-action-graph: $(RX_GRAPH_TEST)
	./$(RX_GRAPH_TEST)

# OMEGA_CAPABILITY_QUERY: AIEN states a CapabilityNeed; Omega compiles it into
# probes of the capability sources and returns a bounded list of candidates
# ranked on explicit dimensions. rx_capq.o is built alone first and must not
# reference any AIENOS admin operation: a query can learn that authority is
# held or missing, never create it.
RX_CAPQ_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/sha256.c src/omega_evidence.c \
	src/omega_core.c src/omega_canonical.c src/runtime/aien_machine_id.c \
	tests/runtime/rx_capability_query.c
RX_CAPQ_TEST = $(OUT_DIR)/rx_capability_query_test
RX_CAPQ_OBJ = $(OUT_DIR)/rx_capq.o

$(RX_CAPQ_OBJ): src/runtime/rx_capq.c src/runtime/rx_capq.h src/runtime/rx_graph.h \
	src/runtime/rx_world.h src/runtime/aien_machine_id.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_capq.c
	@if nm -u $@ | grep -E 'aienos_cap_|rx_caproot_mint|rx_caproot_revoke' ; then \
		echo "rx_capq.o references an authority admin operation; a query must not mint"; \
		rm -f $@; exit 1; fi

$(RX_CAPQ_TEST): $(RX_CAPQ_SRCS) $(RX_CAPQ_OBJ) $(RX_GRAPH_OBJ) src/runtime/rx_caproot.h \
	src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_CAPQ_SRCS) $(RX_CAPQ_OBJ) $(RX_GRAPH_OBJ) \
		$(AIENOS_CAP_LIB) -lm

test-capability-query: $(RX_CAPQ_TEST)
	./$(RX_CAPQ_TEST)

# M20 canonical Capability Graph and Skill Router: rx_capq promoted (keys,
# update/withdraw, canonical machine identity, wire form) plus rx_skillroute.
# Like rx_capq.o, rx_skillroute.o must not reference an authority admin
# operation: routing discovers, it never authorizes.
RX_CAPGRAPH_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/sha256.c src/omega_evidence.c \
	src/omega_core.c src/omega_canonical.c src/runtime/aien_machine_id.c \
	tests/runtime/rx_capability_graph.c
RX_CAPGRAPH_TEST = $(OUT_DIR)/rx_capability_graph_test
RX_SKILLROUTE_OBJ = $(OUT_DIR)/rx_skillroute.o

$(RX_SKILLROUTE_OBJ): src/runtime/rx_skillroute.c src/runtime/rx_skillroute.h \
	src/runtime/rx_capq.h src/runtime/aien_machine_id.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_skillroute.c
	@if nm -u $@ | grep -E 'aienos_cap_|rx_caproot_mint|rx_caproot_revoke' ; then \
		echo "rx_skillroute.o references an authority admin operation; routing must not mint"; \
		rm -f $@; exit 1; fi

$(RX_CAPGRAPH_TEST): $(RX_CAPGRAPH_SRCS) $(RX_SKILLROUTE_OBJ) $(RX_CAPQ_OBJ) $(RX_GRAPH_OBJ) \
	src/runtime/rx_caproot.h src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_CAPGRAPH_SRCS) $(RX_SKILLROUTE_OBJ) $(RX_CAPQ_OBJ) \
		$(RX_GRAPH_OBJ) $(AIENOS_CAP_LIB) -lm

test-capability-graph: $(RX_CAPGRAPH_TEST)
	./$(RX_CAPGRAPH_TEST)

# COMPOSITION-2 WP-D: Skill Router end to end (requirement -> graph -> Skill
# -> bound action-graph node -> World run) and each failure mode failing
# closed. Repeats the no-mint symbol check on the router object it links.
RX_SRCOMPOSE_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/sha256.c src/omega_evidence.c \
	src/omega_core.c src/omega_canonical.c src/runtime/aien_machine_id.c \
	tests/runtime/rx_skillroute_compose_test.c
RX_SRCOMPOSE_TEST = $(OUT_DIR)/rx_skillroute_compose_test

$(RX_SRCOMPOSE_TEST): $(RX_SRCOMPOSE_SRCS) $(RX_SKILLROUTE_OBJ) $(RX_CAPQ_OBJ) $(RX_GRAPH_OBJ) \
	src/runtime/rx_caproot.h src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_SRCOMPOSE_SRCS) $(RX_SKILLROUTE_OBJ) $(RX_CAPQ_OBJ) \
		$(RX_GRAPH_OBJ) $(AIENOS_CAP_LIB) -lm

test-skillroute-compose: $(RX_SRCOMPOSE_TEST)
	@if nm -u $(RX_SKILLROUTE_OBJ) | grep -E 'aienos_cap_|rx_caproot_mint|rx_caproot_revoke' ; then \
		echo "rx_skillroute.o references an authority admin operation"; exit 1; fi
	@echo "SKILLROUTE_NO_MINT_PASS"
	./$(RX_SRCOMPOSE_TEST)

# OMEGA_PLAN_REUSE: plan IR and verified plan cache. rx_plan.o must not
# reference any AIENOS admin operation (the cache checks authority, never
# creates it); rx_plan_arrange.o (AIEN's planner) must only read the World.
RX_PLAN_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/sha256.c src/omega_evidence.c \
	src/omega_core.c src/omega_canonical.c tests/runtime/rx_plan_reuse.c
RX_PLAN_TEST = $(OUT_DIR)/rx_plan_reuse_test
RX_PLAN_OBJS = $(OUT_DIR)/rx_plan.o $(OUT_DIR)/rx_plan_arrange.o

$(OUT_DIR)/rx_plan.o: src/runtime/rx_plan.c src/runtime/rx_plan.h src/runtime/rx_graph.h \
	src/runtime/rx_world.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_plan.c
	@if nm -u $@ | grep -E 'aienos_cap_|rx_caproot_mint|rx_caproot_revoke' ; then \
		echo "rx_plan.o references an authority admin operation; the plan cache must not mint"; \
		rm -f $@; exit 1; fi

$(OUT_DIR)/rx_plan_arrange.o: src/runtime/rx_plan_arrange.c src/runtime/rx_plan_arrange.h \
	src/runtime/rx_plan.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_plan_arrange.c
	@if nm -u $@ | grep -E 'aienos_cap_|rx_caproot_|rx_world_publish|rx_graph_lower|rx_graph_start' ; then \
		echo "rx_plan_arrange.o must only read the World: the planner changes nothing"; \
		rm -f $@; exit 1; fi

$(RX_PLAN_TEST): $(RX_PLAN_SRCS) $(RX_PLAN_OBJS) $(RX_GRAPH_OBJ) src/runtime/rx_caproot.h \
	src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_PLAN_SRCS) $(RX_PLAN_OBJS) $(RX_GRAPH_OBJ) $(AIENOS_CAP_LIB) -lm

test-plan-reuse: $(RX_PLAN_TEST)
	./$(RX_PLAN_TEST)

# OMEGA_SEMANTIC_COMMUNICATION: branches and workers receive the semantic
# projection their InformationNeed selects, then only deltas, not full state.
# rx_semcomm.o is built alone first. It enforces capability boundaries, so it
# must not reference any authority admin operation, and it only reads the
# World, so it must not reference a World write either.
RX_SEMCOMM_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/sha256.c src/omega_evidence.c \
	src/omega_core.c src/omega_canonical.c tests/runtime/rx_semantic_comm.c
RX_SEMCOMM_TEST = $(OUT_DIR)/rx_semantic_comm_test
RX_SEMCOMM_OBJ = $(OUT_DIR)/rx_semcomm.o

$(RX_SEMCOMM_OBJ): src/runtime/rx_semcomm.c src/runtime/rx_semcomm.h src/runtime/rx_world.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_semcomm.c
	@if nm -u $@ | grep -E 'aienos_cap_|rx_caproot_mint|rx_caproot_revoke|rx_world_publish|rx_world_create|rx_world_retire|rx_world_add_reaction' ; then \
		echo "rx_semcomm.o references an authority admin operation or a World write; projection must only read"; \
		rm -f $@; exit 1; fi

$(RX_SEMCOMM_TEST): $(RX_SEMCOMM_SRCS) $(RX_SEMCOMM_OBJ) src/runtime/rx_caproot.h \
	src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_SEMCOMM_SRCS) $(RX_SEMCOMM_OBJ) $(AIENOS_CAP_LIB) -lm

test-semantic-comm: $(RX_SEMCOMM_TEST)
	./$(RX_SEMCOMM_TEST)

# OMEGA_COGNITIVE_ROUTING: each cognitive step goes to the cheapest
# realization the evidence says meets it, escalating only on insufficient
# verification or calibrated confidence. rx_route.o is built alone first and
# must not reference promotion or any AIENOS admin operation: routing profiles
# change only through a promoted generation.
RX_ROUTE_SRCS = src/runtime/rx_generation.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_cog_engines.c tests/runtime/rx_cognitive_routing.c
RX_ROUTE_TEST = $(OUT_DIR)/rx_cognitive_routing_test
RX_ROUTE_OBJ = $(OUT_DIR)/rx_route.o

$(RX_ROUTE_OBJ): src/runtime/rx_route.c src/runtime/rx_route.h src/runtime/rx_generation.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_route.c
	@if nm -u $@ | grep -E 'rx_gen_promote|aienos_cap_|rx_caproot_mint|rx_caproot_revoke' ; then \
		echo "rx_route.o references promotion or an authority admin operation"; \
		rm -f $@; exit 1; fi

$(RX_ROUTE_TEST): $(RX_ROUTE_SRCS) $(RX_ROUTE_OBJ) tests/runtime/rx_cog_engines.h \
	src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -Itests -o $@ $(RX_ROUTE_SRCS) $(RX_ROUTE_OBJ) $(AIENOS_CAP_LIB) -lm

test-cognitive-routing: $(RX_ROUTE_TEST)
	./$(RX_ROUTE_TEST)

# OMEGA_EMPIRICAL_OPTIMIZER: realization choice from a learned, calibrated cost
# model. rx_costmodel.o is built alone first and must not reference the
# generation store, any authority operation, or anything that maps or runs
# code: the model is a calculator; durability comes only through promotion.
.PHONY: test-costmodel test-empirical
RX_CM_OBJ = $(OUT_DIR)/rx_costmodel.o

$(RX_CM_OBJ): src/runtime/rx_costmodel.c src/runtime/rx_costmodel.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_costmodel.c
	@if nm -u $@ | grep -E 'rx_gen_|aienos_cap_|rx_caproot_|rx_capadmin_|rx_world_|mmap|mprotect|fork|exec|dlopen|system' ; then \
		echo "rx_costmodel.o references an operation a cost model must not have"; \
		rm -f $@; exit 1; fi

# Any host: synthetic measurements; fit, calibration, decisions, blob.
RX_CM_UNIT = $(OUT_DIR)/rx_costmodel_unit
$(RX_CM_UNIT): tests/runtime/rx_costmodel_unit.c $(RX_CM_OBJ) src/sha256.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ tests/runtime/rx_costmodel_unit.c $(RX_CM_OBJ) src/sha256.c -lm

test-costmodel: $(RX_CM_UNIT)
	./$(RX_CM_UNIT)

# AArch64 hosts: the gate. Real realizations, training, promotion through the
# R9 barrier on the native authority, held-out workloads.
RX_EMP_SRCS = src/runtime/rx_generation.c src/sha256.c src/omega_evidence.c \
	src/omega_canonical.c src/omega_validate.c src/omega_core.c src/omega_codec.c \
	src/aarch64_encoder.c src/aarch64_decoder.c src/omega_realize.c src/omega_realize_synth.c src/omega_program.c \
	src/omega_machine.c src/omega_exec.c src/omega_verify.c src/omega_matvec.c src/omega_matvec_quad.c \
	tests/runtime/rx_empirical_optimizer.c
RX_EMP_TEST = $(OUT_DIR)/rx_empirical_optimizer_test

$(RX_EMP_TEST): $(RX_EMP_SRCS) $(RX_CM_OBJ) src/runtime/rx_generation.h src/runtime/aienos_cap.h \
	$(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_EMP_SRCS) $(RX_CM_OBJ) $(AIENOS_CAP_LIB) -lm

test-empirical: $(RX_EMP_TEST)
	./$(RX_EMP_TEST)

# OMEGA_WORKFLOW_FUSION: repeated verified action-graph fragments become
# MetaSkills; only a verified, measured, canaried, promoted (R9 barrier) and
# published one replaces the steps. rx_fusion.o, like rx_graph.o, must not
# reference any AIENOS admin operation.
.PHONY: test-workflow-fusion
RX_FUSION_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/runtime/rx_generation.c src/sha256.c \
	src/omega_evidence.c src/omega_core.c src/omega_canonical.c tests/runtime/rx_workflow_fusion.c
RX_FUSION_TEST = $(OUT_DIR)/rx_workflow_fusion_test
RX_FUSION_OBJ = $(OUT_DIR)/rx_fusion.o

$(RX_FUSION_OBJ): src/runtime/rx_fusion.c src/runtime/rx_fusion.h src/runtime/rx_graph.h \
	src/runtime/rx_generation.h src/runtime/rx_world.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_fusion.c
	@if nm -u $@ | grep -E 'aienos_cap_|rx_caproot_mint|rx_caproot_revoke' ; then \
		echo "rx_fusion.o references an authority admin operation; fusion must not mint"; \
		rm -f $@; exit 1; fi

$(RX_FUSION_TEST): $(RX_FUSION_SRCS) $(RX_FUSION_OBJ) $(RX_GRAPH_OBJ) src/runtime/rx_caproot.h \
	src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_FUSION_SRCS) $(RX_FUSION_OBJ) $(RX_GRAPH_OBJ) \
		$(AIENOS_CAP_LIB) -lm

test-workflow-fusion: $(RX_FUSION_TEST)
	./$(RX_FUSION_TEST)

# OMEGA_TYPED_RESULT_CONSTRAINTS: structured results of cognition are checked
# against typed contracts; the publish gate is the only writer of what is
# published. rx_contract.o is built alone first and must not reference any
# AIENOS admin operation: a contract can find that authority is missing, never
# create it.
RX_TYPED_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/sha256.c src/omega_evidence.c \
	src/omega_core.c src/omega_canonical.c tests/runtime/rx_typed_results.c
RX_TYPED_TEST = $(OUT_DIR)/rx_typed_results_test
RX_CONTRACT_OBJ = $(OUT_DIR)/rx_contract.o
TYPED_RESULTS_N ?= 400

$(RX_CONTRACT_OBJ): src/runtime/rx_contract.c src/runtime/rx_contract.h src/runtime/rx_graph.h \
	src/runtime/rx_world.h src/runtime/rx_aien.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_contract.c
	@if nm -u $@ | grep -E 'aienos_cap_|rx_caproot_mint|rx_caproot_revoke' ; then \
		echo "rx_contract.o references an authority admin operation; a contract must not mint"; \
		rm -f $@; exit 1; fi

$(RX_TYPED_TEST): $(RX_TYPED_SRCS) $(RX_CONTRACT_OBJ) $(RX_GRAPH_OBJ) src/runtime/rx_caproot.h \
	src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_TYPED_SRCS) $(RX_CONTRACT_OBJ) $(RX_GRAPH_OBJ) \
		$(AIENOS_CAP_LIB) -lm

.PHONY: test-typed-results
test-typed-results: $(RX_TYPED_TEST)
	./$(RX_TYPED_TEST) $(TYPED_RESULTS_N)

# ---- ARGUS producer (feat/argus-producer, lanes H/H2 of ARGUS-0) ---------
# The runtime feeds ARGUS (src/runtime/rx_argus.{c,h}, ABI v1.1: per-thread
# rings, use tables flushed as CAPABILITY_USE_SUMMARY, full events only on
# transitions). Default builds are unchanged (RX_ARGUS undefined = 0: the
# hooks compile to nothing). This block builds the AEGIS/promotion suites with
# RX_ARGUS=$(RX_ARGUS) against ARGUS pinned by argus.lock, extracted with
# `git archive` from ARGUS_REPO (never built in place). ARGUS's sha256.c is
# Omega's src/sha256.c copied unchanged; Omega's is linked, not both.
#
# Grant source (ARGUS_AUTH):
#   observer (default) the authority announces its own mints/revokes
#            (aienos_cap_set_observer, aienos 12add16, native in the authority
#            pinned by aienos.lock, d39dd5b); the observer is installed by a link
#            wrap of aienos_cap_start, which also announces the office (cap 0).
#   aegis    GRANTED/REVOKED only where rx_aegis mints/revokes (harness mints
#            are unseen and show up as FORGED).
# Every ARGUS target links the aienos.lock authority, $(AIENOS_CAP_LIB), the
# same library as the default targets (with no observer set it
# costs one NULL check per admin operation), RX_ARGUS=0 included.
ARGUS_REPO ?= ../aienos-argus
ARGUS_COMMIT ?= $(shell head -n 1 argus.lock)
ARGUS_SHORT = $(shell echo $(ARGUS_COMMIT) | cut -c1-7)
ARGUS_SRC = $(OUT_DIR)/argus-src/$(ARGUS_SHORT)/native/argus
ARGUS_STAMP = $(OUT_DIR)/argus-src/$(ARGUS_SHORT)/.extracted
ARGUS_LIB_SRCS = $(addprefix $(ARGUS_SRC)/,argus_event.c argus_ring.c argus_core.c argus_detect.c)
RX_ARGUS ?= 2
ARGUS_AUTH ?= observer
ARGUS_CAP_LIB = $(AIENOS_CAP_LIB)
comma := ,
ARGUS_AUTH_FLAGS = $(if $(filter observer,$(ARGUS_AUTH)),-DRX_ARGUS_AUTHORITY_OBSERVER -Wl$(comma)--wrap=aienos_cap_start,)
ARGUS_VARIANT = $(if $(filter observer,$(ARGUS_AUTH)),,-$(ARGUS_AUTH))
ARGUS_OUT = $(OUT_DIR)/argus$(RX_ARGUS)$(ARGUS_VARIANT)
ARGUS_CFLAGS = $(CFLAGS) -DRX_ARGUS=$(RX_ARGUS) -I$(ARGUS_SRC)
ARGUS_STREAMS ?= $(HOME)/workspace/argus-runtime-streams
ARGUS_RUN_ID = $(shell git rev-parse --short HEAD 2>/dev/null)-argus$(ARGUS_SHORT)

$(ARGUS_STAMP):
	@test -n "$(ARGUS_COMMIT)" || { echo "argus.lock is empty"; exit 1; }
	mkdir -p $(OUT_DIR)/argus-src/$(ARGUS_SHORT)
	git -C $(ARGUS_REPO) archive $(ARGUS_COMMIT) native/argus | tar -x -C $(OUT_DIR)/argus-src/$(ARGUS_SHORT)
	touch $@

$(ARGUS_LIB_SRCS): $(ARGUS_STAMP)

# RX_ARGUS=0 links no ARGUS code at all.
ARGUS_RX = $(if $(filter 0,$(RX_ARGUS)),,src/runtime/rx_argus.c $(ARGUS_LIB_SRCS))
ARGUS_LINK_FLAGS = $(if $(filter 0,$(RX_ARGUS)),,$(ARGUS_AUTH_FLAGS))

# ---- Lane 32: the PRODUCTION R13 program, ARGUS linked ----------------------
# Built without AIEN_TEST_BUILD (RX_PROD_REFUSE_TEST) and with ARGUS pinned by
# argus.lock, the way the ARGUS suites build it, in the mode the ARGUS-1
# decisions intend: observe and record. RX_ARGUS=2 (consumer thread ingesting
# into argus_core), authority observer (the authority announces its own
# mints/revokes through the aienos_cap_start link wrap). ARGUS holds no
# capability and the pinned ARGUS has no response path, so it never blocks a
# request and never expands its own authority. These flags are fixed here, not
# taken from the overridable RX_ARGUS/ARGUS_AUTH. The program refuses to run
# unobserved (rx_r13_living.c argus_check_start).
RX_PROD_ARGUS_FLAGS = -DRX_ARGUS=2 -DRX_ARGUS_AUTHORITY_OBSERVER -I$(ARGUS_SRC) \
	-Wl$(comma)--wrap=aienos_cap_start
RX_PROD_ARGUS_SRCS = src/runtime/rx_argus.c $(ARGUS_LIB_SRCS)

$(RX_R13_HOST): $(RX_R13_SRCS) src/runtime/rx_living.h src/runtime/rx_argus.h $(RX_COMPOSE_LIVING_CHECKS) \
	$(ARGUS_STAMP) $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(RX_PROD_REFUSE_TEST)
	$(CC) $(CFLAGS) $(RX_PROD_ARGUS_FLAGS) -pthread -o $@ $(RX_R13_SRCS) $(RX_PROD_ARGUS_SRCS) \
		$(AIENOS_CAP_LIB) -lm

$(RX_R13_SILICON): $(RX_R13_SRCS) $(RX_R13_SILICON_EXTRA) src/runtime/rx_living.h src/runtime/rx_argus.h \
	$(RX_COMPOSE_LIVING_CHECKS) $(ARGUS_STAMP) $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(RX_PROD_REFUSE_TEST)
	$(CC) $(CFLAGS) $(RX_PROD_ARGUS_FLAGS) -DR13_SILICON -pthread -o $@ $(RX_R13_SRCS) \
		$(RX_PROD_ARGUS_SRCS) $(RX_R13_SILICON_EXTRA) $(AIENOS_CAP_LIB) -ldl -lm

# Production hygiene (link map + strings + ARGUS present; host run of the
# ARGUS probe where the binary can run) and the negative build test: compiling
# the production program with any test piece must FAIL (tools/r16_prod_hygiene.sh).
.PHONY: test-prod-hygiene test-prod-hygiene-silicon test-prod-refuses-test-pieces
test-prod-hygiene: $(RX_R13_HOST)
	sh tools/r16_prod_hygiene.sh host $(RX_R13_HOST)

test-prod-hygiene-silicon: $(RX_R13_SILICON)
	sh tools/r16_prod_hygiene.sh silicon $(RX_R13_SILICON)

test-prod-refuses-test-pieces: $(ARGUS_STAMP) $(AIENOS_CAP_LIB) $(RX_R13_HOST)
	CC="$(CC)" CFLAGS="$(CFLAGS)" ARGUS_SRC="$(ARGUS_SRC)" OUT="$(OUT_DIR)/prod-refuses" MAKE="$(MAKE)" PROD_BIN="$(RX_R13_HOST)" \
		sh tools/r16_prod_refuses.sh

# R16 G6 operator control by execution (docs/r16-operator-control.md): the
# production program driven from outside through its operator entry point.
# Host: the production host binary (R12 processor stand-in). Silicon: the
# production GB10 binary on the resident seat, needs the quiet lock, never
# killed (no SIGKILL phase). The mutants rebuild the production host program
# from mutated copies; the test must fail against each.
RX_OPERATOR_CLI = $(OUT_DIR)/rx_operator
$(RX_OPERATOR_CLI): tools/rx_operator.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ tools/rx_operator.c

.PHONY: test-r16-operator-host test-r16-operator-silicon test-r16-operator-mutants
test-r16-operator-host: $(RX_R13_HOST) $(RX_OPERATOR_CLI)
	bash tests/runtime/rx_operator_host.sh host $(RX_R13_HOST) $(RX_OPERATOR_CLI)

test-r16-operator-silicon: $(RX_R13_SILICON) $(RX_OPERATOR_CLI)
	bash tests/runtime/rx_operator_host.sh silicon $(RX_R13_SILICON) $(RX_OPERATOR_CLI)

test-r16-operator-mutants: $(RX_R13_HOST) $(RX_OPERATOR_CLI)
	CC="$(CC)" CFLAGS="$(CFLAGS)" RX_PROD_ARGUS_FLAGS="$(RX_PROD_ARGUS_FLAGS)" \
		RX_R13_SRCS="$(RX_R13_SRCS)" RX_PROD_ARGUS_SRCS="$(RX_PROD_ARGUS_SRCS)" \
		AIENOS_CAP_LIB="$(AIENOS_CAP_LIB)" bash tests/runtime/rx_operator_mutants.sh $(RX_OPERATOR_CLI)

ARGUS_R7 = $(ARGUS_OUT)/rx_r7_native_test
ARGUS_R8 = $(ARGUS_OUT)/rx_r8_aegis_test
ARGUS_R9 = $(ARGUS_OUT)/rx_r9_barrier_test
ARGUS_BENCH = $(ARGUS_OUT)/bench_rx_argus
ARGUS_BENCH_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/sha256.c tests/bench_rx_argus.c

$(ARGUS_OUT):
	mkdir -p $@

$(ARGUS_R7): $(RX_R7_SRCS) src/runtime/rx_argus.h $(ARGUS_RX) $(ARGUS_STAMP) $(ARGUS_CAP_LIB) | $(ARGUS_OUT)
	$(CC) $(ARGUS_CFLAGS) $(ARGUS_LINK_FLAGS) -pthread -o $@ $(RX_R7_SRCS) $(ARGUS_RX) $(ARGUS_CAP_LIB) -lm
$(ARGUS_R8): $(RX_R8_SRCS) src/runtime/rx_argus.h $(ARGUS_RX) $(ARGUS_STAMP) $(ARGUS_CAP_LIB) | $(ARGUS_OUT)
	$(CC) $(ARGUS_CFLAGS) $(ARGUS_LINK_FLAGS) -pthread -o $@ $(RX_R8_SRCS) $(ARGUS_RX) $(ARGUS_CAP_LIB) -lm
$(ARGUS_R9): $(RX_R9_SRCS) src/runtime/rx_argus.h $(ARGUS_RX) $(ARGUS_STAMP) $(ARGUS_CAP_LIB) | $(ARGUS_OUT)
	$(CC) $(ARGUS_CFLAGS) $(ARGUS_LINK_FLAGS) -pthread -o $@ $(RX_R9_SRCS) $(ARGUS_RX) $(ARGUS_CAP_LIB) -lm
# The bench mints directly and announces its one grant itself: no observer wrap.
$(ARGUS_BENCH): $(ARGUS_BENCH_SRCS) src/runtime/rx_argus.h $(ARGUS_RX) $(ARGUS_STAMP) $(ARGUS_CAP_LIB) | $(ARGUS_OUT)
	$(CC) $(ARGUS_CFLAGS) -pthread -o $@ $(ARGUS_BENCH_SRCS) $(ARGUS_RX) $(ARGUS_CAP_LIB) -lm

# ARGUS_RUNTIME_INTEGRATION: run each suite with the consumer ingesting into
# the real argus_core; keep the raw 128-byte stream (consumer ingest order)
# and a JSON summary: $(ARGUS_STREAMS)/<argus>-<suite>-v11.bin/.json
.PHONY: test-argus-runtime bench-rx-argus
test-argus-runtime: $(ARGUS_R7) $(ARGUS_R8) $(ARGUS_R9)
	mkdir -p $(ARGUS_STREAMS)
	@set -e; for s in r7:$(ARGUS_R7) r8:$(ARGUS_R8) r9:$(ARGUS_R9); do \
		n=$${s%%:*}; b=$${s#*:}; \
		echo "== ARGUS runtime integration $$n"; \
		RX_ARGUS_CONSUMER=ingest RX_ARGUS_RUN_ID=$(ARGUS_RUN_ID)-$$n RX_ARGUS_SUITE=$$n \
		RX_ARGUS_STREAM=$(ARGUS_STREAMS)/$(ARGUS_SHORT)-$$n$(ARGUS_VARIANT)-v11.bin \
		RX_ARGUS_SUMMARY=$(ARGUS_STREAMS)/$(ARGUS_SHORT)-$$n$(ARGUS_VARIANT)-v11.json ./$$b > $(ARGUS_OUT)/$$n.log 2>&1 \
		|| { tail -20 $(ARGUS_OUT)/$$n.log; exit 1; }; \
		tail -2 $(ARGUS_OUT)/$$n.log; \
	done

bench-rx-argus: $(ARGUS_BENCH)
	./$(ARGUS_BENCH)

ARGUS_REPLAY = $(OUT_DIR)/argus_replay-$(ARGUS_SHORT)
$(ARGUS_REPLAY): tools/argus_replay.c src/sha256.c $(ARGUS_STAMP) | $(OUT_DIR)
	$(CC) $(CFLAGS) -I$(ARGUS_SRC) -o $@ tools/argus_replay.c src/sha256.c $(ARGUS_LIB_SRCS)
.PHONY: argus-replay
argus-replay: $(ARGUS_REPLAY)

# ---------------------------------------------------------------------------
# OMEGA MIXED ALGEBRA (spec/mixed-algebra-reference.md): correctness-first CPU
# reference ("parity oracle") for balanced trits, Z3, packing and absmean
# quantization. Plain C11, no runtime or physics dependencies.
# test-algebra-asan reruns the same suite under address+undefined sanitizers.
.PHONY: test-algebra test-algebra-asan
OMA_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 -Isrc
OMA_SRCS = src/algebra/oma_trit.c src/algebra/oma_z3.c src/algebra/oma_pack.c src/algebra/oma_quant.c
OMA_HDRS = src/algebra/oma_trit.h src/algebra/oma_z3.h src/algebra/oma_pack.h src/algebra/oma_quant.h
OMA_TEST = $(OUT_DIR)/tests-algebra/test_oma
OMA_TEST_ASAN = $(OUT_DIR)/tests-algebra/test_oma_asan

$(OMA_TEST): tests/algebra/test_oma.c $(OMA_SRCS) $(OMA_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(OMA_CFLAGS) -o $@ tests/algebra/test_oma.c $(OMA_SRCS) -lm

$(OMA_TEST_ASAN): tests/algebra/test_oma.c $(OMA_SRCS) $(OMA_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(OMA_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-o $@ tests/algebra/test_oma.c $(OMA_SRCS) -lm

test-algebra: $(OMA_TEST)
	./$(OMA_TEST)

test-algebra-asan: $(OMA_TEST_ASAN)
	./$(OMA_TEST_ASAN)

# ---------------------------------------------------------------------------
# OMEGA MIXED ALGEBRA MA-2 (spec/mixed-algebra-ma2.md): one exact operation
# (ternary W x int8 x -> int32 y) with several verified realizations on the
# Grace CPU, a measured cost table and the MA-2 selector. The selector
# (oma_select) is RETIRED for new decisions (TURING K.6/K.7): it is kept only
# to reproduce ma2_select_receipt.json; new selections use src/turing
# turing_rank_min_cost and are recorded as turing.decision.v1.
# test-realize: bit-exact gate vs the naive oracle (plain and ASan+UBSan).
# bench-mixed-algebra: two benchmark runs on one pinned Cortex-X925 core,
# then the selector with its reproducibility check. Every run writes NEW
# files under evidence/MIXED_ALGEBRA/runs/$(MA2_RUN_ID)/ (default: UTC
# timestamp; an existing run directory is refused). Committed evidence
# (ma3_bench_run{1,2}.json, ma2_select_receipt.json) is never rewritten:
# check-mixed-algebra-evidence fails if a committed evidence/MIXED_ALGEBRA
# file is modified or deleted, and runs before and after the bench.
.PHONY: test-realize bench-mixed-algebra check-mixed-algebra-evidence
OMA_RZ_ARCH = -march=armv8.6-a+dotprod+i8mm+sve
OMA_RZ_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 $(OMA_RZ_ARCH) -Isrc
OMA_RZ_SRCS = src/algebra/realize_common.c src/algebra/realize_binary.c \
	src/algebra/realize_bitplane.c src/algebra/realize_sparse.c \
	src/algebra/realize_rns.c src/algebra/realize_dense.c
OMA_RZ_HDRS = src/algebra/realize_common.h
OMA_SEL_SRCS = src/algebra/oma_select.c src/sha256.c
OMA_SEL_HDRS = src/algebra/oma_select.h
OMA_RZ_TEST = $(OUT_DIR)/tests-algebra/test_realize
OMA_RZ_TEST_ASAN = $(OUT_DIR)/tests-algebra/test_realize_asan
OMA_RZ_BENCH = $(OUT_DIR)/tests-algebra/bench_mixed_algebra
OMA_RZ_SELECT = $(OUT_DIR)/tests-algebra/bench_select
MA2_EVIDENCE = evidence/MIXED_ALGEBRA
MA2_RUN_ID ?= $(shell date -u +%Y%m%dT%H%M%SZ)
MA2_RUN_ID := $(MA2_RUN_ID)
MA2_RUN_DIR = $(MA2_EVIDENCE)/runs/$(MA2_RUN_ID)

$(OMA_RZ_TEST): tests/algebra/test_realize.c $(OMA_RZ_SRCS) $(OMA_RZ_HDRS) src/algebra/oma_select.c $(OMA_SEL_HDRS) $(OMA_SRCS) $(OMA_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(OMA_RZ_CFLAGS) -o $@ tests/algebra/test_realize.c $(OMA_RZ_SRCS) src/algebra/oma_select.c $(OMA_SRCS) -lm

$(OMA_RZ_TEST_ASAN): tests/algebra/test_realize.c $(OMA_RZ_SRCS) $(OMA_RZ_HDRS) src/algebra/oma_select.c $(OMA_SEL_HDRS) $(OMA_SRCS) $(OMA_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(OMA_RZ_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-o $@ tests/algebra/test_realize.c $(OMA_RZ_SRCS) src/algebra/oma_select.c $(OMA_SRCS) -lm

$(OMA_RZ_BENCH): tests/algebra/bench_mixed_algebra.c $(OMA_RZ_SRCS) $(OMA_RZ_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(OMA_RZ_CFLAGS) -o $@ tests/algebra/bench_mixed_algebra.c $(OMA_RZ_SRCS) -lm

$(OMA_RZ_SELECT): tests/algebra/bench_select.c $(OMA_SEL_SRCS) $(OMA_SEL_HDRS) $(OMA_RZ_SRCS) $(OMA_RZ_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(OMA_RZ_CFLAGS) -o $@ tests/algebra/bench_select.c $(OMA_SEL_SRCS) $(OMA_RZ_SRCS) -lm

test-realize: $(OMA_RZ_TEST) $(OMA_RZ_TEST_ASAN)
	./$(OMA_RZ_TEST)
	./$(OMA_RZ_TEST_ASAN)

# Fails if any committed evidence/MIXED_ALGEBRA file is modified or deleted
# (staged or not). New, untracked files are allowed.
check-mixed-algebra-evidence:
	@changed="$$(git diff --name-status HEAD -- $(MA2_EVIDENCE))"; \
	if [ -n "$$changed" ]; then \
		echo "FAIL: committed mixed-algebra evidence would be modified:"; \
		echo "$$changed"; exit 1; \
	fi; \
	echo "check-mixed-algebra-evidence: PASS (committed evidence unchanged)"

bench-mixed-algebra: $(OMA_RZ_BENCH) $(OMA_RZ_SELECT)
	@$(MAKE) --no-print-directory check-mixed-algebra-evidence
	@if [ -e $(MA2_RUN_DIR) ]; then echo "FAIL: $(MA2_RUN_DIR) exists; choose a new MA2_RUN_ID"; exit 1; fi
	@mkdir -p $(MA2_RUN_DIR)
	OMA_BENCH_COMMIT=$$(git rev-parse HEAD) OMA_BENCH_DIRTY=$$(git status --porcelain -- src tests Makefile | grep -c .) \
		OMA_BENCH_BIN_SHA=$$(sha256sum $(OMA_RZ_BENCH) | cut -c1-64) \
		./$(OMA_RZ_BENCH) $(MA2_RUN_DIR)/ma2_bench_run1.json
	OMA_BENCH_COMMIT=$$(git rev-parse HEAD) OMA_BENCH_DIRTY=$$(git status --porcelain -- src tests Makefile | grep -c .) \
		OMA_BENCH_BIN_SHA=$$(sha256sum $(OMA_RZ_BENCH) | cut -c1-64) \
		./$(OMA_RZ_BENCH) $(MA2_RUN_DIR)/ma2_bench_run2.json
	./$(OMA_RZ_SELECT) $(MA2_RUN_DIR)/ma2_select_receipt.json \
		$(MA2_RUN_DIR)/ma2_bench_run1.json $(MA2_RUN_DIR)/ma2_bench_run2.json
	@$(MAKE) --no-print-directory check-mixed-algebra-evidence
	@echo "bench-mixed-algebra: new receipts in $(MA2_RUN_DIR)"

# ---------------------------------------------------------------------------
# OMEGA MIXED ALGEBRA, ADR 0019 MA-8 step 0 (spec/mixed-algebra-phase-twin.md):
# phase-domain Z3 digital twin. SIMULATED_DEVELOPMENT only: a software model
# of Z3 addition carried as tone phase, checked against oma_z3. No runtime,
# no selection, no digest. C11 + libm.
# test-phase-twin: full run -> $(OUT_DIR) receipt (never into evidence/);
#   quick run plain and under ASan+UBSan, receipts must be byte-identical;
#   if a committed receipt exists, the full run must reproduce it (all fields
#   except git_commit / tree_dirty / run_id / toolchain).
# phase-twin-receipt: from a clean committed tree only, copies the full
#   receipt to evidence/MIXED_ALGEBRA/phase_twin_receipt.<sha256>.json.
.PHONY: test-phase-twin phase-twin-receipt
PT_SRCS = src/algebra/phase_twin.c src/algebra/oma_z3.c src/algebra/oma_trit.c
PT_HDRS = src/algebra/phase_twin.h src/algebra/oma_z3.h src/algebra/oma_trit.h
PT_OUT = $(OUT_DIR)/tests-algebra
PT_TEST = $(PT_OUT)/test_phase_twin
PT_TEST_ASAN = $(PT_OUT)/test_phase_twin_asan
PT_ENV = PT_COMMIT=$$(git rev-parse HEAD) \
	PT_DIRTY=$$(git status --porcelain -- src tests Makefile spec evidence | grep -c .) \
	PT_TOOLCHAIN="$$($(CC) --version | head -1)"
PT_VOLATILE = '"(git_commit|tree_dirty|run_id|toolchain)"'

$(PT_TEST): tests/algebra/test_phase_twin.c $(PT_SRCS) $(PT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(OMA_CFLAGS) -o $@ tests/algebra/test_phase_twin.c $(PT_SRCS) -lm

$(PT_TEST_ASAN): tests/algebra/test_phase_twin.c $(PT_SRCS) $(PT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(OMA_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-o $@ tests/algebra/test_phase_twin.c $(PT_SRCS) -lm

test-phase-twin: $(PT_TEST) $(PT_TEST_ASAN)
	$(PT_ENV) ./$(PT_TEST) $(PT_OUT)/phase_twin_receipt.json
	./$(PT_TEST) --quick $(PT_OUT)/phase_twin_quick_plain.json
	./$(PT_TEST_ASAN) --quick $(PT_OUT)/phase_twin_quick_asan.json
	cmp $(PT_OUT)/phase_twin_quick_plain.json $(PT_OUT)/phase_twin_quick_asan.json
	@echo "test-phase-twin: quick receipts byte-identical across plain and ASan/UBSan builds"
	@ref=$$(ls evidence/MIXED_ALGEBRA/phase_twin_receipt.*.json 2>/dev/null | head -1); \
	if [ -n "$$ref" ]; then \
		grep -v -E $(PT_VOLATILE) "$$ref" > $(PT_OUT)/pt_ref.cmp; \
		grep -v -E $(PT_VOLATILE) $(PT_OUT)/phase_twin_receipt.json > $(PT_OUT)/pt_new.cmp; \
		cmp $(PT_OUT)/pt_ref.cmp $(PT_OUT)/pt_new.cmp && echo "test-phase-twin: reproduces $$ref"; \
	else echo "test-phase-twin: no committed receipt to compare"; fi

phase-twin-receipt: test-phase-twin
	@test "$$(git status --porcelain -- src tests Makefile spec | grep -c .)" = 0 || \
		{ echo "phase-twin-receipt: tree not clean, refusing"; exit 1; }
	@mkdir -p evidence/MIXED_ALGEBRA
	@h=$$(sha256sum $(PT_OUT)/phase_twin_receipt.json | cut -c1-64); \
	cp $(PT_OUT)/phase_twin_receipt.json evidence/MIXED_ALGEBRA/phase_twin_receipt.$$h.json; \
	echo "phase-twin-receipt: evidence/MIXED_ALGEBRA/phase_twin_receipt.$$h.json"

# ---------------------------------------------------------------------------
# TURING Wave 1 (docs/turing/TURING_W0_PROPOSAL.md): Field v1 records (K.7) +
# control-arm selector, post hoc over evidence/MIXED_ALGEBRA receipts.
# Reads src/algebra (registry) without modifying it; no runtime, no timed runs.
# test-turing: plain + ASan/UBSan suites, then a rebuild check (spec ids from
# two different builds must be byte-identical).
.PHONY: test-turing turing-field
TURING_SRCS = src/turing/field.c src/turing/field_select.c src/turing/field_select_v0_retired.c src/turing/history_selector.c \
	src/turing/replay.c src/omega_canonical.c src/sha256.c $(OMA_RZ_SRCS)
TURING_HDRS = src/turing/field.h src/turing/select.h src/omega_canonical.h src/omega_types.h src/sha256.h $(OMA_RZ_HDRS)
TURING_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
TURING_TEST = $(OUT_DIR)/tests-turing/test_turing
TURING_TEST_ASAN = $(OUT_DIR)/tests-turing/test_turing_asan
TURING_TOOL = $(OUT_DIR)/tests-turing/turing-field
TURING_TOOL_ASAN = $(OUT_DIR)/tests-turing/turing-field_asan

$(TURING_TEST): tests/turing/test_turing.c $(TURING_SRCS) $(TURING_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(OMA_RZ_CFLAGS) -o $@ tests/turing/test_turing.c $(TURING_SRCS) -lm

$(TURING_TEST_ASAN): tests/turing/test_turing.c $(TURING_SRCS) $(TURING_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(OMA_RZ_CFLAGS) $(TURING_ASAN) -o $@ tests/turing/test_turing.c $(TURING_SRCS) -lm

$(TURING_TOOL): tools/turing_field.c $(TURING_SRCS) $(TURING_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(OMA_RZ_CFLAGS) -o $@ tools/turing_field.c $(TURING_SRCS) -lm

$(TURING_TOOL_ASAN): tools/turing_field.c $(TURING_SRCS) $(TURING_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(OMA_RZ_CFLAGS) $(TURING_ASAN) -o $@ tools/turing_field.c $(TURING_SRCS) -lm

turing-field: $(TURING_TOOL)
	./$(TURING_TOOL)

test-turing: $(TURING_TEST) $(TURING_TEST_ASAN) $(TURING_TOOL) $(TURING_TOOL_ASAN)
	./$(TURING_TEST)
	./$(TURING_TEST_ASAN)
	./$(TURING_TOOL) --spec-ids > $(OUT_DIR)/tests-turing/spec_ids_plain.txt
	./$(TURING_TOOL_ASAN) --spec-ids > $(OUT_DIR)/tests-turing/spec_ids_asan.txt
	cmp $(OUT_DIR)/tests-turing/spec_ids_plain.txt $(OUT_DIR)/tests-turing/spec_ids_asan.txt
	./$(TURING_TOOL_ASAN) > $(OUT_DIR)/tests-turing/turing_field_asan.txt
	@echo "test-turing: spec ids identical across plain and ASan builds; turing-field runs clean under ASan/UBSan"

# ---------------------------------------------------------------------------
# MIXED_ALGEBRA_DIGITAL_V1 closure gate (spec/mixed-algebra-digital-v1.md,
# pre-registered). test-ma-digital-bottom: realization-layer ⊥ coverage
# (plain + ASan/UBSan). gate-mixed-algebra-digital-v1: correctness leg, one
# bench-mixed-algebra run under ~/workspace/.spark-quiet (or reuse with
# MA_DV1_REUSE_RUN=<run id>), TURING Field v1 selection for the
# pre-registered queries, separate-process digest reproduction, and a
# digest-named wrapper receipt in evidence/MIXED_ALGEBRA/digital_v1/.
.PHONY: test-ma-digital-bottom gate-mixed-algebra-digital-v1
DV1_BOTTOM = $(OUT_DIR)/tests-algebra/test_ma_digital_bottom
DV1_BOTTOM_ASAN = $(OUT_DIR)/tests-algebra/test_ma_digital_bottom_asan
DV1_GATE_BIN = $(OUT_DIR)/tests-algebra/ma_digital_v1_gate
DV1_GATE_BIN_ASAN = $(OUT_DIR)/tests-algebra/ma_digital_v1_gate_asan

$(DV1_BOTTOM): tests/algebra/test_ma_digital_bottom.c $(OMA_RZ_SRCS) $(OMA_RZ_HDRS) $(OMA_SRCS) $(OMA_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(OMA_RZ_CFLAGS) -o $@ tests/algebra/test_ma_digital_bottom.c $(OMA_RZ_SRCS) $(OMA_SRCS) -lm

$(DV1_BOTTOM_ASAN): tests/algebra/test_ma_digital_bottom.c $(OMA_RZ_SRCS) $(OMA_RZ_HDRS) $(OMA_SRCS) $(OMA_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(OMA_RZ_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
		-o $@ tests/algebra/test_ma_digital_bottom.c $(OMA_RZ_SRCS) $(OMA_SRCS) -lm

$(DV1_GATE_BIN): tests/algebra/ma_digital_v1_gate.c $(TURING_SRCS) $(TURING_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(OMA_RZ_CFLAGS) -o $@ tests/algebra/ma_digital_v1_gate.c $(TURING_SRCS) -lm

$(DV1_GATE_BIN_ASAN): tests/algebra/ma_digital_v1_gate.c $(TURING_SRCS) $(TURING_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(OMA_RZ_CFLAGS) $(TURING_ASAN) -o $@ tests/algebra/ma_digital_v1_gate.c $(TURING_SRCS) -lm

test-ma-digital-bottom: $(DV1_BOTTOM) $(DV1_BOTTOM_ASAN)
	./$(DV1_BOTTOM)
	./$(DV1_BOTTOM_ASAN)

gate-mixed-algebra-digital-v1: $(DV1_GATE_BIN) $(DV1_GATE_BIN_ASAN) $(DV1_BOTTOM) $(DV1_BOTTOM_ASAN)
	DV1_GATE=./$(DV1_GATE_BIN) DV1_GATE_ASAN=./$(DV1_GATE_BIN_ASAN) MAKE="$(MAKE)" \
		sh tests/algebra/ma_digital_v1_gate.sh

# Content check (post-run addenda 2026-09-29): every digital_v1 wrapper
# re-hashes to its file name and every receipt it cites re-hashes to the
# digest recorded in the wrapper. Catches edits that were committed.
.PHONY: check-mixed-algebra-digital-v1-evidence
check-mixed-algebra-digital-v1-evidence:
	sh tests/algebra/ma_digital_v1_check_evidence.sh

# PATH-1 (spec/path-semantic-object.md section 15). BOOTSTRAP / REFERENCE oracle
# and TEST HARNESS only; no runtime, physics or GPU links.
PATH1_TEST_BIN := $(OUT_DIR)/tests-path/test_path_identity
.PHONY: test-path
$(PATH1_TEST_BIN): src/path/rx_path.c src/path/rx_path.h src/sha256.c src/sha256.h \
		src/omega_types.h tests/path/test_path_identity.c
	mkdir -p $(dir $@)
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -Isrc -Isrc/path -o $@ \
		src/path/rx_path.c src/sha256.c tests/path/test_path_identity.c

test-path: $(PATH1_TEST_BIN)
	sh tests/path/check_path_boundary.sh
	./$(PATH1_TEST_BIN)
	CC="$(CC)" sh tests/path/mutate.sh

# COMPOSITION-2: one World, one causal path (rx_compose). test-composition
# runs every fault point in process and as a crashed child, the OLD-or-NEW
# recovery, authority refusals and the Cortex record (WP-B, WP-C).
# test-composition-gate runs the 14-step gate in fresh directories; the
# receipt is written by tools/composition_gate.sh from a clean tree (WP-E).
RX_COMPOSE_SRCS = src/runtime/rx_compose.c src/runtime/rx_jspace.c src/runtime/rx_caproot.c \
	src/runtime/rx_world.c src/runtime/rx_coherent.c src/runtime/rx_native_bind.c \
	src/runtime/rx_aegis.c src/runtime/rx_cortex_record.c src/runtime/aien_machine_id.c \
	src/sha256.c src/omega_evidence.c src/omega_core.c src/omega_canonical.c
RX_COMPOSE_DEPS = $(RX_COMPOSE_SRCS) $(OUT_DIR)/rx_cortex.o $(RX_SKILLROUTE_OBJ) $(RX_CAPQ_OBJ) \
	$(RX_GRAPH_OBJ) src/runtime/rx_compose.h tests/runtime/rx_compose_fixture.h \
	src/runtime/rx_world.h src/runtime/rx_jspace.h src/runtime/aienos_cap.h $(AIENOS_CAP_LIB)
RX_COMPOSE_LINK = $(OUT_DIR)/rx_cortex.o $(RX_SKILLROUTE_OBJ) $(RX_CAPQ_OBJ) $(RX_GRAPH_OBJ) \
	$(AIENOS_CAP_LIB) -lm
# Test build ($(AIEN_TEST_FLAGS), Lane 32): the composition fault points and
# rogue-candidate hook (rx_compose.h, RXC_TEST_HOOKS) exist only in the unit
# tests and the R13 test-build variant; the gate, the production program and
# every other build compile them out.
RX_COMPOSE_TEST = $(OUT_DIR)/rx_compose_test
RX_COMPOSE_GATE = $(OUT_DIR)/rx_composition_gate
# Attach hygiene (one per World, close waits for its own steps, inert
# reactions + table bound); its ASan build is test-composition-attach-asan.
RX_COMPOSE_ATTACH_TEST = $(OUT_DIR)/rx_compose_attach_test
RX_COMPOSE_ATTACH_ASAN = $(OUT_DIR)/rx_compose_attach_test_asan

$(RX_COMPOSE_TEST): $(RX_COMPOSE_DEPS) tests/runtime/rx_compose_test.c | $(OUT_DIR)
	$(CC) $(CFLAGS) $(AIEN_TEST_FLAGS) -pthread -o $@ $(RX_COMPOSE_SRCS) tests/runtime/rx_compose_test.c \
		$(RX_COMPOSE_LINK)

$(RX_COMPOSE_GATE): $(RX_COMPOSE_DEPS) tests/runtime/rx_composition_gate.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_COMPOSE_SRCS) tests/runtime/rx_composition_gate.c \
		$(RX_COMPOSE_LINK)

$(RX_COMPOSE_ATTACH_TEST): $(RX_COMPOSE_DEPS) tests/runtime/rx_compose_attach_test.c | $(OUT_DIR)
	$(CC) $(CFLAGS) $(AIEN_TEST_FLAGS) -pthread -o $@ $(RX_COMPOSE_SRCS) \
		tests/runtime/rx_compose_attach_test.c $(RX_COMPOSE_LINK)

$(RX_COMPOSE_ATTACH_ASAN): $(RX_COMPOSE_DEPS) tests/runtime/rx_compose_attach_test.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $(AIEN_TEST_FLAGS) \
		-pthread -o $@ $(RX_COMPOSE_SRCS) tests/runtime/rx_compose_attach_test.c $(RX_COMPOSE_LINK)

test-composition: $(RX_COMPOSE_TEST) $(RX_COMPOSE_ATTACH_TEST)
	./$(RX_COMPOSE_TEST)
	./$(RX_COMPOSE_ATTACH_TEST)

test-composition-attach-asan: $(RX_COMPOSE_ATTACH_ASAN)
	./$(RX_COMPOSE_ATTACH_ASAN)

test-composition-gate: $(RX_COMPOSE_GATE)
	./$(RX_COMPOSE_GATE) "$$(git rev-parse HEAD)" $(OUT_DIR)/composition_gate_receipt.json

composition-gate-bin: $(RX_COMPOSE_GATE)

print-composition-gate-bin:
	@echo $(RX_COMPOSE_GATE)

.PHONY: test-composition test-composition-gate test-composition-attach-asan composition-gate-bin print-composition-gate-bin

# C4 (Convergence plan item 4): requalification of the local runtime with
# interruption and recovery (crash at every stage boundary, SIGKILL sweep, on-disk
# corruption). Test build only (crash points). Receipt: tools/c4_requal.sh.
RX_C4_REQUAL = $(OUT_DIR)/rx_c4_requal

$(RX_C4_REQUAL): $(RX_COMPOSE_DEPS) tests/runtime/rx_c4_requal.c | $(OUT_DIR)
	$(CC) $(CFLAGS) $(AIEN_TEST_FLAGS) -pthread -o $@ $(RX_COMPOSE_SRCS) tests/runtime/rx_c4_requal.c \
		$(RX_COMPOSE_LINK)

test-c4-requal: $(RX_C4_REQUAL)
	./$(RX_C4_REQUAL) $(OUT_DIR)/c4_requal_receipt.json

c4-requal-bin: $(RX_C4_REQUAL)

print-c4-requal-bin:
	@echo $(RX_C4_REQUAL)

.PHONY: test-c4-requal c4-requal-bin print-c4-requal-bin

# COMPOSITION-2 GPU tier: the same 14-step gate with both Skills executed on
# the GB10 through the sovereign M16 native path (no CUDA); see
# tests/runtime/rx_compose_gpu_skill.h. A chip run: take the quiet flag and
# use tools/composition_gate.sh --gpu (clean tree, content-addressed receipt).
RX_COMPOSE_GATE_GPU = $(OUT_DIR)/rx_composition_gate_gpu
RX_COMPOSE_GPU_SRCS = src/omega_blackwell_submit.c src/omega_blackwell_engine.c src/omega_gpu_engine.c src/omega_blackwell_matmul.c \
	src/omega_blackwell_codegen.c src/omega_blackwell_encoder.c src/omega_blackwell_qmd.c \
	src/omega_blackwell_realize.c src/omega_vector.c src/omega_validate.c src/omega_gpu_wait.c \
	$(PHYSICS_DIR)/m16/m16_native.c $(PHYSICS_DIR)/nvrm/nvrm.c

$(RX_COMPOSE_GATE_GPU): $(RX_COMPOSE_DEPS) $(RX_COMPOSE_GPU_SRCS) tests/runtime/rx_composition_gate.c \
	tests/runtime/rx_compose_gpu_skill.h | check-physics-lock $(OUT_DIR)
	$(CC) $(CFLAGS) -DRXC_GATE_GPU -Itests/runtime -pthread -o $@ $(RX_COMPOSE_SRCS) \
		$(RX_COMPOSE_GPU_SRCS) tests/runtime/rx_composition_gate.c $(RX_COMPOSE_LINK) -ldl

test-composition-gate-gpu: $(RX_COMPOSE_GATE_GPU)
	./$(RX_COMPOSE_GATE_GPU) "$$(git rev-parse HEAD)" $(OUT_DIR)/composition_gate_gpu_receipt.json

composition-gate-gpu-bin: $(RX_COMPOSE_GATE_GPU)

print-composition-gate-gpu-bin:
	@echo $(RX_COMPOSE_GATE_GPU)

.PHONY: test-composition-gate-gpu composition-gate-gpu-bin print-composition-gate-gpu-bin

# E1 row 7: correctly rounded FP32 DIV and SQRT on the GB10
# (docs/numeric/E1_DIVSQRT_GB10.md). Host tier, offline nvdisasm provenance
# and the CHECK mutation sweep need no device; the chip run is
# tools/run_divsqrt_gate.sh only (quiet flag, detached, receipt).
DIVSQRT_SRCS = tests/test_omega_divsqrt_gb10.c src/omega_numeric_divsqrt_gb10.c src/omega_numeric.c \
               src/omega_numeric_provenance.c $(NUMERIC_BW_SRCS)
DIVSQRT_HDRS = src/omega_numeric_divsqrt_gb10.h src/omega_numeric.h src/omega_blackwell_encoder.h \
               src/omega_blackwell_qmd.h src/sha256.h
.PHONY: test-divsqrt-host test-divsqrt-nvdisasm test-divsqrt-sweep
build/test_omega_divsqrt_gb10_cpu: $(DIVSQRT_SRCS) $(DIVSQRT_HDRS)
	@mkdir -p build
	gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off -Isrc -DOMEGA_NUMERIC_CPU_ONLY -pthread -o $@ $(DIVSQRT_SRCS)
test-divsqrt-host: build/test_omega_divsqrt_gb10_cpu
	./build/test_omega_divsqrt_gb10_cpu
test-divsqrt-nvdisasm:
	tools/divsqrt_nvdisasm_check.sh
test-divsqrt-sweep:
	tools/divsqrt_check_sweep.sh

# E1 row 10: GB10 realizations of the frozen transcendental sequences (EXP2,
# LOG2), bit-identical to src/omega_numeric_transc.c. Kernels live in
# src/omega_numeric_divsqrt_gb10.c (same frame as DIV/SQRT; nvdisasm
# provenance via test-divsqrt-nvdisasm). Host tier needs no device; the chip
# run (all 2^32 inputs per op) is tools/run_numeric_transc_gate.sh only.
TRANSC_GB10_SRCS = tests/test_omega_numeric_transc_gb10.c src/omega_numeric_divsqrt_gb10.c \
                   src/omega_numeric_transc.c src/omega_numeric.c src/omega_numeric_provenance.c \
                   $(NUMERIC_BW_SRCS)
.PHONY: test-numeric-transc-gb10-host
build/test_omega_numeric_transc_gb10_cpu: $(TRANSC_GB10_SRCS) $(DIVSQRT_HDRS) src/omega_numeric_transc.h
	@mkdir -p build
	gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off -fno-fast-math -Isrc -DOMEGA_NUMERIC_CPU_ONLY -pthread -o $@ $(TRANSC_GB10_SRCS)
test-numeric-transc-gb10-host: build/test_omega_numeric_transc_gb10_cpu
	./build/test_omega_numeric_transc_gb10_cpu

# C3: host-only detector for the intermittent unwritten-output event, with its
# own unit test on synthetic buffers. The GB10 harness is
# tests/test_omega_unwritten_trap_gb10.c (chip: forge queue only, see
# tools/run_unwritten_trap.sh).
.PHONY: test-unwritten-trap-host
build/test_omega_unwritten_trap: tests/test_omega_unwritten_trap.c src/omega_unwritten_trap.c src/omega_unwritten_trap.h
	@mkdir -p build
	gcc -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc -o $@ tests/test_omega_unwritten_trap.c src/omega_unwritten_trap.c
test-unwritten-trap-host: build/test_omega_unwritten_trap
	./build/test_omega_unwritten_trap

# E1 row 2: general global load/store on the GB10 (src/omega_numeric_ldst_gb10.h,
# docs/numeric/E1_LDST_GB10.md). test-ldst-host needs no device; test-ldst-nvdisasm
# decodes every table kernel offline; the chip run is tools/run_numeric_ldst_chip.sh.
LDST_SRCS = tests/test_omega_ldst_gb10.c src/omega_numeric_ldst_gb10.c src/omega_numeric_divsqrt_gb10.c src/omega_numeric.c \
            src/omega_numeric_provenance.c src/omega_blackwell_encoder.c src/omega_blackwell_codegen.c \
            src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c src/sha256.c
LDST_HDRS = src/omega_numeric_ldst_gb10.h src/omega_numeric_divsqrt_gb10.h src/omega_numeric.h src/omega_blackwell_encoder.h \
            src/omega_blackwell_qmd.h src/sha256.h
.PHONY: test-ldst-host test-ldst-nvdisasm
build/test_omega_ldst_gb10_cpu: $(LDST_SRCS) $(LDST_HDRS)
	@mkdir -p build
	gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off -Isrc -DOMEGA_NUMERIC_CPU_ONLY -pthread -o $@ $(LDST_SRCS)
test-ldst-host: build/test_omega_ldst_gb10_cpu
	./build/test_omega_ldst_gb10_cpu
test-ldst-nvdisasm:
	tools/ldst_nvdisasm_check.sh

# VC1-LIB: unit test for the library admission gate (receipt required, bootstrap kind,
# dependencies must exist, no truncation) plus mutation proof. Each mutant is a copy of
# src/omega_library.c with one tagged guard line (VC1:<tag>) deleted or weakened; the test
# must exit 1 (KILLED) for every one. A mutant whose sed did not change the file fails the build.
# Physics-free; CPU only.
.PHONY: test-library
LIBTEST_CORE = src/sha256.c src/omega_canonical.c src/omega_validate.c src/omega_core.c src/omega_codec.c \
	src/aarch64_encoder.c src/aarch64_decoder.c src/omega_realize.c src/omega_realize_synth.c \
	src/omega_machine.c src/omega_exec.c src/omega_verify.c src/omega_program.c src/omega_synthesis.c
LIBTEST_FLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc
LIBTEST_DIR = $(OUT_DIR)/library-test
test-library: tests/test_omega_library.c src/omega_library.c src/omega_library.h
	@mkdir -p $(LIBTEST_DIR)
	$(CC) $(LIBTEST_FLAGS) -o $(LIBTEST_DIR)/test_omega_library tests/test_omega_library.c src/omega_library.c $(LIBTEST_CORE)
	$(LIBTEST_DIR)/test_omega_library
	@set -eu; build_mutant() { name=$$1; tag=$$2; repl=$$3; \
	  sed "/VC1:$$tag/c\\$$repl" src/omega_library.c > $(LIBTEST_DIR)/mut_$$name.c; \
	  if cmp -s src/omega_library.c $(LIBTEST_DIR)/mut_$$name.c; then echo "mutant $$name: sed changed nothing"; exit 1; fi; \
	  $(CC) $(LIBTEST_FLAGS) -o $(LIBTEST_DIR)/mut_$$name tests/test_omega_library.c $(LIBTEST_DIR)/mut_$$name.c $(LIBTEST_CORE); \
	  rc=0; $(LIBTEST_DIR)/mut_$$name $$name > $(LIBTEST_DIR)/mut_$$name.out 2>&1 || rc=$$?; \
	  if [ $$rc -ne 1 ] || ! grep -q "^MUTANT $$name KILLED" $(LIBTEST_DIR)/mut_$$name.out; then \
	    echo "mutant $$name NOT killed (exit $$rc)"; cat $(LIBTEST_DIR)/mut_$$name.out; exit 1; fi; \
	  echo "mutant $$name: killed (exit 1)"; }; \
	  build_mutant allow-null-receipt    null-receipt         '    if (!receipt_hash) receipt_hash = (const uint8_t *)"0123456789abcdef0123456789abcdef";'; \
	  build_mutant allow-zero-receipt    zero-receipt         '    ;'; \
	  build_mutant bootstrap-null-audit  bootstrap-null-audit '    if (!audit_hash) audit_hash = (const uint8_t *)"0123456789abcdef0123456789abcdef";'; \
	  build_mutant bootstrap-zero-audit  bootstrap-zero-audit '    ;'; \
	  build_mutant allow-unverified      verified-gate        '    ;'; \
	  build_mutant allow-duplicate       duplicate            '    ;'; \
	  build_mutant allow-unknown-dep     dep-unknown          '        ;'; \
	  build_mutant truncate-deps         dep-overflow         '    if (dep_count > OMEGA_LIB_MAX_DEPS) dep_count = OMEGA_LIB_MAX_DEPS;'; \
	  build_mutant max-deps-off-by-one   dep-overflow         '    if (dep_count >= OMEGA_LIB_MAX_DEPS) return -1;'; \
	  build_mutant digest-ignores-kind   digest-kind          '            uint8_t kind = 0;'; \
	  build_mutant bootstrap-as-verified kind-bootstrap       '    return lib_admit(lib, prog, deps, dep_count, audit_hash, OMEGA_LIB_ADMISSION_VERIFIED);'; \
	  build_mutant refuse-valid          capacity             '    if (prog->name[3] == (char)55) return -1;'; \
	  build_mutant refuse-deps           dep-null             '    if (dep_count > 0) return -1;'
	@echo "test-library: PASS (all checks, all mutants killed)"

# VC1-STORE (VC1 stage 3): unit test for the Verified Crumb Store (omega_vcstore): golden
# vectors from aien-protocols (tests/vcstore/golden, PIN names the commit), immutability,
# dependency closure, insert-order-independent digest, name index, save/load. Plus a mutation
# proof: each mutant is a copy of src/omega_vcstore.c with one tagged line (VC1S:<tag>) deleted or
# weakened; the test must exit 1 (KILLED) for every one. A mutant whose sed changed nothing
# fails the build. Physics-free; CPU only; needs only src/sha256.c.
.PHONY: test-vcstore
VCSTEST_FLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc
VCSTEST_DIR = $(OUT_DIR)/vcstore-test
test-vcstore: tests/test_omega_vcstore.c src/omega_vcstore.c src/omega_vcstore.h src/sha256.c
	@mkdir -p $(VCSTEST_DIR)
	$(CC) $(VCSTEST_FLAGS) -o $(VCSTEST_DIR)/test_omega_vcstore tests/test_omega_vcstore.c src/omega_vcstore.c src/sha256.c
	$(VCSTEST_DIR)/test_omega_vcstore
	@set -eu; build_mutant() { name=$$1; tag=$$2; repl=$$3; \
	  sed "/VC1S:$$tag/c\\$$repl" src/omega_vcstore.c > $(VCSTEST_DIR)/mut_$$name.c; \
	  if cmp -s src/omega_vcstore.c $(VCSTEST_DIR)/mut_$$name.c; then echo "mutant $$name: sed changed nothing"; exit 1; fi; \
	  $(CC) $(VCSTEST_FLAGS) -o $(VCSTEST_DIR)/mut_$$name tests/test_omega_vcstore.c $(VCSTEST_DIR)/mut_$$name.c src/sha256.c; \
	  rc=0; $(VCSTEST_DIR)/mut_$$name $$name > $(VCSTEST_DIR)/mut_$$name.out 2>&1 || rc=$$?; \
	  if [ $$rc -ne 1 ] || ! grep -q "^MUTANT $$name KILLED by " $(VCSTEST_DIR)/mut_$$name.out; then \
	    echo "mutant $$name NOT killed (exit $$rc)"; tail -5 $(VCSTEST_DIR)/mut_$$name.out; exit 1; fi; \
	  echo "mutant $$name: killed (exit 1)"; }; \
	  build_mutant refuse-valid             dec-trailing           '    if (!c.err && c.pos == c.n) cfail(&c, OMEGA_VCS_TRAILING_BYTES);'; \
	  build_mutant refuse-existing-dep      dep-exists             '        if (vcs_find(s, dep, &di)) { rc = OMEGA_VCS_UNVERIFIED_DEPENDENCY; break; } else continue;'; \
	  build_mutant caps-reversed            dec-caps-order         '        if (!c.err && i > 0 && str_cmp(&v->capabilities[i - 1], &v->capabilities[i]) <= 0) cfail(&c, OMEGA_VCS_NONCANONICAL_SET);'; \
	  build_mutant kind-source-only         dec-kind               '        if (v->digest_kind != OMEGA_VC_DIGEST_SOURCE) cfail(&c, OMEGA_VCS_BAD_DIGEST_KIND);'; \
	  build_mutant id-hash                  id-hash                '    sha256_hash(bytes + (len >= 22 ? 22 : 0), len >= 22 ? len - 22 : len, out);'; \
	  build_mutant dec-dep-dup              dec-dep-dup            '        ;'; \
	  build_mutant dec-dep-order            dec-dep-order          '        ;'; \
	  build_mutant dec-receipt              dec-receipt            '    ;'; \
	  build_mutant dec-dep-self             dec-dep-self           '        ;'; \
	  build_mutant dec-trailing             dec-trailing           '    ;'; \
	  build_mutant dec-version              dec-version            '    ;'; \
	  build_mutant dec-truncated            dec-truncated          '    if (c->n - c->pos < k) return 0;'; \
	  build_mutant dec-tag                  dec-tag                '    ;'; \
	  build_mutant dec-exports-order        dec-exports-order      '        ;'; \
	  build_mutant dec-kind                 dec-kind               '        ;'; \
	  build_mutant dec-zero-ids             dec-zero-ids           '    ;'; \
	  build_mutant dec-string-char          dec-string-char        '        (void)ch;'; \
	  build_mutant dec-count                dec-count              '    ;'; \
	  build_mutant dec-real-order           dec-real-order         '        (void)prev;'; \
	  build_mutant dec-caps-order           dec-caps-order         '        ;'; \
	  build_mutant dec-dep-zero             dec-dep-zero           '        (void)req;'; \
	  build_mutant dec-evroot               dec-evroot             '    ;'; \
	  build_mutant dec-string-empty         dec-string-len         '    if (len > OMEGA_VC_MAX_STRING) { cfail(c, OMEGA_VCS_BAD_STRING); return; }'; \
	  build_mutant dec-string-max           dec-string-len         '    if (len == 0) { cfail(c, OMEGA_VCS_BAD_STRING); return; }'; \
	  build_mutant claimed-required         claimed-required       '    if (!claimed) { static uint8_t zz[32]; omega_vc_compute_id(canon, len, zz); claimed = zz; }'; \
	  build_mutant insert-idcheck           insert-idcheck         '    if (0) { free(rec); return OMEGA_VCS_VCSTORE_ID_MISMATCH; }'; \
	  build_mutant idempotent               idempotent             '        (void)o;'; \
	  build_mutant idempotent-ignores-kind  idempotent             '        if (o->len == len && memcmp(o->bytes, canon, len) == 0) return OMEGA_VCS_OK;'; \
	  build_mutant immutable                immutable              '        return OMEGA_VCS_OK;'; \
	  build_mutant dep-exists               dep-exists             '        if (!vcs_find(s, dep, &di)) continue;'; \
	  build_mutant dep-contract             dep-contract           '        ;'; \
	  build_mutant no-cap                   no-cap                 '    if (s->count >= 128) return OMEGA_VCS_VCSTORE_CAPACITY;'; \
	  build_mutant sorted-insert            sorted-insert          '    size_t at = s->count;'; \
	  build_mutant kind-bootstrap           kind-bootstrap         '    return vcs_insert(s, canonical, len, claimed_vc_id, OMEGA_VCS_ADMISSION_VERIFIED);'; \
	  build_mutant get-recompute            get-recompute          '    ;'; \
	  build_mutant get-key                  get-key                '    ;'; \
	  build_mutant receipt-of               receipt-of             '    if (rc == OMEGA_VCS_OK) memcpy(out_receipt, rec->vc.evidence_root, 32);'; \
	  build_mutant walk-order               walk-order             '            const uint8_t *dep = f->deps + 64 * (size_t)(f->n_dep - 1 - f->next++);'; \
	  build_mutant walk-visited             walk-visited           '            ;'; \
	  build_mutant walk-capacity            walk-capacity          '    ;'; \
	  build_mutant walk-dep-exists          walk-dep-exists        '            if (!vcs_find(s, dep, &di)) continue;'; \
	  build_mutant walk-contract            walk-contract          '            ;'; \
	  build_mutant walk-cycle               walk-cycle             '            if (state[di] == 1) continue;'; \
	  build_mutant walk-names               walk-names             '    if (s->n_names) return OMEGA_VCS_UNVERIFIED_DEPENDENCY;'; \
	  build_mutant digest-magic             digest-magic           '    sha256_update(&ctx, (const uint8_t *)"VCS2", 4);'; \
	  build_mutant digest-kind              digest-kind            '        uint8_t kind = 0;'; \
	  build_mutant digest-bytes             digest-bytes           '        sha256_update(&ctx, o->bytes, 0);'; \
	  build_mutant digest-names             digest-names           '    u64be(&ctx, (uint64_t)s->n_names);'; \
	  build_mutant namedigest-id            namedigest-id          '        sha256_update(&ctx, nm->id, 0);'; \
	  build_mutant name-sorted              name-sorted            '    return name_insert_at(s, s->n_names, name, id);'; \
	  build_mutant name-many                name-many              '    for (size_t k = 0; k < s->n_names; k++) if (memcmp(s->names[k].id, id, 32) == 0) return OMEGA_VCS_VCSTORE_NAME_EXISTS;'; \
	  build_mutant name-no-silent-rebind    name-no-silent-rebind  '        { memcpy(s->names[np].id, id, 32); return OMEGA_VCS_OK; }'; \
	  build_mutant rebind-write             rebind-write           '    ;'; \
	  build_mutant rebind-exists            rebind-exists          '    if (!name_find(s, name, &np)) np = 0;'; \
	  build_mutant rebind-id-exists         rebind-id-exists       '    (void)vcs_find(s, id, &di);'; \
	  build_mutant name-id-exists           name-id-exists         '    (void)vcs_find(s, id, &di);'; \
	  build_mutant name-valid               name-valid             '    ;'; \
	  build_mutant resolve                  resolve                '    memset(out_id, 0, 32);'; \
	  build_mutant load-names               load-names             '        ;'; \
	  build_mutant load-fail                load-fail              '        omega_vcstore_destroy(s);'; \
	  build_mutant load-magic               load-magic             '    if (!m) return OMEGA_VCS_VCSTORE_MALFORMED;'; \
	  build_mutant load-digest-obj          load-digest-obj        '    ;'; \
	  build_mutant load-digest-names        load-digest-names      '    ;'; \
	  build_mutant load-trailing            load-trailing          '    ;'; \
	  build_mutant load-kind                load-kind              '        ;'; \
	  build_mutant load-order               load-order             '        ;'; \
	  build_mutant load-graph               load-graph             '    (void)vcs_graph_check;'; \
	  build_mutant load-graph-missing       load-graph             '    (void)vcs_graph_check;'; \
	  build_mutant load-name-id             load-name-id           '        (void)vcs_find(t, id, &di);'; \
	  build_mutant save-verify              save-verify            '        int rc = 0;'
	@echo "test-vcstore: PASS (all checks, all mutants killed)"

# VC1-RESOLVE (VC1 stage 4): import resolution in the Omega OSC compiler. `import NAME;` resolves
# only through omega.lock -> semantic id -> Verified Crumb Store -> receipt check -> transitive
# verified closure (src/omega_resolve.c), with a C reader of the aien-proof receipt wire contract
# (src/omega_receipt.c, BLAKE3 in src/omega_blake3.c). oscv is the verified driver. Test inputs:
# tests/resolve/ (official BLAKE3 vectors, three receipts written by the Rust implementation).
# Mutation proof: each mutant is a copy of ONE source file with one tagged line (VC1R:<tag> in
# omega_resolve.c / omega_receipt.c, B3M:<tag> in omega_blake3.c) replaced; the test must exit 1
# with "MUTANT <name> KILLED by <check>" for the check named here, every time. A mutant whose sed
# changed nothing, did not compile, or survived fails the build. CPU only; no physics; no chip.
.PHONY: test-resolve oscv
RESOLVE_FLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc -Isrc/compiler -Isrc/compiler/model
RESOLVE_DIR = $(OUT_DIR)/resolve-test
RESOLVE_OSC_LIB = $(filter-out src/compiler/oscc_main.c,$(wildcard src/compiler/*.c))
RESOLVE_SRC = src/omega_resolve.c src/omega_receipt.c src/omega_blake3.c
RESOLVE_REST = src/omega_resolve_osc.c src/omega_vcstore.c $(RESOLVE_OSC_LIB) src/sha256.c
RESOLVE_HDRS = $(wildcard src/compiler/*.h) src/omega_resolve.h src/omega_receipt.h src/omega_blake3.h src/omega_resolve_osc.h src/omega_vcstore.h src/sha256.h
$(OUT_DIR)/compiler/oscv: src/oscv_main.c $(RESOLVE_SRC) $(RESOLVE_REST) $(RESOLVE_HDRS)
	@mkdir -p $(OUT_DIR)/compiler
	$(CC) $(RESOLVE_FLAGS) -o $@ src/oscv_main.c $(RESOLVE_SRC) $(RESOLVE_REST)
oscv: $(OUT_DIR)/compiler/oscv
test-resolve: tests/test_omega_resolve.c $(OUT_DIR)/compiler/oscv $(RESOLVE_SRC) $(RESOLVE_REST) $(RESOLVE_HDRS)
	@mkdir -p $(RESOLVE_DIR)
	$(CC) $(RESOLVE_FLAGS) -DOSCV_PATH='"$(OUT_DIR)/compiler/oscv"' -o $(RESOLVE_DIR)/test_omega_resolve tests/test_omega_resolve.c $(RESOLVE_SRC) $(RESOLVE_REST)
	$(RESOLVE_DIR)/test_omega_resolve
	$(CC) $(RESOLVE_FLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DOSCV_PATH='"$(OUT_DIR)/compiler/oscv"' -o $(RESOLVE_DIR)/test_omega_resolve_asan tests/test_omega_resolve.c $(RESOLVE_SRC) $(RESOLVE_REST)
	$(RESOLVE_DIR)/test_omega_resolve_asan
	@set -eu; build_mutant() { name=$$1; tag=$$2; which=$$3; chk=$$4; repl=$$5; \
	  case $$which in blake3) f=src/omega_blake3.c; pfx=B3M;; resolve) f=src/omega_resolve.c; pfx=VC1R;; receipt) f=src/omega_receipt.c; pfx=VC1R;; *) echo "bad mutant file $$which"; exit 1;; esac; \
	  sed "/$$pfx:$$tag/c\\$$repl" $$f > $(RESOLVE_DIR)/mut_$$name.c; \
	  if cmp -s $$f $(RESOLVE_DIR)/mut_$$name.c; then echo "mutant $$name: sed changed nothing"; exit 1; fi; \
	  srcs=""; for s in $(RESOLVE_SRC); do if [ "$$s" = "$$f" ]; then srcs="$$srcs $(RESOLVE_DIR)/mut_$$name.c"; else srcs="$$srcs $$s"; fi; done; \
	  $(CC) $(RESOLVE_FLAGS) -DOSCV_PATH='"$(OUT_DIR)/compiler/oscv"' -o $(RESOLVE_DIR)/mut_$$name tests/test_omega_resolve.c $$srcs $(RESOLVE_REST); \
	  rc=0; $(RESOLVE_DIR)/mut_$$name $$name > $(RESOLVE_DIR)/mut_$$name.out 2>&1 || rc=$$?; \
	  if [ $$rc -ne 1 ] || ! grep -q "^MUTANT $$name KILLED by $$chk\$$" $(RESOLVE_DIR)/mut_$$name.out; then \
	    echo "mutant $$name NOT killed by $$chk (exit $$rc)"; tail -5 $(RESOLVE_DIR)/mut_$$name.out; exit 1; fi; \
	  echo "mutant $$name: killed by $$chk (exit 1)"; }; \
	  build_mutant b3-merge                 merge                  blake3   b3-official-vectors                              '        while ((total & 1) == 0 && sp > 1) {'; \
	  build_mutant b3-root                  root                    blake3  b3-official-vectors                          '    cur.flags |= 0;'; \
	  build_mutant profile-known            profile-known          resolve  profile-unknown-refused                          '    if (!p) p = table;'; \
	  build_mutant profile-version          profile-version        resolve profile-version-too-old-refused              '    if (cmp < -1)'; \
	  build_mutant lock-bytes-tab           lock-bytes             resolve  lock-refuses-tab-in-comment                      '        if (text[i] == 13 || text[i] == 0) return lock_bad(err, 1, "CR or NUL byte");'; \
	  build_mutant lock-bytes-cr            lock-bytes             resolve  lock-refuses-cr-in-comment                       '        if (text[i] == 9 || text[i] == 0) return lock_bad(err, 1, "tab or NUL byte");'; \
	  build_mutant lock-bytes-nul           lock-bytes             resolve  lock-refuses-nul-in-comment                      '        if (text[i] == 9 || text[i] == 13) return lock_bad(err, 1, "tab or CR byte");'; \
	  build_mutant lock-header              lock-header            resolve lock-refuses-wrong-header                    '            if (ll != 13) { free(ents); return lock_bad(err, line, "header"); }'; \
	  build_mutant lock-unknown-line        lock-unknown-line      resolve lock-refuses-bad-name-start                  '        if (0) { free(ents); return lock_bad(err, line, "unknown line"); }'; \
	  build_mutant lock-hex                 lock-hex               resolve  lock-refuses-uppercase-hex                   '        if (unhex32(h1, en.semantic) || unhex32(h2, en.receipt)) { memset(en.semantic, 1, 32); memset(en.receipt, 1, 32); }'; \
	  build_mutant lock-order-duplicate     lock-order             resolve lock-refuses-duplicate-name                  '        if (n > 0 && strcmp(ents[n - 1].name, en.name) > 0) {'; \
	  build_mutant lock-order-sorted        lock-order             resolve lock-refuses-unsorted                        '        if (n > 0 && strcmp(ents[n - 1].name, en.name) == 0) {'; \
	  build_mutant missing-receipt          missing-receipt        resolve refuse-missing-receipt                       '    if (fr != 0) return fail(err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, sid, "receipt unavailable");'; \
	  build_mutant rule1-name               rule1-name             resolve refuse-receipt-file-holds-another-receipt    '    if (0) {'; \
	  build_mutant rule-clean               rule-clean             resolve refuse-dirty-receipt-in-build                '    if (0) {'; \
	  build_mutant rule-clean-admit         rule-clean             resolve admit-refuses-dirty-receipt-in-build         '    if (0) {'; \
	  build_mutant rule4-semantic           rule4-semantic         resolve refuse-receipt-does-not-name-program         '    if (0) {'; \
	  build_mutant rule4-source             rule4-source           resolve refuse-stale-receipt                         '        if (0) {'; \
	  build_mutant rule3                    rule3                  resolve refuse-receipt-output-digest-is-not-evidence-root '    if (0) {'; \
	  build_mutant rule5                    rule5                  resolve refuse-receipt-kind-is-not-profile           '    if (0) {'; \
	  build_mutant rule6                    rule6                  resolve  refuse-receipt-has-extra-dependency              '        if ((a != nrd || (a && memcmp(deps, rdeps, a * 32) != 0)) && 0) {'; \
	  build_mutant rule2-pass               rule2-pass             resolve refuse-receipt-result-is-not-pass            '    if (0) {'; \
	  build_mutant rule2-assert             rule2-assert           resolve refuse-receipt-assertion-failed              '        if (0) {'; \
	  build_mutant rule2-testonly           rule2-testonly         resolve refuse-receipt-test-only-trust               '    if (0) {'; \
	  build_mutant rule2-tier               rule2-tier             resolve  refuse-receipt-tier-below-profile                '    if (!omega_tier_satisfies(need_rank, rcpt.tier_rank) && 0) {'; \
	  build_mutant closure-receipt          closure-receipt        resolve closure-digest-matches-reference             '        (void)e[i].receipt_id;'; \
	  build_mutant cycle                    cycle                  resolve refuse-dependency-cycle                      '        if (0) return 0;'; \
	  build_mutant lock-receipt             lock-receipt           resolve refuse-lock-pins-another-receipt             '    if (0) {'; \
	  build_mutant lock-receipt-visited     visited-lock-receipt  resolve refuse-lock-pins-another-receipt-for-visited-node '        if (0)'; \
	  build_mutant genesis-gate             genesis-gate           resolve refuse-genesis-record-without-permission     '    if (0) {'; \
	  build_mutant taint-cap                taint-cap              resolve refuse-tainted-record-in-build-domain        '            0) {'; \
	  build_mutant verified-needs-receipt   verified-needs-receipt resolve refuse-missing-receipt                       '    if (0) {'; \
	  build_mutant edge-contract            edge-contract          resolve  refuse-edge-contract-mismatch                    '        if (memcmp(w->nodes[di].contract, req, 32) != 0 && 0) {'; \
	  build_mutant boot-dep-verified        boot-dep-verified      resolve refuse-genesis-record-depending-on-verified  '        if (0) {'; \
	  build_mutant not-pinned               not-pinned             resolve  refuse-not-pinned-without-lock                   '        if (0 && !omega_lock_find(lock, names[i]))'; \
	  build_mutant undeclared               undeclared             resolve  refuse-undeclared-lock-line-without-import       '        if (!used && 0)'; \
	  build_mutant closure-sort             closure-sort           resolve closure-sorted-ascending-by-semantic-id      '    (void)cmp_entry;'; \
	  build_mutant build-domain             build-domain           resolve  build-id-matches-reference                       '    uint8_t db = (uint8_t)d & 0;'; \
	  build_mutant build-closure            build-closure          resolve build-id-matches-reference                   '    (void)closure_digest;'; \
	  build_mutant taint-mark               taint-mark             resolve artifact-dev-is-tainted-build-is-not         '    m->tainted = 0;'; \
	  build_mutant meta-taint-consistent    meta-taint-consistent  resolve artifact-refuses-dev-header-marked-untainted '    if (0)'; \
	  build_mutant meta-build-id            meta-build-id          resolve artifact-refuses-edited-build-id             '    if (0)'; \
	  build_mutant admit-origin             admit-origin           resolve  admit-refuses-record-from-tainted-origin         '    if (origin && (origin->tainted || origin->domain == OMEGA_DOMAIN_DEV) && 0)'; \
	  build_mutant admit-cap                admit-cap              resolve admit-refuses-taint-capability               '    if (0) {'; \
	  build_mutant admit-receipt            admit-receipt          resolve  admit-refuses-mismatched-receipt                 '    rc = 0; (void)rr;'; \
	  build_mutant genesis-dep              genesis-dep            resolve  admit-genesis-refuses-verified-dependency        '        if ((omega_vcstore_admission_kind(s, view->dependencies + 64 * (size_t)i, &k) != 0 || k != OMEGA_VCS_ADMISSION_BOOTSTRAP) && 0) {'; \
	  build_mutant blob-digest              blob-digest            resolve refuse-blob-does-not-hash-to-digest          '        if (0) {'; \
	  build_mutant json-dup-key             json-dup-key           receipt receipt-refuses-duplicate-key                '                if (0) { free(key); jfree(j); return jfail(p, "duplicate key"); }'; \
	  build_mutant deny-unknown             deny-unknown           receipt receipt-refuses-unknown-field                '    if (!all_known_key(root) && 0) { ef(&e, "receipt: unknown field%s", ""); goto done; }'; \
	  build_mutant schema                   schema                 receipt receipt-refuses-unknown-schema               '    if (strcmp(schema, SCHEMA) != 0 && 0) { ef(&e, "receipt: unknown receipt schema%s", ""); goto done; }'; \
	  build_mutant version                  version                receipt receipt-refuses-unknown-version              '    if (r->version != 1 && 0) { ef(&e, "receipt: unsupported receipt version%s", ""); goto done; }'; \
	  build_mutant feature                  feature                receipt receipt-refuses-unknown-feature              '        if (0) { ef(&e, "receipt: unknown required receipt feature%s", ""); goto done; }'; \
	  build_mutant reserved                 reserved               receipt receipt-refuses-reserved-field               '    if (reserved[0] && 0) { ef(&e, "receipt: reserved field must be empty%s", ""); goto done; }'; \
	  build_mutant env-class                env-class              receipt receipt-refuses-env-class-mismatch           '    if (strcmp(r->env_class, r->tier) != 0 && 0) { ef(&e, "receipt: env_class contradicts tier%s", ""); goto done; }'; \
	  build_mutant mutation-order           mutation-order         receipt receipt-refuses-observed-over-declared       '    if (MUTS[r->observed_mutation].sev > MUTS[r->declared_mutation].sev && 0) { ef(&e, "receipt: observed mutation exceeds declared mutation%s", ""); goto done; }'; \
	  build_mutant canon-sort               canon-sort             receipt receipt-canon-order-independent              '    (void)cmp_str;'; \
	  build_mutant id-compare               id-compare             receipt receipt-refuses-wrong-id                     '    if (0) {'; \
	  build_mutant max-depth-removed        max-depth              resolve  refuse-dependency-chain-too-deep                 '    if (0 && depth >= MAX_DEPTH) return fail(w->err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, sid, "dependency chain deeper than %d", MAX_DEPTH);'; \
	  build_mutant max-depth-one-too-many   max-depth              resolve  refuse-dependency-chain-too-deep                 '    if (depth > MAX_DEPTH) return fail(w->err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, sid, "dependency chain deeper than %d", MAX_DEPTH);'; \
	  build_mutant max-depth-one-too-few    max-depth              resolve  accept-dependency-chain-at-the-limit             '    if (depth >= MAX_DEPTH - 1) return fail(w->err, OMEGA_RES_UNVERIFIED_DEPENDENCY, 0, sid, "dependency chain deeper than %d", MAX_DEPTH);'; \
	  build_mutant dup-import               dup-import             resolve  refuse-duplicate-import-names-through-the-api    '            if (0) return fail(err, OMEGA_RES_BAD_ARGUMENT, 0, names[i], "import listed twice");'; \
	  build_mutant pass-needs-assertion     pass-needs-assertion   receipt  receipt-refuses-pass-without-assertion           '    if (r->n_assertions == 0 && r->result == OMEGA_RECEIPT_PASS && 0) { ef(&e, "receipt: PASS receipts must carry at least one assertion%s", ""); goto done; }'; \
	  build_mutant prod-authority-empty     prod-authority         receipt  receipt-refuses-production-blank-authority       '        if (a == b && 0) { ef(&e, "receipt: PRODUCTION receipts require an authority reference%s", ""); goto done; }'; \
	  build_mutant prod-authority-trim      prod-authority         receipt  receipt-refuses-production-blank-authority       '        if (b == 0) { ef(&e, "receipt: PRODUCTION receipts require an authority reference%s", ""); goto done; }'; \
	  build_mutant prod-spelling            prod-spelling          receipt  receipt-refuses-production-test-only-authority   '        int bad = strstr(low, "test-only") || strstr(low, "testonly");'; \
	  build_mutant prod-bad-refused         prod-bad-refused       receipt  receipt-refuses-production-test-only-authority   '        if (bad && 0) { ef(&e, "receipt: a TEST_ONLY signer cannot produce a PRODUCTION receipt%s", ""); goto done; }'
	@echo "test-resolve: PASS (all checks, ASan/UBSan clean, all 66 mutants killed)"

# VC1-GENESIS (VC1 stage 6): the pinned Genesis Set, the private store doors, the mandatory
# source/IR recheck in the build domain. The resolver and the store now rebuild the program from
# its IR (src/omega_program_ir.c), so the resolver link set grows by the program stack. These
# additions are at the end on purpose: `+=` on a recursive variable is read by every recipe above.
RESOLVE_PROGRAM_STACK = src/omega_program_ir.c src/omega_program.c src/omega_canonical.c src/omega_validate.c src/omega_core.c src/omega_codec.c src/aarch64_encoder.c src/aarch64_decoder.c src/omega_realize.c src/omega_exec.c src/omega_verify.c src/omega_realize_synth.c src/omega_machine.c
RESOLVE_REST += $(RESOLVE_PROGRAM_STACK)
RESOLVE_HDRS += src/omega_genesis.h src/omega_vcstore_priv.h src/omega_program_ir.h tests/vc_fixture.h

# test-genesis: the LISTED-member behavior of VC-GENESIS-1, which the empty real set cannot show.
# The variant header is GENERATED from src/omega_genesis.h (same functions, byte for byte; only the
# count, the set name and one table row differ), and omega_vcstore.c and omega_resolve.c are copied
# with ONE rewritten include line so they read it. A quote include looks in the including file's own
# directory first, which is why -I cannot swap the real header and why the copy is made. Nothing in
# production source can select the variant. Mutants below break one guard each in the copies.
.PHONY: test-genesis
GENESIS_DIR = $(OUT_DIR)/genesis-test
GENESIS_STACK = src/omega_receipt.c src/omega_blake3.c src/sha256.c $(RESOLVE_PROGRAM_STACK)
GENESIS_CFLAGS = $(RESOLVE_FLAGS) -I$(GENESIS_DIR) -Itests
test-genesis: tests/test_omega_genesis_set.c tests/genesis_variant/member.inc src/omega_vcstore.c src/omega_resolve.c src/omega_genesis.h $(RESOLVE_HDRS)
	@set -eu; mkdir -p $(GENESIS_DIR)/genesis_variant; \
	sed -e 's|^#define OMEGA_GENESIS_1_COUNT 0u|#define OMEGA_GENESIS_1_COUNT 1u|' -e 's|^#define OMEGA_GENESIS_SET_NAME "VC-GENESIS-1"|#define OMEGA_GENESIS_SET_NAME "VC-GENESIS-1-TEST-VARIANT"|' src/omega_genesis.h \
	  | awk -v f=tests/genesis_variant/member.inc '/^    \{ 0 \}$$/ { while ((getline l < f) > 0) print l } { print }' > $(GENESIS_DIR)/genesis_variant/omega_genesis.h; \
	grep -q '^#define OMEGA_GENESIS_1_COUNT 1u' $(GENESIS_DIR)/genesis_variant/omega_genesis.h; \
	grep -q 'TEST-VARIANT' $(GENESIS_DIR)/genesis_variant/omega_genesis.h; \
	[ "$$(grep -c '0x8f, 0x0b' $(GENESIS_DIR)/genesis_variant/omega_genesis.h)" = 1 ]; \
	for f in omega_vcstore omega_resolve; do \
	  sed 's|#include "omega_genesis.h"|#include "genesis_variant/omega_genesis.h"|' src/$$f.c > $(GENESIS_DIR)/$$f.c; \
	  if cmp -s src/$$f.c $(GENESIS_DIR)/$$f.c; then echo "test-genesis: include line not found in $$f.c"; exit 1; fi; done
	$(CC) $(GENESIS_CFLAGS) -o $(GENESIS_DIR)/test_omega_genesis_set tests/test_omega_genesis_set.c $(GENESIS_DIR)/omega_vcstore.c $(GENESIS_DIR)/omega_resolve.c $(GENESIS_STACK)
	$(GENESIS_DIR)/test_omega_genesis_set
	$(CC) $(GENESIS_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o $(GENESIS_DIR)/test_omega_genesis_set_asan tests/test_omega_genesis_set.c $(GENESIS_DIR)/omega_vcstore.c $(GENESIS_DIR)/omega_resolve.c $(GENESIS_STACK)
	$(GENESIS_DIR)/test_omega_genesis_set_asan
	@set -eu; gmut() { name=$$1; which=$$2; tag=$$3; chk=$$4; repl=$$5; \
	  f=$(GENESIS_DIR)/$$which.c; sed "/$$tag/c\\$$repl" $$f > $(GENESIS_DIR)/mut_$$name.c; \
	  if cmp -s $$f $(GENESIS_DIR)/mut_$$name.c; then echo "mutant $$name: sed changed nothing"; exit 1; fi; \
	  if [ $$which = omega_vcstore ]; then a=$(GENESIS_DIR)/mut_$$name.c; b=$(GENESIS_DIR)/omega_resolve.c; else a=$(GENESIS_DIR)/omega_vcstore.c; b=$(GENESIS_DIR)/mut_$$name.c; fi; \
	  $(CC) $(GENESIS_CFLAGS) -o $(GENESIS_DIR)/mut_$$name tests/test_omega_genesis_set.c $$a $$b $(GENESIS_STACK); \
	  rc=0; $(GENESIS_DIR)/mut_$$name > $(GENESIS_DIR)/mut_$$name.out 2>&1 || rc=$$?; \
	  if [ $$rc -eq 0 ] || ! grep -q "^FAIL $$chk\$$" $(GENESIS_DIR)/mut_$$name.out; then echo "mutant $$name NOT killed by $$chk (exit $$rc)"; tail -5 $(GENESIS_DIR)/mut_$$name.out; exit 1; fi; \
	  echo "mutant $$name: killed by $$chk (exit $$rc)"; }; \
	  gmut gv-insert-refuses-listed   omega_vcstore VC1S:insert-genesis      store-inserts-a-listed-bootstrap-record  '    if (kind == OMEGA_VCS_ADMISSION_BOOTSTRAP) { free(rec); return OMEGA_VCS_GENESIS_NOT_LISTED; }'; \
	  gmut gv-insert-accepts-unlisted omega_vcstore VC1S:insert-genesis      store-refuses-an-unlisted-bootstrap-record '    if (0) { free(rec); return OMEGA_VCS_GENESIS_NOT_LISTED; }'; \
	  gmut gv-load-refuses-listed     omega_vcstore VC1S:load-genesis        store-saves-listed-bootstrap-and-verified-and-loads-them '        if (kind == OMEGA_VCS_ADMISSION_BOOTSTRAP) { rc = OMEGA_VCS_GENESIS_NOT_LISTED; break; }'; \
	  gmut gv-load-accepts-unlisted   omega_vcstore VC1S:load-genesis        store-load-refuses-a-file-with-an-unlisted-bootstrap-record '        if (0) { rc = OMEGA_VCS_GENESIS_NOT_LISTED; break; }'; \
	  gmut gv-gate-refuses-listed     omega_resolve VC1R:genesis-gate        resolver-lets-a-listed-bootstrap-record-satisfy-an-import-in-the-build-domain '    if (rec->admission_kind == OMEGA_VCS_ADMISSION_BOOTSTRAP) {'; \
	  gmut gv-gate-accepts-unlisted   omega_resolve VC1R:genesis-gate        resolver-refuses-an-unlisted-bootstrap-record-even-with-the-list-non-empty '    if (0) {'; \
	  gmut gv-admit-refuses-listed    omega_resolve VC1R:admit-genesis-list  admit-genesis-admits-a-listed-member '    if (1) {'; \
	  gmut gv-admit-accepts-unlisted  omega_resolve VC1R:admit-genesis-list  admit-genesis-refuses-a-record-that-is-not-a-member '    if (0) {'
	@echo "test-genesis: PASS (listed and unlisted at every door, ASan/UBSan clean, 8 mutants killed)"

# test-program-ir: the canonical program IR (src/omega_program_ir.c). Each VC1I-tagged guard is
# broken in a copy and the named check must FAIL.
.PHONY: test-program-ir
PIR_DIR = $(OUT_DIR)/program-ir-test
PIR_STACK = src/sha256.c $(filter-out src/omega_program_ir.c,$(RESOLVE_PROGRAM_STACK))
test-program-ir: tests/test_omega_program_ir.c src/omega_program_ir.c src/omega_program_ir.h
	@mkdir -p $(PIR_DIR)
	$(CC) $(RESOLVE_FLAGS) -Wno-pedantic -o $(PIR_DIR)/test_omega_program_ir tests/test_omega_program_ir.c src/omega_program_ir.c $(PIR_STACK)
	$(PIR_DIR)/test_omega_program_ir
	$(CC) $(RESOLVE_FLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o $(PIR_DIR)/test_omega_program_ir_asan tests/test_omega_program_ir.c src/omega_program_ir.c $(PIR_STACK)
	$(PIR_DIR)/test_omega_program_ir_asan
	@set -eu; pmut() { name=$$1; tag=$$2; chk=$$3; repl=$$4; \
	  sed "/VC1I:$$tag/c\\$$repl" src/omega_program_ir.c > $(PIR_DIR)/mut_$$name.c; \
	  if cmp -s src/omega_program_ir.c $(PIR_DIR)/mut_$$name.c; then echo "mutant $$name: sed changed nothing"; exit 1; fi; \
	  $(CC) $(RESOLVE_FLAGS) -Isrc -o $(PIR_DIR)/mut_$$name tests/test_omega_program_ir.c $(PIR_DIR)/mut_$$name.c $(PIR_STACK); \
	  rc=0; $(PIR_DIR)/mut_$$name > $(PIR_DIR)/mut_$$name.out 2>&1 || rc=$$?; \
	  if [ $$rc -eq 0 ] || ! grep -q "^FAIL $$chk\$$" $(PIR_DIR)/mut_$$name.out; then echo "mutant $$name NOT killed by $$chk (exit $$rc)"; tail -5 $(PIR_DIR)/mut_$$name.out; exit 1; fi; \
	  echo "mutant $$name: killed by $$chk (exit $$rc)"; }; \
	  pmut ir-tag       tag       ir-refuses-wrong-tag                  '    if (!tag) return -1;'; \
	  pmut ir-trailing  trailing  ir-refuses-trailing-bytes             '    if (r.bad) { memset(out, 0, sizeof *out); return -1; }'; \
	  pmut ir-id        id        ir-refuses-a-type-the-program-id-cannot-take '    (void)0;'
	@set -eu; sed '/VC1I:truncated/c\    if (r->bad) { r->bad = 1; return NULL; }' src/omega_program_ir.c > $(PIR_DIR)/mut_ir-truncated.c; \
	  if cmp -s src/omega_program_ir.c $(PIR_DIR)/mut_ir-truncated.c; then echo "mutant ir-truncated: sed changed nothing"; exit 1; fi; \
	  $(CC) $(RESOLVE_FLAGS) -Isrc -O1 -g -fsanitize=address -o $(PIR_DIR)/mut_ir-truncated tests/test_omega_program_ir.c $(PIR_DIR)/mut_ir-truncated.c $(PIR_STACK); \
	  rc=0; $(PIR_DIR)/mut_ir-truncated > $(PIR_DIR)/mut_ir-truncated.out 2>&1 || rc=$$?; \
	  if [ $$rc -eq 0 ] || ! grep -q 'AddressSanitizer: heap-buffer-overflow' $(PIR_DIR)/mut_ir-truncated.out; then echo "mutant ir-truncated NOT killed (exit $$rc)"; tail -5 $(PIR_DIR)/mut_ir-truncated.out; exit 1; fi; \
	  echo "mutant ir-truncated: killed by AddressSanitizer heap-buffer-overflow on a truncated blob (exit $$rc)"
	@set -eu; sed '/VC1I:steps/c\    if (r.bad) { memset(out, 0, sizeof *out); return -1; }' src/omega_program_ir.c > $(PIR_DIR)/mut_ir-steps.c; \
	  if cmp -s src/omega_program_ir.c $(PIR_DIR)/mut_ir-steps.c; then echo "mutant ir-steps: sed changed nothing"; exit 1; fi; \
	  $(CC) $(RESOLVE_FLAGS) -Isrc -O1 -g -fsanitize=address -o $(PIR_DIR)/mut_ir-steps tests/test_omega_program_ir.c $(PIR_DIR)/mut_ir-steps.c $(PIR_STACK); \
	  rc=0; $(PIR_DIR)/mut_ir-steps > $(PIR_DIR)/mut_ir-steps.out 2>&1 || rc=$$?; \
	  if [ $$rc -eq 0 ] || ! grep -q 'AddressSanitizer: heap-buffer-overflow' $(PIR_DIR)/mut_ir-steps.out; then echo "mutant ir-steps NOT killed (exit $$rc)"; tail -5 $(PIR_DIR)/mut_ir-steps.out; exit 1; fi; \
	  echo "mutant ir-steps: killed by AddressSanitizer heap-buffer-overflow on a huge step count (exit $$rc)"
	@echo "test-program-ir: PASS (round trip, refusals, ASan/UBSan clean, 5 mutants killed)"

# The resolver mutants for the VC1 stage 6 guards (genesis list at admission, mandatory blob store
# in the build domain, program-id recompute, IR-only digest kind in the build domain), plus the two
# genesis guards in the store. Same method as test-resolve and test-vcstore: copy ONE source file,
# break ONE tagged line, require the named check to fail. Run after the two main targets.
.PHONY: test-resolve-genesis test-vcstore-genesis
test-resolve-genesis: test-resolve
	@set -eu; build_mutant() { name=$$1; tag=$$2; chk=$$3; repl=$$4; f=src/omega_resolve.c; \
	  sed "/VC1R:$$tag/c\\$$repl" $$f > $(RESOLVE_DIR)/mut_$$name.c; \
	  if cmp -s $$f $(RESOLVE_DIR)/mut_$$name.c; then echo "mutant $$name: sed changed nothing"; exit 1; fi; \
	  srcs=""; for s in $(RESOLVE_SRC); do if [ "$$s" = "$$f" ]; then srcs="$$srcs $(RESOLVE_DIR)/mut_$$name.c"; else srcs="$$srcs $$s"; fi; done; \
	  $(CC) $(RESOLVE_FLAGS) -DOSCV_PATH='"$(OUT_DIR)/compiler/oscv"' -o $(RESOLVE_DIR)/mut_$$name tests/test_omega_resolve.c $$srcs $(RESOLVE_REST); \
	  rc=0; $(RESOLVE_DIR)/mut_$$name $$name > $(RESOLVE_DIR)/mut_$$name.out 2>&1 || rc=$$?; \
	  if [ $$rc -ne 1 ] || ! grep -q "^MUTANT $$name KILLED by $$chk\$$" $(RESOLVE_DIR)/mut_$$name.out; then \
	    echo "mutant $$name NOT killed by $$chk (exit $$rc)"; tail -5 $(RESOLVE_DIR)/mut_$$name.out; exit 1; fi; \
	  echo "mutant $$name: killed by $$chk (exit 1)"; }; \
	  build_mutant admit-genesis-list   admit-genesis-list   admit-genesis-refuses-unlisted-record                    '    if (0) { /* VC1R:admit-genesis-list */'; \
	  build_mutant build-needs-blobs    build-needs-blobs    refuse-build-domain-without-blob-store                    '    if (0 && !r->fetch_blob) { /* VC1R:build-needs-blobs */'; \
	  build_mutant program-id           program-id           refuse-ir-that-does-not-recompute-to-semantic-id          '            if (omega_program_ir_recompute_id(bb, bl, pid) != 0) { /* VC1R:program-id */'; \
	  build_mutant ir-kind              ir-kind              refuse-source-digest-record-in-build-domain               '        } else if (0) { /* VC1R:ir-kind */'
	@echo "test-resolve-genesis: PASS (4 mutants killed)"
test-vcstore-genesis: test-vcstore
	@set -eu; build_mutant() { name=$$1; tag=$$2; chk=$$3; repl=$$4; \
	  sed "/VC1S:$$tag/c\\$$repl" src/omega_vcstore.c > $(VCSTEST_DIR)/mut_$$name.c; \
	  if cmp -s src/omega_vcstore.c $(VCSTEST_DIR)/mut_$$name.c; then echo "mutant $$name: sed changed nothing"; exit 1; fi; \
	  $(CC) $(VCSTEST_FLAGS) -o $(VCSTEST_DIR)/mut_$$name tests/test_omega_vcstore.c $(VCSTEST_DIR)/mut_$$name.c src/sha256.c; \
	  rc=0; $(VCSTEST_DIR)/mut_$$name $$name > $(VCSTEST_DIR)/mut_$$name.out 2>&1 || rc=$$?; \
	  if [ $$rc -ne 1 ] || ! grep -q "^MUTANT $$name KILLED by $$chk\$$" $(VCSTEST_DIR)/mut_$$name.out; then \
	    echo "mutant $$name NOT killed by $$chk (exit $$rc)"; tail -5 $(VCSTEST_DIR)/mut_$$name.out; exit 1; fi; \
	  echo "mutant $$name: killed by $$chk (exit 1)"; }; \
	  build_mutant insert-genesis       insert-genesis       insert-bootstrap-unlisted-refused  '    if (0) { free(rec); return OMEGA_VCS_GENESIS_NOT_LISTED; }'; \
	  build_mutant load-genesis         load-genesis         load-bootstrap-unlisted-refused    '        if (0) { rc = OMEGA_VCS_GENESIS_NOT_LISTED; break; }'
	@echo "test-vcstore-genesis: PASS (2 mutants killed)"

# VC1 stage 6: library admissions in Crumbline and in omegatool go through the Verified Crumb bridge
# (src/omega_vc_bridge.c), so both link the bridge, the resolver, the receipt reader and the store.
# `+=` at the end is read by the recipes above (they use the variables lazily); the extra
# prerequisite rules below make sure the new objects are built first.
VC_BRIDGE_CORE = omega_program_ir omega_vc_bridge omega_resolve omega_receipt omega_blake3 omega_vcstore
VC_BRIDGE_OBJS = $(addprefix $(OUT_DIR)/,$(addsuffix .o,$(VC_BRIDGE_CORE)))
LEARNER_CORE += $(VC_BRIDGE_CORE)
$(LEARNER): $(VC_BRIDGE_OBJS)
SRCS += $(patsubst %,src/%.c,$(VC_BRIDGE_CORE))
$(TARGET): $(VC_BRIDGE_OBJS)
-include $(VC_BRIDGE_OBJS:.o=.d)

# test-genesis-real: the REAL VC-GENESIS-1 (src/omega_genesis.h) pinned against its audit record
# docs/osc/VC-GENESIS-1.md. Two mutants of the real header must be caught.
.PHONY: test-genesis-real
GENREAL_DIR = $(OUT_DIR)/genesis-real-test
GENREAL_STACK = src/sha256.c $(RESOLVE_PROGRAM_STACK)
test-genesis-real: tests/test_omega_genesis.c src/omega_genesis.h docs/osc/VC-GENESIS-1.md tests/vc_fixture.h
	@mkdir -p $(GENREAL_DIR)
	$(CC) $(RESOLVE_FLAGS) -o $(GENREAL_DIR)/test_omega_genesis tests/test_omega_genesis.c $(GENREAL_STACK)
	$(GENREAL_DIR)/test_omega_genesis
	@set -eu; gmut() { name=$$1; chk=$$2; expr=$$3; \
	  mkdir -p $(GENREAL_DIR)/$$name; sed "$$expr" src/omega_genesis.h > $(GENREAL_DIR)/$$name/omega_genesis.h; \
	  if cmp -s src/omega_genesis.h $(GENREAL_DIR)/$$name/omega_genesis.h; then echo "mutant $$name: sed changed nothing"; exit 1; fi; \
	  $(CC) -I$(GENREAL_DIR)/$$name $(RESOLVE_FLAGS) -o $(GENREAL_DIR)/mut_$$name tests/test_omega_genesis.c $(GENREAL_STACK); \
	  rc=0; $(GENREAL_DIR)/mut_$$name > $(GENREAL_DIR)/mut_$$name.out 2>&1 || rc=$$?; \
	  if [ $$rc -ne 1 ] || ! grep -q "^FAIL $$chk\$$" $(GENREAL_DIR)/mut_$$name.out; then echo "mutant $$name NOT killed by $$chk (exit $$rc)"; tail -5 $(GENREAL_DIR)/mut_$$name.out; exit 1; fi; \
	  echo "mutant $$name: killed by $$chk (exit 1)"; }; \
	  gmut gr-count-one          member-count-is-the-pinned-audit-result 's|^#define OMEGA_GENESIS_1_COUNT 0u|#define OMEGA_GENESIS_1_COUNT 1u|'; \
	  gmut gr-contains-everything no-program-we-know-and-no-junk-id-is-a-member 's|if (!id \|\| memcmp(id, zero, 32) == 0) return 0;|(void)zero; return 1;|'; \
	  gmut gr-name-changed       set-name-is-vc-genesis-1 's|#define OMEGA_GENESIS_SET_NAME "VC-GENESIS-1"|#define OMEGA_GENESIS_SET_NAME "VC-GENESIS-2"|'
	@echo "test-genesis-real: PASS (pinned set and audit record agree, 3 mutants killed)"

# test-vc-bridge (VC1 stage 6 fix): the bridge verifies and recomputes the id itself, and what it
# mints cannot satisfy a build import. Each VC1B-tagged guard is broken in a copy and the named
# check must FAIL. The tag lines are single lines, so sed replaces whole lines.
.PHONY: test-vc-bridge
VCB_DIR = $(OUT_DIR)/vc-bridge-test
VCB_STACK = src/omega_library.c src/omega_receipt.c src/omega_blake3.c src/sha256.c src/omega_vcstore.c $(RESOLVE_PROGRAM_STACK)
test-vc-bridge: tests/test_omega_vc_bridge.c src/omega_vc_bridge.c src/omega_vc_bridge.h src/omega_resolve.c src/omega_resolve.h
	@mkdir -p $(VCB_DIR)
	$(CC) $(RESOLVE_FLAGS) -o $(VCB_DIR)/test_omega_vc_bridge tests/test_omega_vc_bridge.c src/omega_vc_bridge.c src/omega_resolve.c $(VCB_STACK)
	$(VCB_DIR)/test_omega_vc_bridge
	$(CC) $(RESOLVE_FLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o $(VCB_DIR)/test_omega_vc_bridge_asan tests/test_omega_vc_bridge.c src/omega_vc_bridge.c src/omega_resolve.c $(VCB_STACK)
	$(VCB_DIR)/test_omega_vc_bridge_asan
	@set -eu; vmut() { name=$$1; which=$$2; chk=$$3; repl=$$4; \
	  sed "/VC1B:$$name/c\\$$repl" src/$$which.c > $(VCB_DIR)/mut_$$name.c; \
	  if cmp -s src/$$which.c $(VCB_DIR)/mut_$$name.c; then echo "mutant $$name: sed changed nothing"; exit 1; fi; \
	  if [ $$which = omega_vc_bridge ]; then a=$(VCB_DIR)/mut_$$name.c; b=src/omega_resolve.c; else a=src/omega_vc_bridge.c; b=$(VCB_DIR)/mut_$$name.c; fi; \
	  $(CC) $(RESOLVE_FLAGS) -o $(VCB_DIR)/mut_$$name tests/test_omega_vc_bridge.c $$a $$b $(VCB_STACK); \
	  rc=0; $(VCB_DIR)/mut_$$name > $(VCB_DIR)/mut_$$name.out 2>&1 || rc=$$?; \
	  if [ $$rc -eq 0 ] || ! grep -q "^FAIL $$chk\$$" $(VCB_DIR)/mut_$$name.out; then echo "mutant $$name NOT killed by $$chk (exit $$rc)"; tail -5 $(VCB_DIR)/mut_$$name.out; exit 1; fi; \
	  echo "mutant $$name: killed by $$chk (exit $$rc)"; }; \
	  vmut verify                    omega_vc_bridge refuse-corrupted-program-even-with-flags-set '        (void)rep;'; \
	  vmut id                        omega_vc_bridge refuse-forged-program-id-at-admit            '        (void)rid_;'; \
	  vmut cap                       omega_vc_bridge refuse-bridge-record-in-build-domain         '    ou32(&o, 0);'; \
	  vmut build-refuses-selfminted  omega_resolve   refuse-bridge-record-in-build-domain         '    (void)lists_selfminted_cap;'
	@echo "test-vc-bridge: PASS (bridge verifies and recomputes the id, bridge records refused in build, ASan/UBSan clean, 4 mutants killed)"

# FB-1 cut 1: native (no CUDA) matmul entry point for the inference stack.
# libomega_gpu.a = everything the tool links except its main; the Rust FFI crate
# (cut 2) links it. test-gpu-matmul-api runs the host-only refusals; the chip
# sweep is `./build/gpu_matmul_api_test --out receipt.json` through the heavy queue.
.PHONY: libomega_gpu test-gpu-matmul-api
# The two gate-runner objects (omegatool's M17/M18/M19 and World gates) are NOT part of the
# library: nothing in the library or in the Rust FFI crate calls them (checked with nm), and
# they compile the physics checkout path into the object (-DOMEGA_PHYSICS_DIR), which made the
# archive digest depend on where physics was checked out. Without them libomega_gpu.a is
# byte-identical for any PHYSICS_DIR and any omega checkout directory.
GPU_API_OBJS = $(filter-out $(OUT_DIR)/omegatool.o $(OUT_DIR)/omega_blackwell_gates.o $(OUT_DIR)/omega_world_gates.o,$(OBJS))
GPU_API_TEST = $(OUT_DIR)/gpu_matmul_api_test
$(OUT_DIR)/libomega_gpu.a: check-physics-lock $(GPU_API_OBJS)
	ar rcs $@ $(GPU_API_OBJS)
libomega_gpu: $(OUT_DIR)/libomega_gpu.a
$(GPU_API_TEST): tests/gpu_matmul_api_test.c src/omega_gpu_matmul_api.h $(OUT_DIR)/libomega_gpu.a
	$(CC) $(CFLAGS) -o $@ tests/gpu_matmul_api_test.c $(OUT_DIR)/libomega_gpu.a -lpthread -lm
test-gpu-matmul-api: $(GPU_API_TEST)
	./$(GPU_API_TEST) --host-only

# FB-1 cut 4: native rmsnorm / rope / swiglu (+ EX2 and shared-exchange probes).
# test-gpu-elementwise is host-only (refusals, codegen, word fixtures, nvdisasm
# listing); the chip gate is `./build/gpu_elementwise_test --out receipt.json`
# through the heavy queue. -ffp-contract=off keeps the f32 oracle free of FMA.
.PHONY: test-gpu-elementwise
GPU_EW_TEST = $(OUT_DIR)/gpu_elementwise_test
$(GPU_EW_TEST): tests/gpu_elementwise_test.c src/omega_gpu_elementwise_api.h $(OUT_DIR)/libomega_gpu.a
	$(CC) $(CFLAGS) -ffp-contract=off -o $@ tests/gpu_elementwise_test.c $(OUT_DIR)/libomega_gpu.a -lpthread -lm
test-gpu-elementwise: $(GPU_EW_TEST)
	./$(GPU_EW_TEST) --host-only

# omega #308: structured warp reconvergence (BSSY/BSYNC regions). test-gpu-reconv is
# host-only (encoder goldens, region bookkeeping, refusals, nvdisasm listing, every probe
# kernel through the SIMT warp simulator tests/bw_warp_sim.h against the oracle, mutation
# controls); the chip gate is tools/run_gpu_reconv_chip.sh.
.PHONY: test-gpu-reconv
GPU_RECONV_TEST = $(OUT_DIR)/gpu_reconv_test
$(GPU_RECONV_TEST): tests/gpu_reconv_test.c tests/bw_warp_sim.h src/omega_gpu_elementwise_api.h src/omega_bw_reconv.h $(OUT_DIR)/libomega_gpu.a
	$(CC) $(CFLAGS) -Itests -o $@ tests/gpu_reconv_test.c $(OUT_DIR)/libomega_gpu.a -lpthread -lm
test-gpu-reconv: $(GPU_RECONV_TEST)
	./$(GPU_RECONV_TEST) --host-only

# FB-1 cut 5: native gqa_attention (f32 KV) and paged_attention (bf16 KV, + batch).
# test-gpu-attention runs host-only (refusals, codegen, nvdisasm listing) and then the
# whole parity battery through the host IR simulator (--sim, no chip); the chip
# gate is `./build/gpu_attention_test --out receipt.json` through the heavy queue
# (tools/run_gpu_attention_chip.sh). -ffp-contract=off keeps the oracle free of FMA.
.PHONY: test-gpu-attention
GPU_ATTN_TEST = $(OUT_DIR)/gpu_attention_test
$(GPU_ATTN_TEST): tests/gpu_attention_test.c tests/bw_warp_sim.h src/omega_gpu_attention_api.h $(OUT_DIR)/libomega_gpu.a
	$(CC) $(CFLAGS) -ffp-contract=off -Itests -o $@ tests/gpu_attention_test.c $(OUT_DIR)/libomega_gpu.a -lpthread -lm
test-gpu-attention: $(GPU_ATTN_TEST)
	./$(GPU_ATTN_TEST) --host-only
	./$(GPU_ATTN_TEST) --sim
	./$(GPU_ATTN_TEST) --sweep --sim --out $(OUT_DIR)/gpu_attention_sweep_sim.json

# FB-1 cut 4b: fresh-process device-open probe (flake investigation, tools/probe_gpu_open.sh)
$(OUT_DIR)/gpu_session_probe: tests/gpu_session_probe.c src/omega_gpu_session.h $(OUT_DIR)/libomega_gpu.a
	$(CC) $(CFLAGS) -o $@ tests/gpu_session_probe.c $(OUT_DIR)/libomega_gpu.a -lpthread -lm
