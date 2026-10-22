#pragma once
#include <stdint.h>
#include "fdt.h"

/* Live-FDT facts probed in shim_main before the first console pixel:
 *
 * splash: the ABL's continued-splash framebuffer (reserved-memory
 * "cont_splash_region"); console.c paints into it and the boot_args Video +
 * the ADT /arm-io/sweet-fbcon reg must all use this base.
 *
 * fiscal: the DRAM banks the bootloader published in /memory; loader.c
 * reconciles the payload's phys_base/mem_size against them, so a first-boot
 * memory guess can never trap the machine (see NOTES.md 2026-10-22). */
extern struct fdt_reg g_splash;
extern int g_splash_ok;
extern struct fdt_reg g_banks[8];
extern int g_banks_n;

/* Boot the SWEETXNU payload found in the ABL ramdisk; returns only on error. */
const char *loader_boot(uintptr_t dtb);

/* Read the live-FDT facts (splash framebuffer, DRAM banks) and switch the
 * console framebuffer; call before any console output. */
void loader_probe(uintptr_t dtb);
