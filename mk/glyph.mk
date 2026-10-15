# tools/glyph: Glyph, the AI-writer program language (docs/one_layer/GLYPH.md). Glyph lowers to the
# omega_emit op list, so the IR, checker2 and the signed receipt chain are unchanged.
# Picked up by `-include mk/*.mk`; not part of `all` or `test`.
#
# glyph        build/glyph/glyph
# test-glyph   the 10 public dev programs: glyph -> omega_emit -> checker2 ACCEPT; refusals with reasons;
#              a loop-guard negative control (checker2 must REJECT); the tokenizer gate when
#              GLYPH_TOKCOUNT and GLYPH_TOKENIZER are set (every fixed word is one token)
ifndef GLYPH_MK
GLYPH_MK := 1
.PHONY: glyph test-glyph
GLYPH_DIR = $(OUT_DIR)/glyph

$(GLYPH_DIR)/glyph: tools/glyph/glyph.c
	@mkdir -p $(GLYPH_DIR)
	$(CC) -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -o $@ tools/glyph/glyph.c

glyph: $(GLYPH_DIR)/glyph

test-glyph: glyph omega-emit checker2
	bash tools/glyph/test_glyph.sh $(abspath $(GLYPH_DIR)/glyph) $(abspath $(OUT_DIR)/omega_emit/omega_emit) $(abspath $(C2_DIR)/checker2)
endif
