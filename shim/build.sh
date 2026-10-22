#!/bin/bash
# Build the shim and wrap it in a boot.img v2 that mirrors the stock PixelOS header.
set -euo pipefail
cd "$(dirname "$0")"
# Cross toolchain: prefer the Linux gcc, fall back to the bare-metal
# aarch64-elf-gcc (brew). -ffreestanding -nostdlib never touches a libc,
# so the elf and linux variants produce identical code here.
CC=${CC:-}
if [ -z "$CC" ]; then
	if command -v aarch64-linux-gnu-gcc >/dev/null; then
		CC=aarch64-linux-gnu-gcc
	else
		CC=aarch64-elf-gcc
	fi
fi
case "$CC" in
	*-gcc) TRIPLET=${CC%-gcc};;
	*-clang) TRIPLET=${CC%-clang};;
	*) TRIPLET=$CC;;
esac
OBJCOPY=${OBJCOPY:-$TRIPLET-objcopy}
READELF=${READELF:-$TRIPLET-readelf}
NM=${NM:-$TRIPLET-nm}
echo "cross cc: $CC"
# mkbootimg: the distro binary when present, else the vendored script.
MKBOOTIMG=${MKBOOTIMG:-}
if [ -z "$MKBOOTIMG" ]; then
	if command -v mkbootimg >/dev/null; then
		MKBOOTIMG=mkbootimg
	else
		MKBOOTIMG="python3 ../tools/mkbootimg.py"
	fi
fi
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$($READELF -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$($NM out/shim.elf | awk '$3=="_end"{print $1}')
truncate -s "$(printf '%d' "0x$size")" out/Image
# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an empty
# newc cpio (ABL dislikes a zero-size ramdisk on some builds) -> diagnostics.
if [ -n "${1:-}" ]; then
	cp "$1" out/ramdisk.bin
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
DTB=${DTB:-../dtb/boot_unpacked/dtb}
[ -f "$DTB" ] || { echo "error: boot dtb '$DTB' not found (device dump; set DTB=... or extract from stock boot.img)" >&2; exit 1; }
$MKBOOTIMG --header_version 2 --kernel out/Image --ramdisk out/ramdisk.bin \
	--dtb "$DTB" --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
	--tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 \
	--os_version 17.0.0 --os_patch_level 2035-06 \
	--cmdline "console=null" -o out/shim-boot.img
# Host render of the console layout.
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test || true
ls -l out/Image out/shim-boot.img
