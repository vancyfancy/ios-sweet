#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

struct fdt_reg {
	uint64_t base, size;
};

/* Decode the reg[] entry of every node named nodepre* at depth (root=1) into
 * out[]; returns entries found (may exceed max), -1 on parse error. Only the
 * named property supplies entries (strings would decode as garbage). */
int fdt_node_regs(uintptr_t fdt, int depth, const char *nodepre,
		  const char *prop, struct fdt_reg *out, int max);

/* /memory* reg entries = the DRAM banks the bootloader filled in. */
int fdt_memory_banks(uintptr_t fdt, struct fdt_reg *banks, int max);

/* reg of the reserved-memory child named or labelled "cont_splash_region"
 * (the ABL's continued-splash framebuffer). Returns 0 if absent. */
int fdt_splash_region(uintptr_t fdt, struct fdt_reg *out);
