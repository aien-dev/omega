# AT-1 (interacting clock-system dynamics) integration fragment. Owner: AT-1 Agent 5
# (aien-architecture docs/plans/atemporal/AT1_CHARTER.md section 7). Picked up by the Makefile's
# existing `-include mk/*.mk` line.
#
# Hygiene rules, checked by research/atemporal/at1/integration/run.sh (controls C14 to C16):
#   - every variable and target defined here starts with AT1_ or at1;
#   - nothing is added to SRCS, all, test, clean or any existing target or variable;
#   - nothing here is evaluated at parse time beyond plain assignments (no $(shell), no includes);
#   - GNU Make 3.81 constructs only (ifndef, :=, ?=, .PHONY), so it also parses on macOS.
#
#   make at1-check   clean-checkout build of the AT-1 oracle, engine and evaluator, then every public
#                    evaluator case through oracle -> engine (splice) -> evaluator, a second run for
#                    repeatability, the splice, mutant, isolation and make-hygiene controls.
#                    Single-threaded, deterministic, CPU only, no network. Output: $(AT1_OUT)/check.
#                    Recording evidence is a separate, explicit act: `sh $(AT1_INTEG)/run.sh --evidence`.
#   make at1-clean   remove $(AT1_OUT), only when its last path component is at1.
ifndef AT1_MK
AT1_MK := 1
AT1_DIR := research/atemporal/at1
AT1_INTEG := $(AT1_DIR)/integration
AT1_OUT ?= $(OUT_DIR)/at1

.PHONY: at1-check at1-clean

at1-check:
	@sh '$(AT1_INTEG)/run.sh' --out '$(AT1_OUT)/check'

at1-clean:
	@case '$(AT1_OUT)' in at1|*/at1) rm -rf -- '$(AT1_OUT)';; *) echo "at1-clean: AT1_OUT='$(AT1_OUT)' does not end in /at1; not removed" >&2; exit 1;; esac
endif
