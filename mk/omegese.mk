# tools/omegese: Omegese, the AI-writer program language (docs/one_layer/OMEGESE.md). Omegese lowers to the
# omega_emit op list, so the IR, checker2 and the signed receipt chain are unchanged.
# Picked up by `-include mk/*.mk`; not part of `all` or `test`.
#
# omegese        build/omegese/omegese
# test-omegese   the 10 public dev programs: omegese -> omega_emit -> checker2 ACCEPT; refusals with reasons;
#              a loop-guard negative control (checker2 must REJECT); the tokenizer gate when
#              OMEGESE_TOKCOUNT and OMEGESE_TOKENIZER are set (every fixed word is one token)
ifndef OMEGESE_MK
OMEGESE_MK := 1
.PHONY: omegese test-omegese
OMEGESE_DIR = $(OUT_DIR)/omegese

$(OMEGESE_DIR)/omegese: tools/omegese/omegese.c
	@mkdir -p $(OMEGESE_DIR)
	$(CC) -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -o $@ tools/omegese/omegese.c

omegese: $(OMEGESE_DIR)/omegese

test-omegese: omegese omega-emit checker2
	bash tools/omegese/test_omegese.sh $(abspath $(OMEGESE_DIR)/omegese) $(abspath $(OUT_DIR)/omega_emit/omega_emit) $(abspath $(C2_DIR)/checker2)
endif
