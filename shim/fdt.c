/*
 * Just enough flattened-device-tree parsing: /chosen properties (the ABL's
 * initrd location), /memory banks and the continued-splash reserved region.
 * Big-endian, 4-byte aligned tokens.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

/* prefix match: node names carry an optional "@unit-address" suffix */
static int strpre(const char *pre, const char *s)
{
	while (*pre && *pre == *s)
		pre++, s++;
	return !*pre;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len == 4)
					*out = be32(val);
				else if (len == 8)
					*out = (uint64_t)be32(val) << 32 | be32(val + 4);
				else
					return 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

/*
 * Decode every reg[] entry found in the node(s) whose name starts with
 * `nodepre` at `depth` (the root node itself is depth 1, its children are 2)
 * into out[]; matching continues over the whole tree so multiple nodes of
 * the same prefix (several /memory banks) accumulate. Returns the number of
 * entries found (may exceed max: out[] holds only the first max), or -1 on
 * parse error.
 *
 * Qualcomm FDTs keep #address-cells/#size-cells at 2/2 everywhere relevant
 * (root, /reserved-memory), so each reg entry is 4 big-endian cells:
 * <addr_hi addr_lo size_hi size_lo>. Only explicit named properties are
 * decoded (strings would otherwise decode as garbage).
 */
int fdt_node_regs(uintptr_t fdt, int depth, const char *nodepre,
		  const char *prop, struct fdt_reg *out, int max)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return -1;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth_in = 0, in_node = 0, n = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth_in++;
			in_node = depth_in == depth && strpre(nodepre, name);
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			depth_in--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8, val0 = val;	/* val0: next-token base */
			const char *pname = (const char *)(strings + nameoff);
			if (in_node && prop && streq(pname, prop) &&
			    len % 4 == 0 && len >= 16 && (uintptr_t)val % 4 == 0) {
				/* takes every <addr(2c) size(2c)> pair */
				for (uint32_t left = len / 4; left >= 4; left -= 4, val += 16) {
					if (n < max) {
						out[n].base = (uint64_t)be32(val) << 32 | be32(val + 4);
						out[n].size = (uint64_t)be32(val + 8) << 32 | be32(val + 12);
					}
					n++;
				}
			}
			p = align4(val0 + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage: walk over */
			return n;
		}
	}
}

/* /memory* reg entries = the DRAM banks the bootloader filled in. */
int fdt_memory_banks(uintptr_t fdt, struct fdt_reg *banks, int max)
{
	return fdt_node_regs(fdt, 2, "memory", "reg", banks, max);
}

/* The ABL's continued-splash framebuffer reserved region; the compiled node
 * name is splash_region, its label cont_splash_region (both unit-suffixed). */
int fdt_splash_region(uintptr_t fdt, struct fdt_reg *out)
{
	out->base = out->size = 0;
	int r = fdt_node_regs(fdt, 3, "cont_splash", "reg", out, 1);
	if (r <= 0)
		r = fdt_node_regs(fdt, 3, "splash", "reg", out, 1);
	return r > 0 ? 1 : 0;
}
