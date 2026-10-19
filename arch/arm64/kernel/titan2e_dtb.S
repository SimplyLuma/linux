/* SPDX-License-Identifier: GPL-2.0 */
/* Built-in mainline device tree for Titan 2 Elite bring-up. */
	.section .rodata, "a"
	.balign	4096
	.globl	titan2e_dtb
titan2e_dtb:
	.incbin	CONFIG_TITAN2E_BUILTIN_DTB_PATH
	.balign	8
