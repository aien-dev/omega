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
	-I$(PHYSICS_DIR)/third_party/nvidia-open-580.173.02/src/nvidia/arch/nvalloc/unix/include \
	-DOMEGA_PHYSICS_DIR=\"$(PHYSICS_DIR)\"

SRCS = src/sha256.c src/omega_canonical.c src/omega_validate.c src/omega_core.c src/omega_codec.c \
	src/aarch64_encoder.c src/aarch64_decoder.c src/omega_realize.c src/omega_exec.c \
	src/omega_self_host.c src/omega_verify.c src/omega_program.c src/omega_synthesis.c \
	src/omega_library.c src/omega_discovery.c src/omega_machine.c src/omega_realize_synth.c \
	src/omega_matvec.c src/omega_accelerator.c src/omega_accelerator_world.c \
	src/omega_vector.c src/omega_blackwell_encoder.c src/omega_blackwell_qmd.c \
	src/omega_blackwell_realize.c src/omega_blackwell_submit.c src/omega_blackwell_gates.c src/omega_blackwell_matmul.c src/omega_blackwell_codegen.c src/omega_world_gates.c \
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

.PHONY: all clean check-physics-lock crumbline-learner test-crumbline test-m19 test test-m5 test-m6 test-m7 test-m8 test-m9 test-m10 test-m11 test-m12 test-m13 test-m14 test-m15 test-m17 test-r3 test-action-graph test-state-projection test-capability-query test-capability-graph test-skillroute-compose test-semantic-comm test-cognitive-routing test-sem-incremental test-branch-reuse test-jspace-prod test-plan-reuse test-cortex

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

# Host-only tests of the Gate 14 combiner (tools/gate14_combine.sh). No GPU.
.PHONY: test-gate14-combine
test-gate14-combine:
	tools/test_gate14_combine.sh

# Host-only self-test of the shared chip-run module (tools/chip_run.sh): every
# refusal path with fake binaries. No GPU.
.PHONY: test-chip-run
test-chip-run:
	tests/test_chip_run.sh

# Host-only check of the unwritten-trap manifest and its wrapper. No GPU.
.PHONY: test-chip-run-trap-manifest
test-chip-run-trap-manifest:
	tests/test_chip_run_trap_manifest.sh

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
# Default: the authority pinned by aienos.lock, extracted with `git archive` from
# AIENOS_LOCK_REPO (a clone that has the commit) into $(OUT_DIR). An override must point
# at a tree with native/capability; the build stops if it does not (the old default
# ../aienos-r9 silently had none on the Spark).
AIENOS_LOCK_REPO ?= ../aienos-argus-cap
AIENOS_LOCK = $(shell head -n 1 aienos.lock)
AIENOS_R7_DEFAULT = $(OUT_DIR)/aienos-authority/$(shell echo $(AIENOS_LOCK) | cut -c1-7)
AIENOS_R7_DIR ?= $(AIENOS_R7_DEFAULT)
AIENOS_CAP_LIB ?= $(AIENOS_R7_DIR)/native/capability/out/libaienos_capability.a
RX_R7_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_r7_native.c
RX_R7_TEST = $(OUT_DIR)/rx_r7_native_test

$(AIENOS_CAP_LIB):
	@if [ "$(AIENOS_R7_DIR)" = "$(AIENOS_R7_DEFAULT)" ] && [ ! -d "$(AIENOS_R7_DIR)/native/capability" ]; then \
		test -n "$(AIENOS_LOCK)" || { echo "aienos.lock is empty"; exit 1; }; \
		git -C "$(AIENOS_LOCK_REPO)" cat-file -e "$(AIENOS_LOCK)^{commit}" 2>/dev/null || { \
			echo "error: AIENOS_LOCK_REPO=$(AIENOS_LOCK_REPO) is not an aienos clone with commit $(AIENOS_LOCK) (aienos.lock)."; \
			echo "  pass AIENOS_LOCK_REPO=<path to an aienos clone that has it>, e.g. make AIENOS_LOCK_REPO=$$HOME/workspace/aienos-argus-cap <target>,"; \
			echo "  or AIENOS_R7_DIR=<an aienos tree at that commit>."; exit 1; }; \
		mkdir -p "$(AIENOS_R7_DIR)" && \
		git -C $(AIENOS_LOCK_REPO) archive $(AIENOS_LOCK) native/capability | tar -x -C "$(AIENOS_R7_DIR)"; \
	fi
	@test -f "$(AIENOS_R7_DIR)/native/capability/Makefile" || { \
		echo "AIENOS_R7_DIR=$(AIENOS_R7_DIR) has no native/capability:"; \
		echo "  set AIENOS_R7_DIR to an aienos tree at aienos.lock, or AIENOS_LOCK_REPO to a clone with it"; exit 1; }
	$(MAKE) -C $(AIENOS_R7_DIR)/native/capability

# OMEGA_EFFECT_CAP64 (spec/effect-cap64-migration.md): effect objects carry the
# full 64-bit AIENOS capability generation. Physics-free: the Omega core, the
# Visor effect-request adapter, and the pinned AIENOS header + library
# (aienos.lock) used directly. Prints OMEGA_EFFECT_CAP64_{ROUNDTRIP,IDENTITY,
# STALE_REJECT,AUTHORITY}_PASS gate lines.
EFFECT_CAP64_CAP_LIB ?= $(AIENOS_CAP_LIB)
EFFECT_CAP64_CAP_INC ?= $(patsubst %/,%,$(dir $(EFFECT_CAP64_CAP_LIB)))/..
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
RX_R14_SRCS = $(filter-out tests/runtime/rx_r13_living.c,$(RX_R13_SRCS)) \
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
RX_R15_RIG_SRCS = $(filter-out tests/runtime/rx_r13_living.c,$(RX_R13_SRCS)) \
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
	sh tests/r16_negative/mutate.sh "$(CC)" "$(CFLAGS)" "$(AIENOS_R7_DIR)" \
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
RX_COMPOSE_GPU_SRCS = src/omega_blackwell_submit.c src/omega_blackwell_matmul.c \
	src/omega_blackwell_codegen.c src/omega_blackwell_encoder.c src/omega_blackwell_qmd.c \
	src/omega_blackwell_realize.c src/omega_vector.c src/omega_validate.c \
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
