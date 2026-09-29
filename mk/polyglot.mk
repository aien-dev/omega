# POLYGLOT-0 build and test targets. spec/polyglot-0.md
# Owner: lane F (verifier/bench). Other lanes ask F to add their sources.
#
# ---------------------------------------------------------------------------
# LANE HOOK CONTRACT (B1 asm, B2 encoder, B3 Mojo): put a file mk/polyglot-<lane>.mk
# next to this one. It is picked up automatically. In it, only APPEND to these:
#
#   POLYGLOT_LANE_SRCS     += src/polyglot/omx_asm.c src/polyglot/asm/x.S
#       .c and .S files. Lane F compiles each one twice (plain: $(PG_LANE_CFLAGS);
#       sanitizer: + $(PG_SAN)) into $(OUT_DIR)/polyglot/{lane,asan/lane}/<path>.o,
#       links them into verify/bench, and records per object in the bench manifest:
#       SHA-256 (build digest), text-section bytes, compile time (min of 5).
#       The source path you give in omx_candidate.source must be one of these
#       paths so the bench can find the object of each candidate.
#   POLYGLOT_LANE_OBJS     += path/to/prebuilt.o   (objects your own rules build,
#       e.g. Mojo; linked into both plain and sanitizer binaries as they are)
#   POLYGLOT_LANE_MANIFEST += <source-path>=<object-path>   (for prebuilt objects:
#       lets the bench digest and size them; compile time is then recorded as
#       null with a reason)
#   POLYGLOT_LANE_LDLIBS   += -lfoo      (extra link libraries)
#   POLYGLOT_LANE_DEPS     += some-target (built before any polyglot binary)
#   POLYGLOT_LANE_CFLAGS   += -Dsomething (added when compiling lane sources)
#
# A lane's C file defines the strong table, which replaces the weak empty one
# in src/polyglot/omx_lang.c:
#   const omx_candidate omx_lane_asm[] = {...};
#   const size_t omx_lane_asm_count = sizeof omx_lane_asm / sizeof omx_lane_asm[0];
# (omx_lane_encoder / omx_lane_mojo likewise). Guard your mk file with
# `ifndef POLYGLOT_<LANE>_MK` because the top-level Makefile may include it twice.
# Lists here are de-duplicated with $(sort), so a double += is harmless.
#
# Targets:
#   test-polyglot       verifier, plain and ASan+UBSan builds (correctness only),
#                       then bench_polyglot --selftest (harness checks, no timing)
#   bench-polyglot      timed benchmark (lane F / lead only; refuses when
#                       ~/workspace/.spark-quiet exists); receipts to
#                       $(POLYGLOT_EVIDENCE). Args via POLYGLOT_BENCH_ARGS.
#   bench-polyglot-smoke  N=3, sparsity 0, receipts to $(OUT_DIR)/polyglot/smoke
#                       (never committed)
#   polyglot-explain    explainer over $(POLYGLOT_RECEIPTS), run twice and cmp'd
#                       (POLYGLOT_RUN=<run_id> picks the run when several are present)
# ---------------------------------------------------------------------------
ifndef POLYGLOT_MK_LOADED
POLYGLOT_MK_LOADED := 1

.PHONY: test-polyglot bench-polyglot bench-polyglot-smoke polyglot-explain
# Lane mk files may be read before OR after this one (make sorts mk/*.mk by
# locale), so every lane-dependent prerequisite is written $$(...) and expanded
# in the second phase, after all makefiles are read.
.SECONDEXPANSION:

PG_OUT := $(OUT_DIR)/polyglot
POLYGLOT_EVIDENCE ?= evidence/POLYGLOT
POLYGLOT_RECEIPTS ?= $(POLYGLOT_EVIDENCE)
POLYGLOT_BENCH_ARGS ?=
POLYGLOT_RUN ?=
PG_GCC_VER := $(shell $(CC) -dumpfullversion 2>/dev/null)

PG_ARCH := -march=armv8.6-a+dotprod+i8mm+sve
# MA-3's own flags (Makefile OMA_RZ_CFLAGS), unchanged.
PG_O2_FLAGS := -std=c11 -O2 $(PG_ARCH)
# Second C flavor (spec section 2: -O3 -mcpu=native). gcc 13.3 resolves
# -mcpu=native on this X925/A725 machine to generic armv8-a (no dotprod/i8mm),
# so MA-3's NEON intrinsics do not compile with -mcpu=native alone; the MA-3
# -march is kept and -mcpu=native adds only (generic) tuning. Recorded in receipts.
PG_O3_FLAGS := -std=c11 -O3 $(PG_ARCH) -mcpu=native
PG_WARN := -Wall -Wextra -Werror -pedantic
PG_SAN := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
PG_HCFLAGS := -std=gnu11 -Wall -Wextra -Werror -O2 $(PG_ARCH) -Isrc
PG_LANE_CFLAGS = -Wall -Wextra -Werror -O2 $(PG_ARCH) -Isrc $(POLYGLOT_LANE_CFLAGS)

PG_MA_SRCS := src/algebra/realize_common.c src/algebra/realize_binary.c \
	src/algebra/realize_bitplane.c src/algebra/realize_sparse.c \
	src/algebra/realize_rns.c src/algebra/realize_dense.c
PG_O3_SRCS := src/algebra/realize_binary.c src/algebra/realize_bitplane.c
PG_O3_RENAME := -Doma_rz_r1_plain=omx_o3_r1_plain -Doma_rz_r1_sdot=omx_o3_r1_sdot \
	-Doma_rz_r1_sdot_il=omx_o3_r1_sdot_il -Doma_rz_r1_smmla=omx_o3_r1_smmla \
	-Doma_rz_r2_bitplane=omx_o3_r2_bitplane -Doma_rz_r2b_lut=omx_o3_r2b_lut \
	-Doma_rz_r2c_crumb=omx_o3_r2c_crumb
PG_HDRS := src/algebra/realize_common.h src/polyglot/omx_lang.h src/polyglot/omx_bench.h src/sha256.h
PG_TOOLCHAIN_DEFS = -DOMX_C_O2_TOOLCHAIN='"gcc $(PG_GCC_VER) $(PG_O2_FLAGS)"' \
	-DOMX_C_O3_TOOLCHAIN='"gcc $(PG_GCC_VER) $(PG_O3_FLAGS)"'
PG_COMMON_SRCS := src/polyglot/omx_lang.c src/polyglot/omx_bench.c src/sha256.c

# Lanes that predate this contract are picked up by their own variables:
# B1 asm (PGA_ASM + omx_asm.c), B3 Mojo (OMX_MOJO_SRCS, OMX_MOJO_OBJS).
PG_LANE_SRCS = $(sort $(POLYGLOT_LANE_SRCS) $(if $(PGA_ASM),src/polyglot/omx_asm.c $(PGA_ASM)) $(OMX_MOJO_SRCS))
PG_LANE_PREBUILT = $(sort $(POLYGLOT_LANE_OBJS) $(OMX_MOJO_OBJS))
PG_LANE_OBJS_PLAIN = $(addprefix $(PG_OUT)/lane/,$(addsuffix .o,$(PG_LANE_SRCS)))
PG_LANE_OBJS_ASAN = $(addprefix $(PG_OUT)/asan/lane/,$(addsuffix .o,$(PG_LANE_SRCS)))
PG_O2_OBJS := $(patsubst src/algebra/%.c,$(PG_OUT)/o2/%.o,$(PG_MA_SRCS))
PG_O3_OBJS := $(patsubst src/algebra/%.c,$(PG_OUT)/o3/%.o,$(PG_O3_SRCS))
PG_O2_OBJS_ASAN := $(patsubst src/algebra/%.c,$(PG_OUT)/asan/o2/%.o,$(PG_MA_SRCS))
PG_O3_OBJS_ASAN := $(patsubst src/algebra/%.c,$(PG_OUT)/asan/o3/%.o,$(PG_O3_SRCS))
PG_PLAIN_OBJS = $(PG_O2_OBJS) $(PG_O3_OBJS) $(PG_LANE_OBJS_PLAIN) $(PG_LANE_PREBUILT)
PG_ASAN_OBJS = $(PG_O2_OBJS_ASAN) $(PG_O3_OBJS_ASAN) $(PG_LANE_OBJS_ASAN) $(PG_LANE_PREBUILT)
PG_LDLIBS = $(sort $(POLYGLOT_LANE_LDLIBS)) -lm

PG_VERIFY := $(PG_OUT)/verify_polyglot
PG_VERIFY_ASAN := $(PG_OUT)/verify_polyglot_asan
PG_BENCH := $(PG_OUT)/bench_polyglot
PG_EXPLAIN := $(PG_OUT)/polyglot_explain
PG_MANIFEST := $(PG_OUT)/manifest.tsv

# ---- objects ----
$(PG_OUT)/o2/%.o: src/algebra/%.c src/algebra/realize_common.h
	@mkdir -p $(dir $@)
	$(CC) $(PG_WARN) $(PG_O2_FLAGS) -Isrc -c -o $@ $<
$(PG_OUT)/o3/%.o: src/algebra/%.c src/algebra/realize_common.h
	@mkdir -p $(dir $@)
	$(CC) $(PG_WARN) $(PG_O3_FLAGS) $(PG_O3_RENAME) -Isrc -c -o $@ $<
$(PG_OUT)/asan/o2/%.o: src/algebra/%.c src/algebra/realize_common.h
	@mkdir -p $(dir $@)
	$(CC) $(PG_WARN) $(PG_O2_FLAGS) $(PG_SAN) -Isrc -c -o $@ $<
$(PG_OUT)/asan/o3/%.o: src/algebra/%.c src/algebra/realize_common.h
	@mkdir -p $(dir $@)
	$(CC) $(PG_WARN) $(PG_O3_FLAGS) $(PG_SAN) $(PG_O3_RENAME) -Isrc -c -o $@ $<
$(PG_OUT)/lane/%.o: % $(PG_HDRS) | $$(POLYGLOT_LANE_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(PG_LANE_CFLAGS) -c -o $@ $<
$(PG_OUT)/asan/lane/%.o: % $(PG_HDRS) | $$(POLYGLOT_LANE_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(PG_LANE_CFLAGS) $(PG_SAN) -c -o $@ $<

# ---- binaries ----
$(PG_VERIFY): tests/polyglot/verify_polyglot.c tests/polyglot/cs_call.S $(PG_COMMON_SRCS) $(PG_HDRS) $$(PG_PLAIN_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(PG_HCFLAGS) $(PG_TOOLCHAIN_DEFS) -DOMX_VERIFY_BUILD='"plain"' -o $@ \
		tests/polyglot/verify_polyglot.c tests/polyglot/cs_call.S $(PG_COMMON_SRCS) $(PG_PLAIN_OBJS) $(PG_LDLIBS)
$(PG_VERIFY_ASAN): tests/polyglot/verify_polyglot.c tests/polyglot/cs_call.S $(PG_COMMON_SRCS) $(PG_HDRS) $$(PG_ASAN_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(PG_HCFLAGS) $(PG_SAN) $(PG_TOOLCHAIN_DEFS) -DOMX_VERIFY_BUILD='"asan+ubsan"' -o $@ \
		tests/polyglot/verify_polyglot.c tests/polyglot/cs_call.S $(PG_COMMON_SRCS) $(PG_ASAN_OBJS) $(PG_LDLIBS)
$(PG_BENCH): tests/polyglot/bench_polyglot.c $(PG_COMMON_SRCS) $(PG_HDRS) $$(PG_PLAIN_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(PG_HCFLAGS) $(PG_TOOLCHAIN_DEFS) -o $@ \
		tests/polyglot/bench_polyglot.c $(PG_COMMON_SRCS) $(PG_PLAIN_OBJS) $(PG_LDLIBS)
$(PG_EXPLAIN): tests/polyglot/polyglot_explain.c src/polyglot/omx_bench.c src/sha256.c $(PG_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(PG_HCFLAGS) -o $@ tests/polyglot/polyglot_explain.c src/polyglot/omx_bench.c src/sha256.c -lm

# ---- bench manifest: source <TAB> flavor <TAB> object <TAB> compile command ----
# (@OUT@ in the command is replaced by a scratch path when the bench times it)
$(PG_MANIFEST): $$(PG_PLAIN_OBJS) $$(MAKEFILE_LIST)
	@mkdir -p $(dir $@)
	@: > $@
	@$(foreach s,$(PG_MA_SRCS),printf '%s\tO2\t%s\t%s\n' '$(s)' '$(PG_OUT)/o2/$(notdir $(s:.c=.o))' \
		'$(CC) $(PG_WARN) $(PG_O2_FLAGS) -Isrc -c -o @OUT@ $(s)' >> $@;)
	@$(foreach s,$(PG_O3_SRCS),printf '%s\tO3\t%s\t%s\n' '$(s)' '$(PG_OUT)/o3/$(notdir $(s:.c=.o))' \
		'$(CC) $(PG_WARN) $(PG_O3_FLAGS) $(PG_O3_RENAME) -Isrc -c -o @OUT@ $(s)' >> $@;)
	@$(foreach s,$(PG_LANE_SRCS),printf '%s\tlane\t%s\t%s\n' '$(s)' '$(PG_OUT)/lane/$(s).o' \
		'$(CC) $(PG_LANE_CFLAGS) -c -o @OUT@ $(s)' >> $@;)
	@$(foreach e,$(sort $(POLYGLOT_LANE_MANIFEST)),printf '%s\tlane\t%s\t-\n' \
		'$(word 1,$(subst =, ,$(e)))' '$(word 2,$(subst =, ,$(e)))' >> $@;)
	@$(if $(OMX_MOJO_OBJS),printf '%s\tlane\t%s\t%s\n' '$(OMX_MOJO_SRC)' '$(OMX_MOJO_KOBJ)' \
		'$(MOJO) build --emit object -o @OUT@.o $(OMX_MOJO_SRC)' >> $@)

# ---- targets ----
test-polyglot: $(PG_VERIFY) $(PG_VERIFY_ASAN) $(PG_BENCH)
	./$(PG_VERIFY)
	./$(PG_VERIFY_ASAN)
	./$(PG_BENCH) --selftest

PG_BENCH_ENV = POLYGLOT_COMMIT=$$(git rev-parse HEAD) \
	POLYGLOT_DIRTY=$$(git status --porcelain -- src tests mk Makefile spec | grep -c .) \
	POLYGLOT_BENCH_SHA=$$(sha256sum $(PG_BENCH) | cut -c1-64) \
	POLYGLOT_CFLAGS_O2='$(PG_O2_FLAGS)' POLYGLOT_CFLAGS_O3='$(PG_O3_FLAGS)' \
	POLYGLOT_CFLAGS_LANE='$(PG_LANE_CFLAGS)'

bench-polyglot: $(PG_BENCH) $(PG_MANIFEST)
	@mkdir -p $(POLYGLOT_EVIDENCE)
	$(PG_BENCH_ENV) ./$(PG_BENCH) --manifest $(PG_MANIFEST) --spec spec/polyglot-0.md \
		--out $(POLYGLOT_EVIDENCE) $(POLYGLOT_BENCH_ARGS)

bench-polyglot-smoke: $(PG_BENCH) $(PG_MANIFEST)
	@mkdir -p $(PG_OUT)/smoke
	$(PG_BENCH_ENV) ./$(PG_BENCH) --manifest $(PG_MANIFEST) --spec spec/polyglot-0.md \
		--out $(PG_OUT)/smoke --smoke --samples 3 --sparsity 0 $(POLYGLOT_BENCH_ARGS)

polyglot-explain: $(PG_EXPLAIN)
	./$(PG_EXPLAIN) --spec spec/polyglot-0.md --receipts $(POLYGLOT_RECEIPTS) $(if $(POLYGLOT_RUN),--run $(POLYGLOT_RUN)) > $(PG_OUT)/polyglot_explain.1.txt
	./$(PG_EXPLAIN) --spec spec/polyglot-0.md --receipts $(POLYGLOT_RECEIPTS) $(if $(POLYGLOT_RUN),--run $(POLYGLOT_RUN)) > $(PG_OUT)/polyglot_explain.2.txt
	cmp $(PG_OUT)/polyglot_explain.1.txt $(PG_OUT)/polyglot_explain.2.txt
	@cat $(PG_OUT)/polyglot_explain.1.txt
	@echo "polyglot-explain: two runs byte-identical ($(PG_OUT)/polyglot_explain.1.txt)"

endif # POLYGLOT_MK_LOADED
