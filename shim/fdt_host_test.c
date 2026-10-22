/*
 * Host-side test for the shim's FDT reader and ADT patcher (the shapes are
 * tools/mkadt.py's; the FDT is what the ABL hands over). Runs on macOS:
 *   cc -arch arm64 shim/fdt_host_test.c -o /tmp/fdt_host_test && /tmp/fdt_host_test
 * fdt.c and loader.c are included to reach their static helpers; console
 * output is stubbed into stdout.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"

volatile uint32_t *con_fb = 0;

void con_clear(uint32_t bg) { (void)bg; }
void con_skip(void) {}
void con_str(const char *label, const char *s) { printf("[STR] %s: %s\n", label, s); }
void con_hex(const char *label, uint64_t v) { printf("[HEX] %s: 0x%llx\n", label, (unsigned long long)v); }
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	printf("[HEX2] %s: %s=%#x %s=%#x\n", label, a, va, b, vb);
}

#include "fdt.c"
#include "loader.c"

static int fails;
#define CHECK(cond, name)					\
	do {							\
		if (cond)					\
			printf("PASS %s\n", name);		\
		else {						\
			printf("FAIL %s (line %d)\n", name, __LINE__);	\
			fails++;				\
		}						\
	} while (0)

/* ================= synthetic FDT (big-endian, 4-byte aligned) ========== */

static uint8_t fblob[16384];
static uint8_t fstr[4096];
static uint32_t fstr_used;

static uint8_t *fw;		/* walks fblob from offset 0x28 */

static uint32_t fstr_ref(const char *s)
{
	size_t want = strlen(s) + 1;
	for (uint32_t i = 0; i + want <= fstr_used; i++)
		if (!memcmp(fstr + i, s, want))
			return i;
	memcpy(fstr + fstr_used, s, want);
	uint32_t off = fstr_used;
	fstr_used = (uint32_t)(fstr_used + want);
	return off;
}

static void fw_begin(const char *name)
{
	size_t n = strlen(name) + 1;
	*fw++ = 0; *fw++ = 0; *fw++ = 0;
	*fw++ = 1;					/* FDT_BEGIN_NODE */
	memcpy(fw, name, n);
	fw += (n + 3) & ~3U;
}

static void fw_prop(const char *name, const void *val, uint32_t len)
{
	uint32_t be = 0;
	*fw++ = 0; *fw++ = 0; *fw++ = 0;
	*fw++ = 3;					/* FDT_PROP */
	be = __builtin_bswap32(len);
	memcpy(fw, &be, 4); fw += 4;
	be = __builtin_bswap32(fstr_ref(name));
	memcpy(fw, &be, 4); fw += 4;
	memset(fw, 0, (len + 3) & ~3U);
	memcpy(fw, val, len);
	fw += (len + 3) & ~3U;
}

static void fw_prop_u32(const char *name, uint32_t v)
{
	fw_prop(name, &(uint32_t){ __builtin_bswap32(v) }, 4);
}

static void fw_prop_u64(const char *name, uint64_t v)
{
	uint64_t be = __builtin_bswap64(v);
	fw_prop(name, &be, 8);
}

static void fw_prop_reg(uint64_t base, uint64_t size)
{
	uint64_t r[2] = { __builtin_bswap64(base), __builtin_bswap64(size) };
	fw_prop("reg", r, 16);
}

static void fw_end(void)
{
	*fw++ = 0; *fw++ = 0; *fw++ = 0;
	*fw++ = 2;					/* FDT_END_NODE */
}

static void build_fdt(void)
{
	memset(fblob, 0, sizeof(fblob));
	memset(fstr, 0, sizeof(fstr));
	fstr_used = 0;
	fw = fblob + 0x28;

	fw_begin("");					/* root */
	fw_prop_u32("#address-cells", 2);
	fw_prop_u32("#size-cells", 2);

	fw_begin("chosen");
	fw_prop_u64("linux,initrd-start", 0x4b800000ull);
	fw_prop_u64("linux,initrd-end", 0x4d000000ull);
	fw_end();

	fw_begin("memory@40000000");
	fw_prop_reg(0x40000000ull, 0x40000000ull);	/* 1 GiB low bank */
	fw_end();
	fw_begin("memory@c0000000");
	fw_prop_reg(0xc0000000ull, 0x80000000ull);	/* 2 GiB bank */
	fw_end();

	fw_begin("reserved-memory");
	fw_begin("splash_region@9c000000");
	fw_prop("label", "cont_splash_region", sizeof("cont_splash_region"));
	fw_prop_reg(0x9c000000ull, 0x2300000ull);
	fw_end();
	fw_end();

	fw_end();					/* root end */
	*fw++ = 0; *fw++ = 0; *fw++ = 0;
	*fw++ = 9;					/* FDT_END */

	uint32_t totalsize = (uint32_t)((uint8_t *)fw - fblob) + fstr_used;
	uint32_t strstart = (uint32_t)((uint8_t *)fw - fblob);
	uint32_t magic = __builtin_bswap32(0xd00dfeed);
	uint32_t be = __builtin_bswap32(totalsize);
	memcpy(fblob + 0, &magic, 4);
	memcpy(fblob + 4, &be, 4);			/* header is BE too */
	be = __builtin_bswap32(0x28);
	memcpy(fblob + 8, &be, 4);			/* off_dt_struct */
	be = __builtin_bswap32(strstart);
	memcpy(fblob + 12, &be, 4);			/* off_dt_strings */
	memcpy(fblob + strstart, fstr, fstr_used);
}

/* ================= synthetic ADT (tools/adt.py layout) ================= */

static uint8_t ablob[8192];

static uint8_t *aprop(uint8_t *d, const char *name, const void *val, uint32_t len)
{
	memset(d, 0, 32);
	memcpy(d, name, strlen(name));
	memcpy(d + 32, &len, 4);			/* host LE, like the ADT */
	if (len) {
		memset(d + 36, 0, (len + 3) & ~3U);
		memcpy(d + 36, val, len);
	}
	return d + 36 + ((len + 3) & ~3U);
}

static uint8_t *aprop_cstr(uint8_t *d, const char *s)
{
	return aprop(d, "name", s, (uint32_t)strlen(s) + 1);
}

static uint8_t *aprop_u32(uint8_t *d, const char *name, uint32_t v)
{
	return aprop(d, name, &v, 4);
}

static uint8_t *aprop_u64x1(uint8_t *d, const char *name, uint64_t a)
{
	return aprop(d, name, &a, 8);
}

static uint8_t *aprop_u64x2(uint8_t *d, const char *name, uint64_t a, uint64_t b)
{
	uint64_t v[2] = { a, b };
	return aprop(d, name, v, 16);
}

/* node header (nprops, nchildren) + the mandatory name prop */
static uint8_t *anode(uint8_t *d, const char *nname, uint32_t nprops, uint32_t nchildren)
{
	uint32_t h[2] = { nprops, nchildren };
	memcpy(d, h, 8);
	return aprop_cstr(d + 8, nname);
}

static void build_adt(void)
{
	memset(ablob, 0, sizeof(ablob));
	uint8_t *root = ablob;
	uint8_t *after = anode(root, "device-tree", 1, 3);

	after = anode(after, "chosen", 3, 0);
	after = aprop_u64x1(after, "dram-base", 0x40000000ull);
	after = aprop_u64x1(after, "dram-size", 0x3ff00000ull);

	after = anode(after, "pram", 3, 0);
	after = aprop_u32(after, "AAPL,phandle", 0x2123);
	after = aprop_u64x2(after, "reg", 0x7ff00000ull, 0x100000ull);

	after = anode(after, "sweet-fbcon", 2, 0);
	after = aprop_u64x2(after, "reg", 0x9c000000ull, 0x2300000ull);
}

/* ================================ tests ================================ */

int main(void)
{
	int n;
	struct fdt_reg banks[8], splash;
	uint64_t v;

	build_fdt();
	uintptr_t fdt = (uintptr_t)fblob;

	if (getenv("DBG")) {
		const unsigned char *d = fblob;
		for (int row = 0; row < 0x100; row += 24) {
			printf("%02x:", row);
			for (int i = 0; i < 24; i++)
				printf(" %02x", d[row + i]);
			printf("\n");
		}
		printf("strings at +0x%x:", (unsigned)(fw - fblob));
		const unsigned char *s = fstr;
		for (unsigned i = 0; i < fstr_used; i++)
			printf(" %02x", s[i]);
		printf("\n");
	}

	n = fdt_memory_banks(fdt, banks, 8);
	CHECK(n == 2, "two memory banks");
	CHECK(banks[0].base == 0x40000000ull && banks[0].size == 0x40000000ull,
	      "low bank 1 GiB");
	CHECK(banks[1].base == 0xc0000000ull && banks[1].size == 0x80000000ull,
	      "high bank 2 GiB");

	CHECK(fdt_splash_region(fdt, &splash) == 1, "splash found (name branch)");
	CHECK(splash.base == 0x9c000000ull && splash.size == 0x2300000ull,
	      "splash reg values");

	CHECK(fdt_chosen_u64(fdt, "linux,initrd-start", &v) == 1 && v == 0x4b800000ull,
	      "chosen u64 read");
	CHECK(fdt_chosen_u64(fdt, "no,such-prop", &v) == 0, "chosen miss");

	/* loader_probe fills the shared state and rewires the console */
	loader_probe(fdt);
	CHECK(g_banks_n == 2 && g_splash_ok == 1, "probe shared state");
	CHECK(con_fb == (volatile uint32_t *)(uintptr_t)0x9c000000ull,
	      "console switched to splash");

	/* reconcile rules */
	{
		struct fdt_reg save[8];
		memcpy(save, g_banks, sizeof(save));
		g_banks[0].base = 0x80000000ull, g_banks[0].size = 0x80000000ull;
		g_banks[1].base = 0x100000000ull, g_banks[1].size = 0xfcc00000ull;
		CHECK(bank_fits(0xb0000000ull, 0x4cf00000ull),
		      "sweet baked map fits sweet live banks");
		memcpy(g_banks, save, sizeof(save));
	}
	CHECK(bank_fits(0x40000000ull, 0x3ff00000ull),
	      "lisa first-guess fits lisa low bank");
	{
		struct fdt_reg save[8];
		memcpy(save, g_banks, sizeof(save));
		g_banks[0].base = 0x40000000ull, g_banks[0].size = 0x40000000ull;
		g_banks[1].base = 0xc0000000ull, g_banks[1].size = 0x80000000ull;
		CHECK(!bank_fits(0xb0000000ull, 0x4cf00000ull),
		      "sweet map rejected by lisa bank list");
		const struct fdt_reg *pk = bank_pick();
		CHECK(pk && pk->base == 0xc0000000ull && pk->size == 0x80000000ull,
		      "pick largest bank below CLADE");
		memcpy(g_banks, save, sizeof(save));
	}

	/* ADT walker + patcher */
	build_adt();
	uintptr_t root = (uintptr_t)ablob;
	uintptr_t chosen = adt_child(root, "chosen");
	uintptr_t pram = adt_child(root, "pram");
	uintptr_t fbcon = adt_child(root, "sweet-fbcon");
	CHECK(chosen != 0, "adt_child chosen");
	CHECK(pram != 0, "adt_child pram");
	CHECK(fbcon != 0, "adt_child fbcon");
	CHECK(adt_child(root, "nosuch") == 0, "adt_child miss");

	uint32_t len = 0;
	CHECK(adt_prop(chosen, "dram-base", &len) != 0 && len == 8, "adt_prop dram-base");
	CHECK(adt_prop(chosen, "dram-size", &len) != 0 && len == 8, "adt_prop dram-size");
	CHECK(adt_prop(pram, "reg", &len) != 0 && len == 16, "adt_prop pram reg");
	CHECK(adt_prop(chosen, "reg", &len) == 0, "adt_prop miss");

	{
		uint32_t sz = 0;
		uintptr_t val = adt_prop(fbcon, "reg", &sz);
		CHECK(*(uint64_t *)val == 0x9c000000ull && *(uint64_t *)(val + 8) == 0x2300000ull,
		      "fbcon reg pre-state");
	}

	uint64_t prvals[2] = { 0x7ff00000ull, 0x100000ull };
	CHECK(adt_patch_u64s(pram, "reg", prvals, 2) == 1, "patch pram reg ok");
	{
		uint32_t sz = 0;
		uintptr_t val = adt_prop(pram, "reg", &sz);
		CHECK(sz == 16 && *(uint64_t *)val == 0x7ff00000ull &&
		      *(uint64_t *)(val + 8) == 0x100000ull, "pram reg patched in place");
	}
	uint64_t wrong[3] = { 1, 2, 3 };
	CHECK(adt_patch_u64s(pram, "reg", wrong, 3) == 0, "size mismatch refused");
	CHECK(adt_patch_u64s(0, "reg", prvals, 2) == 0, "null node refused");

	{
		uint64_t dbv[1] = { 0x40000000ull };
		CHECK(adt_patch_u64s(chosen, "dram-base", dbv, 1) == 1, "patch dram-base ok");
	}

	/* the serialized layout must not have moved anything */
	CHECK(adt_child(root, "sweet-fbcon") == fbcon, "layout intact after patch");

	printf("%s\n", fails ? "FAILED" : "ALL PASS");
	return fails != 0;
}
