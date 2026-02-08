// SPDX-License-Identifier: GPL-2.0+
/*
 * Runtime DTB selection for Qualcomm platforms
 * Parses appended DTBs and matches against socinfo board-id
 *
 * Author: Petr Hodina <petr.hodina@protonmail.com>
 */

#include <common.h>
#include <fdt_support.h>
#include <libfdt.h>
#include <linux/libfdt.h>
#include <soc/qcom/socinfo.h>
#include <cmdline_utils.h>
#include <env.h>
#include <asm/io.h>
#include <malloc.h>
#include <image.h>

#define FDT_MAGIC_SIZE 4

#ifndef CONFIG_QCOM_MAX_DTBS
#define CONFIG_QCOM_MAX_DTBS 128
#endif

struct dtb_entry {
	void *fdt;
	u32 soc_id;
	u32 board_id;
	u32 board_rev;
	size_t size;
	const char *compatible;
};

static struct dtb_entry dtb_list[CONFIG_QCOM_MAX_DTBS];
static int dtb_count = 0;

static int qcom_parse_dtb(const void *fdt, u32 *soc_id, u32 *board_id, u32 *board_rev)
{
	const fdt32_t *prop;
	int len;
	int offset;

	offset = fdt_path_offset(fdt, "/");
	if (offset < 0)
		return -EINVAL;

	prop = fdt_getprop(fdt, offset, "qcom,board-id", &len);
	if (!prop || len < 8)
		return -ENOENT;

	*board_id = fdt32_to_cpu(prop[0]);
	*board_rev = fdt32_to_cpu(prop[1]);

	prop = fdt_getprop(fdt, offset, "qcom,msm-id", &len);
	if (!prop || len < 4)
		return -ENOENT;

	*soc_id = fdt32_to_cpu(prop[0]);

	return 0;
}

/**
 * qcom_scan_appended_dtbs() - Scan for appended DTBs after U-Boot image
 *
 * @start_addr: Address to start scanning from (typically end of U-Boot)
 * @max_size: Maximum size to scan
 *
 * Return: number of DTBs found
 */
int qcom_scan_appended_dtbs(ulong start_addr, size_t max_size)
{
	ulong addr = start_addr;
	ulong end_addr = start_addr + max_size;
	void *fdt;
	int ret;
	u32 soc_id, board_id, board_rev;
	const char *compatible;

	dtb_count = 0;

	printf("Scanning for appended DTBs from 0x%lx to 0x%lx...\n",
	       start_addr, end_addr);

	/* Scan for FDT magic (0xd00dfeed) */
	while (addr < end_addr && dtb_count < CONFIG_QCOM_MAX_DTBS) {
		addr = ALIGN(addr, 4);

		fdt = (void *)addr;

		if (fdt_check_header(fdt) == 0) {
			size_t fdt_size = fdt_totalsize(fdt);

			if (fdt_size > 0 && fdt_size < SZ_1M) {

				ret = qcom_parse_dtb(fdt, &soc_id, &board_id, &board_rev);
				if (ret < 0) {
					debug("DTB at 0x%lx: no qcom,board-id\n", addr);
					board_id = 0;
					board_rev = 0;
					soc_id = 0;
				}
				}

				compatible = fdt_getprop(fdt, 0, "compatible", NULL);

				dtb_list[dtb_count].fdt = fdt;
				dtb_list[dtb_count].soc_id = soc_id;
				dtb_list[dtb_count].board_id = board_id;
				dtb_list[dtb_count].board_rev = board_rev;
				dtb_list[dtb_count].size = fdt_size;
				dtb_list[dtb_count].compatible = compatible;

				printf("  [%d] DTB at 0x%lx: soc_id=0x%x board_id=%u rev=%u size=%zu\n",
				       dtb_count, addr, soc_id, board_id, board_rev, fdt_size);
				if (compatible)
					printf("      Compatible: %s\n", compatible);

				dtb_count++;

				addr += fdt_size;
			} else {
				addr += 4;
			}
		} else {
			addr += 4;
		}

		if (addr <= (ulong)fdt)
			break;
	}

	printf("Found %d DTB(s)\n", dtb_count);
	return dtb_count;
}

/**
 * qcom_scan_fit_dtbs() - Scan for DTBs in FIT image
 *
 * @fit_addr: Address of the FIT image (typically ramdisk location)
 *
 * Return: number of DTBs found
 */
int qcom_scan_fit_dtbs(ulong fit_addr)
{
	const void *fit = (const void *)fit_addr;
	int images_noffset, noffset;
	const char *fit_uname;
	int fit_uname_len;
	int ndepth;
	int count = 0;
	u32 soc_id, board_id, board_rev;
	int ret;
	const char *compatible;

	dtb_count = 0;

	if (!fit_addr) {
		printf("No FIT image address provided\n");
		return 0;
	}

	if (fdt_check_header(fit)) {
		printf("Bad FIT image header at 0x%lx\n", fit_addr);
		return 0;
	}

	printf("Scanning FIT image at 0x%lx for DTBs...\n", fit_addr);

	/* Find the images parent node */
	images_noffset = fdt_path_offset(fit, FIT_IMAGES_PATH);
	if (images_noffset < 0) {
		printf("Can't find images parent node '%s' (%s)\n",
		       FIT_IMAGES_PATH, fdt_strerror(images_noffset));
		return 0;
	}

	/* Iterate over all images in FIT */
	for (ndepth = 0, count = 0,
	     noffset = fdt_next_node(fit, images_noffset, &ndepth);
	     (noffset >= 0) && (ndepth > 0) && (count < CONFIG_QCOM_MAX_DTBS);
	     noffset = fdt_next_node(fit, noffset, &ndepth)) {
		if (ndepth == 1) {
			/*
			 * Direct child node of the images parent node,
			 * i.e. component image node.
			 */
			fit_uname = fit_get_name(fit, noffset, &fit_uname_len);
			if (!fit_uname) {
				printf("Can't get node name\n");
				continue;
			}

			/* Check if this is a fdt image */
			if (fit_image_check_type(fit, noffset, IH_TYPE_FLATDT)) {
				void *fdt_data;
				size_t fdt_len;

				/* Get FDT data */
				ret = fit_image_get_data(fit, noffset, (const void **)&fdt_data, &fdt_len);
				if (ret) {
					printf("Can't get FDT data for '%s': %d\n", fit_uname, ret);
					continue;
				}

				/* Verify FDT header */
				if (fdt_check_header(fdt_data)) {
					printf("Bad FDT header for '%s'\n", fit_uname);
					continue;
				}

				/* Parse DTB for Qcom properties */
				ret = qcom_parse_dtb(fdt_data, &soc_id, &board_id, &board_rev);
				if (ret < 0) {
					debug("FIT DTB '%s': no qcom,board-id\n", fit_uname);
					soc_id = 0;
					board_id = 0;
					board_rev = 0;
				}

				compatible = fdt_getprop(fdt_data, 0, "compatible", NULL);

				/* Add to DTB list */
				dtb_list[dtb_count].fdt = fdt_data;
				dtb_list[dtb_count].soc_id = soc_id;
				dtb_list[dtb_count].board_id = board_id;
				dtb_list[dtb_count].board_rev = board_rev;
				dtb_list[dtb_count].size = fdt_len;
				dtb_list[dtb_count].compatible = compatible;

				printf("  [%d] FIT DTB '%s': soc_id=0x%x board_id=%u rev=%u size=%zu\n",
				       dtb_count, fit_uname, soc_id, board_id, board_rev, fdt_len);
				if (compatible)
					printf("      Compatible: %s\n", compatible);

				dtb_count++;
			}
		}
	}

	printf("Found %d DTB(s) in FIT image\n", dtb_count);
	return dtb_count;
}

/**
 * qcom_select_dtb_by_socinfo() - Select DTB matching socinfo
 *
 * @soc_id: SoC ID from socinfo
 * @board_id: Board ID from socinfo
 *
 * Return: pointer to matching FDT, or NULL if not found
 */
void *qcom_select_dtb_by_socinfo(u32 soc_id, u32 board_id)
{
	int i;
	void *best_match = NULL;
	u32 best_score = 0;

	printf("\nMatching DTBs for: soc_id=0x%x board_id=%u\n", soc_id, board_id);

	for (i = 0; i < dtb_count; i++) {
		u32 score = 0;

		/* SoC ID must match (or DTB has wildcard soc_id=0) */
		if (dtb_list[i].soc_id != 0 && dtb_list[i].soc_id != soc_id)
			continue;

		if (dtb_list[i].soc_id == soc_id)
			score += 10;

		if (dtb_list[i].board_id == board_id)
			score += 100;

		printf("  [%d] score=%u (soc=0x%x board=%u)\n",
		       i, score, dtb_list[i].soc_id, dtb_list[i].board_id);

		if (score > best_score) {
			best_score = score;
			best_match = dtb_list[i].fdt;
		}
	}

	if (best_match) {
		printf("Selected DTB with score %u\n", best_score);
	} else {
		printf("No matching DTB found!\n");
	}

	return best_match;
}

/**
 * qcom_select_dtb_from_socinfo_and_cmdline() - Select DTB using socinfo and Android cmdline
 *
 * This function combines socinfo data and Android command line parameters
 * to select the most appropriate DTB from the scanned list.
 *
 * Return: pointer to matching FDT, or NULL if not found
 */
void *qcom_select_dtb_from_socinfo_and_cmdline(void)
{
	struct androidboot_params boot_params;
	const char *bootargs;
	u32 soc_id = 0, hw_plat = 0, hw_subtype = 0;
	int ret;
	void *selected_dtb = NULL;
	int dtb_count = 0;

	/* Get socinfo data using getter functions */
	soc_id = qcom_socinfo_get_id();
	hw_plat = qcom_socinfo_get_hw_plat();
	hw_subtype = qcom_socinfo_get_hw_plat_subtype();

	if (soc_id != 0) {
		printf("Socinfo: soc_id=0x%x hw_plat=%u hw_subtype=%u\n",
		       soc_id, hw_plat, hw_subtype);
	} else {
		printf("Warning: No socinfo available or not initialized\n");
	}

	/* Scan for DTBs based on configuration */
#ifdef CONFIG_SPL_QCOM_DTB_SELECTION_SOURCE
	/* DTBs are appended after U-Boot binary */
	ulong dtb_scan_start = CONFIG_SYS_TEXT_BASE + 0x100000;
	dtb_count = qcom_scan_appended_dtbs(dtb_scan_start, SZ_4M);
#else
	/* DTBs are in FIT image (ramdisk location) */
	const char *ramdisk_addr_str = env_get("ramdisk_addr_r");
	ulong fit_addr = 0;

	if (ramdisk_addr_str) {
		fit_addr = simple_strtoul(ramdisk_addr_str, NULL, 16);
	} else {
		/* Fallback to common ramdisk load address */
		fit_addr = CONFIG_SYS_LOAD_ADDR + 0x2000000; /* +32MB */
		printf("No ramdisk_addr_r found, trying 0x%lx\n", fit_addr);
	}

	if (fit_addr) {
		dtb_count = qcom_scan_fit_dtbs(fit_addr);
	}
#endif

	if (dtb_count == 0) {
		printf("No DTBs found for selection\n");
		goto parse_cmdline;
	}

	printf("Found %d DTB(s), proceeding with selection\n", dtb_count);

parse_cmdline:
	/* Parse Android boot parameters */
	memset(&boot_params, 0, sizeof(boot_params));
	bootargs = env_get("bootargs");
	if (bootargs) {
		ret = parse_androidboot_params(bootargs, &boot_params);
		if (ret == 0) {
			printf("Android boot params:\n");
			if (boot_params.hardware)
				printf("  Hardware: %s\n", boot_params.hardware);
			if (boot_params.hardware_platform)
				printf("  Platform: %s\n", boot_params.hardware_platform);
			if (boot_params.hardware_sku)
				printf("  SKU: %s\n", boot_params.hardware_sku);
			if (boot_params.revision)
				printf("  Revision: %s\n", boot_params.revision);
		} else {
			printf("Warning: Failed to parse Android boot parameters\n");
		}
	} else {
		printf("Warning: No bootargs available\n");
	}

	/* Try to match DTB using socinfo first */
	if (soc_id) {
		selected_dtb = qcom_select_dtb_by_socinfo(soc_id, hw_plat);
	}

	/* If no DTB found and we have Android hardware info, try string matching */
	if (!selected_dtb && boot_params.hardware) {
		selected_dtb = qcom_select_dtb_by_compatible_string(boot_params.hardware);
	}

	/* If still no match, try platform string */
	if (!selected_dtb && boot_params.hardware_platform) {
		selected_dtb = qcom_select_dtb_by_compatible_string(boot_params.hardware_platform);
	}

	/* Clean up Android boot parameters */
	if (bootargs)
		free_androidboot_params(&boot_params);

	return selected_dtb;
}

/**
 * qcom_select_dtb_by_compatible_string() - Select DTB by compatible string match
 *
 * @target_string: String to match against DTB compatible property
 *
 * Return: pointer to matching FDT, or NULL if not found
 */
void *qcom_select_dtb_by_compatible_string(const char *target_string)
{
	int i;
	void *best_match = NULL;

	if (!target_string)
		return NULL;

	printf("\nMatching DTBs by compatible string: '%s'\n", target_string);

	for (i = 0; i < dtb_count; i++) {
		if (dtb_list[i].compatible &&
		    strstr(dtb_list[i].compatible, target_string)) {
			printf("  [%d] Match found: %s\n", i, dtb_list[i].compatible);
			best_match = dtb_list[i].fdt;
			break;
		}
	}

	if (best_match) {
		printf("Selected DTB by compatible string match\n");
	} else {
		printf("No DTB found matching compatible string '%s'\n", target_string);
	}

	return best_match;
}

void qcom_list_scanned_dtbs(void)
{
	int i;

	if (dtb_count == 0) {
		printf("No DTBs have been scanned yet\n");
		return;
	}

	printf("\nScanned DTBs:\n");
	printf("%-5s %-10s %-10s %-10s %-10s %s\n",
	       "Index", "Address", "SoC ID", "Board ID", "Rev", "Size");
	printf("----------------------------------------------------------------\n");

	for (i = 0; i < dtb_count; i++) {
		printf("%-5d 0x%-8lx 0x%-8x %-10u %-10u %-10zu\n",
		       i,
		       (ulong)dtb_list[i].fdt,
		       dtb_list[i].soc_id,
		       dtb_list[i].board_id,
		       dtb_list[i].board_rev,
		       dtb_list[i].size);
		if (dtb_list[i].compatible)
			printf("      %s\n", dtb_list[i].compatible);
	}
}

void *qcom_get_dtb_by_index(int index)
{
	if (index < 0 || index >= dtb_count)
		return NULL;

	return dtb_list[index].fdt;
}

int qcom_get_dtb_count(void)
{
	return dtb_count;
}
