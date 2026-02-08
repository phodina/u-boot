// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2016
 * Xilinx, Inc.
 *
 * (C) Copyright 2016
 * Toradex AG
 *
 * Michal Simek <michal.simek@amd.com>
 * Stefan Agner <stefan.agner@toradex.com>
 */
#include <binman_sym.h>
#include <image.h>
#include <log.h>
#include <mapmem.h>
#include <spl.h>
#include <linux/libfdt.h>
#ifdef CONFIG_SPL_QCOM_SMEM
#include <env.h>
#include <cmdline_utils.h>
#include <soc/qcom/socinfo.h>
#include <dtb_select.h>
#endif

static ulong spl_ram_load_read(struct spl_load_info *load, ulong sector,
			       ulong count, void *buf)
{
	ulong addr = 0;

	debug("%s: sector %lx, count %lx, buf %lx\n",
	      __func__, sector, count, (ulong)buf);

	if (IS_ENABLED(CONFIG_SPL_LOAD_FIT)) {
		addr = IF_ENABLED_INT(CONFIG_SPL_LOAD_FIT,
				      CONFIG_SPL_LOAD_FIT_ADDRESS);

#ifdef	CONFIG_SPL_PCI_DFU
		if (spl_boot_device() == BOOT_DEVICE_PCIE)
			addr = CONFIG_SPL_PCI_DFU_SPL_LOAD_FIT_ADDRESS;
#endif
	}
	addr += sector;
	if (CONFIG_IS_ENABLED(IMAGE_PRE_LOAD))
		addr += image_load_offset;

	memcpy(buf, (void *)addr, count);

	return count;
}

static int spl_ram_load_image(struct spl_image_info *spl_image,
			      struct spl_boot_device *bootdev)
{
	struct legacy_img_hdr *header;
	ulong addr = 0;
	int ret;

#ifdef CONFIG_SPL_QCOM_SMEM
	ret = qcom_socinfo_init();
	if (ret)
		debug("Warning: Failed to initialize socinfo: %d\n", ret);
	else
		debug("Socinfo initialized successfully\n");

	/* Parse Android boot parameters from bootargs if available */
	do {
		const char *bootargs = env_get("bootargs");
		struct androidboot_params boot_params;

		if (bootargs) {
			ret = parse_androidboot_params(bootargs, &boot_params);
			if (ret == 0) {
				debug("Android boot parameters parsed:\n");
				if (boot_params.hardware)
					debug("  Hardware: %s\n", boot_params.hardware);
				if (boot_params.serialno)
					debug("  Serial: %s\n", boot_params.serialno);
				if (boot_params.bootloader)
					debug("  Bootloader: %s\n", boot_params.bootloader);
				if (boot_params.slot_suffix)
					debug("  Slot suffix: %s\n", boot_params.slot_suffix);

				free_androidboot_params(&boot_params);
			} else {
				debug("Failed to parse Android boot parameters: %d\n", ret);
			}
		} else {
			debug("No bootargs found for Android parameter parsing\n");
		}
	} while (0);

	/* Scan for appended DTBs and select the appropriate one */
	do {
		void *selected_dtb = NULL;

		/* Use unified DTB selection that handles both appended and FIT DTBs */
		selected_dtb = qcom_select_dtb_from_socinfo_and_cmdline();

		if (selected_dtb) {
			debug("Selected DTB at address 0x%p\n", selected_dtb);
			/* Set the selected DTB as the working DTB */
			gd->fdt_blob = selected_dtb;
			gd->fdt_size = fdt_totalsize(selected_dtb);
		} else {
			debug("Warning: No suitable DTB found, using default\n");
		}
	} while (0);
#endif

	if (IS_ENABLED(CONFIG_SPL_LOAD_FIT)) {
		addr = IF_ENABLED_INT(CONFIG_SPL_LOAD_FIT,
				      CONFIG_SPL_LOAD_FIT_ADDRESS);

#ifdef CONFIG_SPL_PCI_DFU
		if (spl_boot_device() == BOOT_DEVICE_PCIE)
			addr = CONFIG_SPL_PCI_DFU_SPL_LOAD_FIT_ADDRESS;
#endif
	}

	if (CONFIG_IS_ENABLED(IMAGE_PRE_LOAD)) {
		ret = image_pre_load(addr);

		if (ret)
			return ret;

		addr += image_load_offset;
	}
	header = map_sysmem(addr, 0);

#if CONFIG_IS_ENABLED(DFU)
	if (bootdev->boot_device == BOOT_DEVICE_DFU)
		spl_dfu_cmd(0, "dfu_alt_info_ram", "ram", "0");
#endif

#if CONFIG_IS_ENABLED(PCI_DFU)
	if (bootdev->boot_device == BOOT_DEVICE_PCIE)
		spl_dfu_cmd(0, "dfu_alt_info_ram", "ram", "0");
#endif

	if (IS_ENABLED(CONFIG_SPL_LOAD_FIT) &&
	    image_get_magic(header) == FDT_MAGIC) {
		struct spl_load_info load;

		debug("Found FIT\n");
		spl_load_init(&load, spl_ram_load_read, NULL, 1);
		ret = spl_load_simple_fit(spl_image, &load, 0, header);
	} else {
		ulong u_boot_pos = spl_get_image_pos();

		debug("Legacy image\n");
		/*
		 * Get the header.  It will point to an address defined by
		 * handoff which will tell where the image located inside
		 * the flash.
		 */
		debug("u_boot_pos = %lx\n", u_boot_pos);
		if (u_boot_pos == BINMAN_SYM_MISSING) {
			/*
			 * No binman support or no information. For now, fix it
			 * to the address pointed to by U-Boot.
			 */
			u_boot_pos = (ulong)spl_get_load_buffer(-sizeof(*header),
								sizeof(*header));
		}
		header = map_sysmem(u_boot_pos, 0);

		ret = spl_parse_image_header(spl_image, bootdev, header);
	}

	return ret;
}
#if CONFIG_IS_ENABLED(RAM_DEVICE)
SPL_LOAD_IMAGE_METHOD("RAM", 0, BOOT_DEVICE_RAM, spl_ram_load_image);
#endif
#if CONFIG_IS_ENABLED(DFU)
SPL_LOAD_IMAGE_METHOD("DFU", 0, BOOT_DEVICE_DFU, spl_ram_load_image);
#endif
#if CONFIG_IS_ENABLED(PCI_DFU)
SPL_LOAD_IMAGE_METHOD("PCIE", 0, BOOT_DEVICE_PCIE, spl_ram_load_image);
#endif
