/*
 * Milestone 4: act as iBoot for xnu.
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT, so this only has to: find the payload in the ABL ramdisk, reconcile
 * its physical-memory map and framebuffer against the live FDT the ABL
 * passes (an offline guess can never trip the first boot), copy the
 * segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump to the entry point with the MMU
 * and caches off, x0 = boot_args (physical).
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL
/* keep in sync with tools/mkadt.py (PANIC_LOG_SIZE) */
#define PANIC_LOG_SIZE		0x100000UL
/* addresses above this line are the SM7325 modem-CLADE carve-out: prefer
 * banks below it when relocating */
#define PREF_BANK_TOP		0xe0000000UL
#define MIN_BANK_SIZE		0x20000000UL	/* never relocate into <512MiB */

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* live-FDT facts (loader.h); probed before the first console pixel */
struct fdt_reg g_splash;
int g_splash_ok;
struct fdt_reg g_banks[8];
int g_banks_n;

void loader_probe(uintptr_t dtb)
{
	g_splash_ok = fdt_splash_region(dtb, &g_splash);
	g_banks_n = fdt_memory_banks(dtb, g_banks, 8);
	if (g_splash_ok)
		con_fb = (volatile uint32_t *)(uintptr_t)g_splash.base;
}

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

/*
 * In-place ADT patching. Serialization of tools/adt.py:
 *   node = {u32 nprops, u32 nchildren, props..., children...}
 *   prop = {char name[32], u32 size (bit31 flag), value padded to 4}
 * Only fixed-size in-place value rewrites are done; the byte layout (and
 * every offset mkpayload recorded) stays valid.
 */

static uint32_t adt_u32(uintptr_t a)
{
	return *(volatile uint32_t *)a;
}

/* where a node's children start (= end of its property list) */
static uintptr_t adt_children(uintptr_t n)
{
	uint32_t nprops = adt_u32(n);
	uintptr_t p = n + 8;
	for (uint32_t i = 0; i < nprops; i++)
		p += 36 + (((adt_u32(p + 32) & 0x7fffffff) + 3) & ~3U);
	return p;
}

/* one past a node's subtree (its next sibling) */
static uintptr_t adt_end(uintptr_t n)
{
	uint32_t nchildren = adt_u32(n + 4);
	uintptr_t p = adt_children(n);
	for (uint32_t i = 0; i < nchildren; i++)
		p = adt_end(p);
	return p;
}

static int adt_field_eq(uintptr_t field, const char *want)
{
	/* 32-byte zero-padded name field vs. a C string */
	for (int i = 0; i < 32; i++) {
		char c = *(volatile char *)(field + i);
		if (want[i] == 0)
			return c == 0;
		if (c != want[i])
			return 0;
	}
	return 1;
}

static int adt_node_name_eq(uintptr_t n, const char *want)
{
	/* a node's first property is always ('name', 0, cstr(node-name)):
	 * the prop's name field says "name", its value holds the node name */
	return adt_field_eq(n + 8, "name") && adt_field_eq(n + 8 + 36, want);
}

/* the child of n whose name prop is exactly `want` (0 when absent) */
static uintptr_t adt_child(uintptr_t n, const char *want)
{
	uint32_t nchildren = adt_u32(n + 4);
	uintptr_t p = adt_children(n);
	for (uint32_t i = 0; i < nchildren; i++) {
		if (adt_node_name_eq(p, want))
			return p;
		p = adt_end(p);
	}
	return 0;
}

/* the value of the prop `want` inside node n (00 when absent; len out) */
static uintptr_t adt_prop(uintptr_t n, const char *want, uint32_t *out_len)
{
	uint32_t nprops = adt_u32(n);
	uintptr_t p = n + 8;
	for (uint32_t i = 0; i < nprops; i++) {
		uint32_t size = adt_u32(p + 32) & 0x7fffffff;
		if (adt_field_eq(p, want)) {
			*out_len = size;
			return p + 36;
		}
		p += 36 + ((size + 3) & ~3U);
	}
	return 0;
}

/* overwrite a prop made of `cnt` little-endian u64s; keeps its size */
static int adt_patch_u64s(uintptr_t n, const char *propname, uint64_t *vals, int cnt)
{
	uint32_t len;
	uintptr_t v = n ? adt_prop(n, propname, &len) : 0;
	if (!v || len != (uint32_t)cnt * 8)
		return 0;
	volatile uint64_t *dst = (volatile uint64_t *)v;
	for (int i = 0; i < cnt; i++)
		dst[i] = vals[i];
	return 1;
}

/* true when [phys, phys+size) lies within any reported bank */
static int bank_fits(uint64_t phys, uint64_t size)
{
	for (int i = 0; i < g_banks_n; i++) {	/* g_banks_n may exceed the array */
		if (i >= 8)
			break;
		struct fdt_reg *b = &g_banks[i];
		if (phys >= b->base && size <= b->size && phys - b->base <= b->size - size)
			return 1;
	}
	return 0;
}

/* largest reported bank, preferring the space below the CLADE carve-out */
static const struct fdt_reg *bank_pick(void)
{
	const struct fdt_reg *lo = 0, *hi = 0;
	for (int i = 0; i < g_banks_n && i < 8; i++) {
		struct fdt_reg *b = &g_banks[i];
		if (b->base < PREF_BANK_TOP) {
			if (!lo || b->size > lo->size)
				lo = b;
		} else if (!hi || b->size > hi->size) {
			hi = b;
		}
	}
	return lo ? lo : hi;
}

const char *loader_boot(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;

	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return "no initrd in /chosen";
	con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);

	struct payload *p = (struct payload *)rd_start;
	if (!magic_ok(p->magic))
		return "no SWEETXNU payload";
	if (p->version != 1 || (p->adt_off & 7) || (rd_start & 7))
		return "bad payload header";

	/*
	 * Reconcile the payload's physical-memory map against the live FDT.
	 * Keep the baked values whenever they already point into a reported
	 * bank (the sweet case); only an impossible map (first-boot guesses)
	 * relocates to the bank the bootloader actually publishes.
	 */
	uint64_t phys = p->phys_base, moved = 0;
	/* baked p->mem_size already excludes the panic log (mkpayload took it
	 * from the ADT); a relocated map must shrink the raw bank size once */
	uint64_t adt_mem;
	if (g_banks_n > 0 && !bank_fits(phys, p->mem_size)) {
		const struct fdt_reg *bk = bank_pick();
		if (bk && bk->size >= MIN_BANK_SIZE) {
			con_hex2("PHYSFIX", "was=", phys, "sz=", p->mem_size);
			phys = bk->base;
			adt_mem = bk->size - PANIC_LOG_SIZE;
			moved = 1;
		} else
			adt_mem = p->mem_size;	/* cannot relocate; keep the baked map */
	} else
		adt_mem = p->mem_size;
	con_hex2("FDTB", "banks=", g_banks_n, "splash=", g_splash_ok);
	if (g_banks_n > 0)
		con_hex2("BANK0", "base=", g_banks[0].base, "size=", g_banks[0].size);

	/* mkpayload bakes the ADT with the panic-log carve already applied:
	 * dram-size excludes it, /pram reg sits at the hole's top. A relocated
	 * map gets the same recomputation; the fbcon reg always tracks the
	 * splash region (identical values on sweet = no-op). */
	uintptr_t adt = rd_start + p->adt_off;
	uint64_t prvals[] = { phys + adt_mem, PANIC_LOG_SIZE };
	uint64_t dbval[] = { phys };
	uint64_t dsval[] = { adt_mem };
	if (moved) {
		int pram_ok = adt_patch_u64s(adt_child(adt, "pram"), "reg", prvals, 2);
		int db_ok = adt_patch_u64s(adt_child(adt, "chosen"), "dram-base", dbval, 1);
		int ds_ok = adt_patch_u64s(adt_child(adt, "chosen"), "dram-size", dsval, 1);
		con_hex2("ADTPATCH", "pram=", pram_ok, "dram=", (uint64_t)(db_ok && ds_ok));
	}
	/* the framebuffer ADT reg always tracks what the splash region says:
	 * on sweet the values are already identical, on lisa this is what
	 * connects the ABL's placement to the console driver */
	if (g_splash_ok) {
		uintptr_t fbcon = adt_child(adt, "sweet-fbcon");
		uint64_t fbvals[] = { g_splash.base, g_splash.size };
		int fb_ok = adt_patch_u64s(fbcon, "reg", fbvals, 2);
		con_hex2("FBPATCH", "ok=", fb_ok, "base=", g_splash.base);
	}

	p->phys_base = phys;
	p->mem_size = adt_mem;
	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	con_hex2("PHYS", "base=", p->phys_base, "size=", p->mem_size);
	con_hex("VIRT", p->virt_base);
	con_hex("ENTRY", p->entry_va);

	/* The payload must not overlap where the kernel goes. */
	if (rd_end > p->phys_base && rd_start < p->phys_base + p->mem_size)
		return "ramdisk overlaps kernel memory";

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", p->nsegs, "end=", p->kernel_end + pa_of_va_base);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, adt, p->adt_size);
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = g_splash_ok ? g_splash.base : FB_BASE;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);

	uint64_t entry_pa = p->entry_va + pa_of_va_base;
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping (fbcon clears screen)");

	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
