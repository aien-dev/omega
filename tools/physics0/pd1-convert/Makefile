# PD-1 evidence conversion tool (D7). Host only, no Python, no GPU.
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror
OUT ?= $(CURDIR)/out
all: pd1_convert
pd1_convert: pd1_convert.c
	$(CC) $(CFLAGS) -o $@ $<
# run: extract -> convert -> verify -> leak check. Never overwrites an existing stream.
run: pd1_convert
	mkdir -p $(OUT)
	sh ./extract.sh > $(OUT)/pd1-input.tsv
	./pd1_convert $(OUT)/pd1-records.bin < $(OUT)/pd1-input.tsv > $(OUT)/pd1-projection.tsv 2> $(OUT)/pd1-convert.log
	./pd1_convert --verify $(OUT)/pd1-records.bin
	sh ./leakcheck.sh $(OUT)/pd1-projection.tsv
	@awk -F'\t' 'NR>1{n[$$2]++; if($$9!=-1)lat[$$2]++; if($$10==1)f[$$2]++} END{for(s in n) printf "src=%s rows=%d with_latency=%d fail=%d\n", s, n[s], lat[s], f[s]}' $(OUT)/pd1-projection.tsv | sort
clean:
	rm -f pd1_convert
.PHONY: all run clean
