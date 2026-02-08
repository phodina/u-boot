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
#include <asm/global_data.h>
#include <linux/libfdt.h>
#ifdef CONFIG_SPL_QCOM_SMEM
#include <soc/qcom/socinfo.h>
#include <dtb_select.h>
#endif

DECLARE_GLOBAL_DATA_PTR;

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
	if (ret) {
		printf("SPL: qcom_socinfo_init() failed: %d\n", ret);
	} else {
		printf("SPL: qcom_socinfo_init() OK\n");
		qcom_socinfo_print();
	}

	/* Scan for appended DTBs and select the appropriate one */
	do {
		void *selected_dtb = NULL;

		selected_dtb = qcom_select_dtb_from_socinfo_and_cmdline();

		if (selected_dtb) {
			printf("SPL: selected DTB at %p\n", selected_dtb);
			selected_dtb = qcom_patch_dtb_with_abl_memory(selected_dtb);
			gd->fdt_blob = selected_dtb;
		} else {
			printf("SPL: no matching DTB selected\n");
		}
	} while (0);

	/*
	 * Chainload into full U-Boot. Packaging places the padded SPL at
	 * SPL load (0x80080000), candidate DTBs at +1 MiB, and u-boot.bin
	 * at +3 MiB (giving DTBs a 2 MiB region). Just point spl_image at
	 * u-boot.bin and let the SPL framework jump there. The board-level
	 * jump_to_image override puts the selected DTB into x0 so full
	 * U-Boot picks it up via board_fdt_blob_setup() / save_boot_params().
	 */
	spl_image->name = "U-Boot";
	spl_image->os = IH_OS_U_BOOT;
	spl_image->load_addr = 0x80380000;
	spl_image->entry_point = 0x80380000;
	spl_image->size = 0;
	return 0;
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
