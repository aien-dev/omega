# AT-0 (Clock-Free Universe) integration fragment. Owner: AT-0 Agent 5 (aien-architecture
# docs/plans/atemporal/AT0_CHARTER.md section 3 item 2). Picked up by the Makefile's existing
# `-include mk/*.mk` line.
#
# Hygiene rules, checked by research/atemporal/at0/integration/run.sh (controls C11 to C13):
#   - every variable and target defined here starts with AT0_ or at0;
#   - nothing is added to SRCS, all, test, clean or any existing target or variable;
#   - nothing here is evaluated at parse time beyond plain assignments (no $(shell), no includes).
#
#   make at0-check   clean-checkout build of the AT-0 model, oracle, evaluator and assembler, then
#                    every evaluator case through model -> oracle -> assembler -> evaluator, a
#                    second run for repeatability, the isolation and make-hygiene controls.
#                    Single-threaded, deterministic, CPU only, no network. Output: $(AT0_OUT)/check.
#                    Recording evidence is a separate, explicit act: `sh $(AT0_INTEG)/run.sh --evidence`.
#   make at0-clean   remove $(AT0_OUT).
ifndef AT0_MK
AT0_MK := 1
AT0_DIR := research/atemporal/at0
AT0_INTEG := $(AT0_DIR)/integration
AT0_OUT ?= $(OUT_DIR)/at0

.PHONY: at0-check at0-clean

at0-check:
	@sh $(AT0_INTEG)/run.sh --out $(AT0_OUT)/check

at0-clean:
	@test -n '$(AT0_OUT)' && test '$(AT0_OUT)' != / && rm -rf -- '$(AT0_OUT)'
endif
