/*
 * sweet boot shim entry: arm64 Linux Image header so Qualcomm ABL treats
 * us like a kernel. ABL enters at EL1, MMU/D-cache off, x0 = DTB pointer.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 (PE offset) */

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* DTB pointer from ABL */

	/*
	 * The ARM64 boot spec says ABL hands over with MMU and D-cache off,
	 * and the sweet ABL does exactly that. The lisa ABL (SM7325) does
	 * not: its page tables stay on (proved by a level-1 translation
	 * fault in loader.c: adt_patch_u64s, whose ADT tail lands far
	 * outside anything the ABL mapped). Canonically shut down what we
	 * find: clear M+C, drop its translation state, then clean the
	 * whole D-cache by set/way - CLEAN not invalidate, so any lines
	 * the ABL left dirty are flushed to RAM instead of destroying
	 * the payload it copied through its cache. After this the shim
	 * runs at VA==PA like on sweet.
	 */
	mrs	x5, sctlr_el1
	mov	x6, #5			/* M (bit0) | C (bit2) */
	bic	x5, x5, x6
	dsb	sy
	msr	sctlr_el1, x5
	dsb	sy
	isb
	tlbi	vmalle1
	dsb	sy
	isb

	/* D-cache clean by set/way over every level with a data side. */
	mrs	x14, clidr_el1
	cbz	x14, 9f
	mov	x4, #0			/* csselr = level << 1 */
	mov	x5, #0			/* clidr shift = 3 * level */
1:	lsr	x6, x14, x5
	and	x6, x6, #7		/* Ctype of this level (2=D,4=unified) */
	cmp	x6, #2
	b.lt	2f
	msr	csselr_el1, x4
	isb
	mrs	x6, ccsidr_el1
	ubfx	x7, x6, #3, #10		/* log2(ways) - 1 */
	ubfx	x8, x6, #13, #15	/* NumSets - 1 */
	mov	x9, xzr			/* set counter */
3:	mov	x10, xzr		/* way counter */
4:	mov	x11, x4
	orr	x11, x11, x9, lsl #13	/* | set << 13 */
	mov	x12, #31
	sub	x12, x12, x7		/* way field shift = 32 - log2(ways) */
	lsl	x16, x10, x12
	orr	x11, x11, x16		/* | way << shift */
	dc	csw, x11
	add	x10, x10, #1
	mov	x12, x7
	add	x12, x12, #1
	mov	x16, #1
	lsl	x12, x16, x12		/* ways = 1 << log2(ways) */
	cmp	x10, x12
	b.lt	4b
	add	x9, x9, #1
	cmp	x9, x8
	b.le	3b
2:	add	x4, x4, #2		/* next level's csselr */
	add	x5, x5, #3
	add	x6, x4, #2
	lsr	x6, x6, #1		/* next level number */
	mov	x12, x14
	lsr	x16, x12, #26
	and	x16, x16, #15		/* LoC field */
	cmp	x6, x16
	b.lt	1b
	dsb	sy
9:

	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	/* zero .bss */
	adr	x1, _bss_start
	adr	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	mov	x0, x19
	bl	shim_main
3:	wfe
	b	3b

/* PSCI CPU_ON target. x0 = context id = cpu index (1..7). */
	.globl secondary_entry
secondary_entry:
	msr	daifset, #0xf
	msr	spsel, #1
	adr	x1, _sec_stacks
	add	x1, x1, x0, lsl #12
	add	x1, x1, #0x1000
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	bl	secondary_main
4:	wfe
	b	4b

/*
 * Exception vectors. Each slot stashes x29/x30, loads its slot number
 * and jumps to the common path, which saves x0-x28 and calls
 * exc_handler(slot, regs). IRQs return; everything else ends in the
 * on-screen panic.
 */
.macro vent slot
	.balign	0x80
	stp	x29, x30, [sp, #-16]!
	mov	x29, #\slot
	b	exc_common
.endm

	.balign	0x800
	.globl vectors
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	sub	sp, sp, #(30 * 8)
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	str	x28, [sp, #224]
	mov	x0, x29
	mov	x1, sp
	bl	exc_handler
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldr	x28, [sp, #224]
	add	sp, sp, #(30 * 8)
	ldp	x29, x30, [sp], #16
	eret
