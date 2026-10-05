# ESTIMATION v6 pre-freeze development tools (docs/estimation/protocols/est-v6.md section 9,
# appendix docs/estimation/protocols/est-v6-appendix-prefreeze.md). Development tools only: not the v6
# tool, no protocol identity, no binding mode. Picked up by `-include mk/*.mk`; not part of `all` or `test`.
#
# est6dev            G1 PIT / miss-rate / block-bootstrap analysis, reduced G1S check, schedule and seed balance
# est6opchar         operating-characteristics simulation (reads no data file)
# test-est6dev       tests for both (D1 development data and synthetic only; held-out refusals)
# est6-opchar-table  prints the appendix operating-characteristics table (about 3 minutes)
ifndef ESTIMATION_V6DEV_MK
ESTIMATION_V6DEV_MK := 1
include mk/estimation_v4.mk
.PHONY: est6dev est6opchar test-est6dev est6-opchar-table
ESTV6D_DIR = $(OUT_DIR)/estimation-v6dev
ESTV6D_SRCS = tools/estimation/est6dev.c tools/estimation/est3c_common.c tools/estimation/est_replay.c \
	src/estimation/est_v4.c src/estimation/est_pred.c src/estimation/est_mix.c $(EST_KF_SRCS)
ESTV6D_HDRS = tools/estimation/est3c_common.h tools/estimation/est_replay.h $(ESTV4_HDRS)
$(ESTV6D_DIR)/est6dev: $(ESTV6D_SRCS) $(ESTV6D_HDRS)
	@mkdir -p $(ESTV6D_DIR)
	$(CC) $(EST_CFLAGS) -Itools/estimation -o $@ $(ESTV6D_SRCS) -lm
$(ESTV6D_DIR)/est6opchar: tools/estimation/est6opchar.c
	@mkdir -p $(ESTV6D_DIR)
	$(CC) $(EST_CFLAGS) -o $@ $< -lm
est6dev: $(ESTV6D_DIR)/est6dev
est6opchar: $(ESTV6D_DIR)/est6opchar
test-est6dev: $(ESTV6D_DIR)/est6dev $(ESTV6D_DIR)/est6opchar
	sh tools/estimation/test_est6dev.sh $(ESTV6D_DIR)/est6dev $(ESTV6D_DIR)/est6opchar
est6-opchar-table: $(ESTV6D_DIR)/est6opchar
	sh tools/estimation/est6opchar_table.sh $(ESTV6D_DIR)/est6opchar 4000
endif
