obj-m += aien_spbm_readonly.o

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean tools check
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

tools: sample load reduce contract_test
sample: sample.c
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror $< -o $@ -ldl
load: load.c
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror $< -o $@ -pthread
reduce: reduce.c
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror $< -o $@
contract_test: contract_test.c contract.h
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined $< -o $@
check: contract_test
	./contract_test
