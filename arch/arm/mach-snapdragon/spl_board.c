// SPDX-License-Identifier: GPL-2.0+
/*
 * Chainloaded-SPL board hooks for Qualcomm Snapdragon.
 *
 * ABL hands us a populated memory image and an FDT pointer; SPL only needs
 * enough scaffolding to do DTB selection and jump to full U-Boot. Reset and
 * boot-device discovery are stubbed accordingly.
 */

#include <spl.h>
#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

u32 spl_boot_device(void)
{
	return BOOT_DEVICE_RAM;
}

void reset_cpu(void)
{
	while (1)
		;
}

/*
 * Override the default SPL jump so we can put the selected DTB into x0
 * before transferring control. Full U-Boot's save_boot_params() picks
 * x0 up as the previous-bootloader FDT pointer, and board_fdt_blob_setup()
 * then uses it as the working device tree. Without this, full U-Boot
 * falls back to its built-in DTB and the runtime selection we did in
 * SPL is discarded.
 */
void __noreturn jump_to_image(struct spl_image_info *spl_image)
{
	register unsigned long x0 asm("x0") = (unsigned long)gd->fdt_blob;
	register unsigned long entry asm("x1") = spl_image->entry_point;

	asm volatile("br %1\n\t" :: "r"(x0), "r"(entry));
	__builtin_unreachable();
}
