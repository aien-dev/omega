/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "contract.h"
int main(void)
{
	unsigned long seen = 0;
	unsigned int i;
	const char *key = spbm_registers[0].key;
	for (i = 0; i < SPBM_N_REGS; ++i)
		assert(spbm_contract_add(&seen, spbm_registers[i].key,
			strlen(spbm_registers[i].key), spbm_registers[i].offset) == 1);
	assert(seen == SPBM_ALL_REGS);
	assert(spbm_contract_add(&seen, key, strlen(key), 0x344) == -1);
	seen = 0;
	assert(spbm_contract_add(&seen, key, strlen(key), 0x348) == -1);
	assert(spbm_contract_add(&seen, key, strlen(key), 0x345) == -1);
	assert(spbm_contract_add(&seen, key, strlen(key), SPBM_SIZE) == -1);
	assert(spbm_contract_add(&seen, key, strlen(key), 0x100000344ULL) == -1);
	assert(spbm_contract_add(&seen, "SPBM_PKG_ENERGY_CLEAR_OVERFLOW_OFFSET",
		strlen("SPBM_PKG_ENERGY_CLEAR_OVERFLOW_OFFSET"), 0x34c) == 0);
	assert(spbm_contract_add(&seen, key, strlen(key) - 1, 0x344) == 0);
	assert(seen == 0);
	puts("SPBM contract: valid map accepted; duplicate/moved/unaligned/out-of-range rejected; control key ignored");
	return 0;
}
