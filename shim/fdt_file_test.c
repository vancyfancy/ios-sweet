/*
 * Run the shim's FDT readers against real DTB files (host side):
 *   cc -arch arm64 shim/fdt_file_test.c -o /tmp/fdt_file_test
 *   /tmp/fdt_file_test _lisa/rom/parts/merged/lisa-base07.dtb ...
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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

int main(int argc, char **argv)
{
	for (int a = 1; a < argc; a++) {
		FILE *f = fopen(argv[a], "rb");
		if (!f) {
			perror(argv[a]);
			return 1;
		}
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		uint8_t *buf = malloc(sz + 8);
		if (!buf || fread(buf, 1, sz, f) != (size_t)sz) {
			fprintf(stderr, "read %s failed\n", argv[a]);
			return 1;
		}
		fclose(f);
		uintptr_t fdt = (uintptr_t)buf;

		printf("== %s (%ld bytes)\n", argv[a], sz);
		struct fdt_reg banks[8];
		int n = fdt_memory_banks(fdt, banks, 8);
		printf("   /memory banks: %d\n", n);
		for (int i = 0; i < n && i < 8; i++)
			printf("    [%d] base=0x%llx size=0x%llx\n", i,
			       (unsigned long long)banks[i].base,
			       (unsigned long long)banks[i].size);
		struct fdt_reg splash;
		int sp = fdt_splash_region(fdt, &splash);
		if (sp)
			printf("   splash: base=0x%llx size=0x%llx\n",
			       (unsigned long long)splash.base,
			       (unsigned long long)splash.size);
		else
			printf("   splash: none\n");
		uint64_t v;
		if (fdt_chosen_u64(fdt, "linux,initrd-start", &v))
			printf("   /chosen linux,initrd-start=0x%llx\n", (unsigned long long)v);
		if (fdt_chosen_u64(fdt, "linux,initrd-end", &v))
			printf("   /chosen linux,initrd-end=0x%llx\n", (unsigned long long)v);
	}
	return 0;
}
