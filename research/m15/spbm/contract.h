/* SPDX-License-Identifier: GPL-2.0-only */
/* Pinned read-only contract from this machine's MTEL _DSM, 2026-09-28.
 * Excludes every CLEAR_OVERFLOW and control register. No fallback map.
 */
#ifndef AIEN_SPBM_CONTRACT_H
#define AIEN_SPBM_CONTRACT_H
#define SPBM_PHYS 0x1c238000ULL
#define SPBM_SIZE 0x1000
#define SPBM_N_ENERGY 5
#define SPBM_N_POWER 5
#define SPBM_OVERFLOW_FIRST SPBM_N_ENERGY
#define SPBM_POWER_FIRST (SPBM_N_ENERGY * 2)
struct spbm_register { const char *key; unsigned int offset; };
static const struct spbm_register spbm_registers[] = {
	{ "SPBM_PKG_ENERGY_VALUE_ACCUMULATE_OFFSET", 0x344 },
	{ "SPBM_CPU_E_ENERGY_VALUE_ACCUMULATE_OFFSET", 0x350 },
	{ "SPBM_CPU_P_ENERGY_VALUE_ACCUMULATE_OFFSET", 0x35c },
	{ "SPBM_GPC_ENERGY_VALUE_ACCUMULATE_OFFSET", 0x368 },
	{ "SPBM_GPM_ENERGY_VALUE_ACCUMULATE_OFFSET", 0x374 },
	{ "SPBM_PKG_ENERGY_VALUE_OVERFLOW_OFFSET", 0x348 },
	{ "SPBM_CPU_E_ENERGY_VALUE_OVERFLOW_OFFSET", 0x354 },
	{ "SPBM_CPU_P_ENERGY_VALUE_OVERFLOW_OFFSET", 0x360 },
	{ "SPBM_GPC_ENERGY_VALUE_OVERFLOW_OFFSET", 0x36c },
	{ "SPBM_GPM_ENERGY_VALUE_OVERFLOW_OFFSET", 0x378 },
	{ "SPBM_TE_SYS_TOTAL_TELEMETRY_OFFSET", 0x300 },
	{ "SPBM_TE_SOC_PKG_TELEMETRY_OFFSET", 0x304 },
	{ "SPBM_TE_CPU_E_TELEMETRY_OFFSET", 0x310 },
	{ "SPBM_TE_CPU_P_TELEMETRY_OFFSET", 0x30c },
	{ "SPBM_TE_TOTAL_GPU_OUT_OFFSET", 0x324 },
};
#define SPBM_N_REGS (sizeof(spbm_registers) / sizeof(spbm_registers[0]))
#define SPBM_ALL_REGS ((1UL << SPBM_N_REGS) - 1)
/* The includer supplies strlen/memcmp. key need not be NUL-terminated. */
static inline int spbm_contract_add(unsigned long *seen, const char *key,
				   unsigned int length, unsigned long long offset)
{
	unsigned int i;
	for (i = 0; i < SPBM_N_REGS; ++i) {
		if (strlen(spbm_registers[i].key) != length ||
		    memcmp(key, spbm_registers[i].key, length))
			continue;
		if ((*seen & (1UL << i)) || offset != spbm_registers[i].offset ||
		    offset > SPBM_SIZE - 4 || (offset & 3))
			return -1;
		*seen |= 1UL << i;
		return 1;
	}
	return 0;
}
#endif
