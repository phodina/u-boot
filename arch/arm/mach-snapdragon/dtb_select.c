// SPDX-License-Identifier: GPL-2.0+
/*
 * Runtime DTB selection for Qualcomm platforms
 * Parses appended DTBs and matches against socinfo board-id
 *
 * Author: Petr Hodina <petr.hodina@protonmail.com>
 */

#include <config.h>
#include <fdt_support.h>
#include <init.h>
#include <libfdt.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include <asm-generic/sections.h>
#include <soc/qcom/socinfo.h>
#include <cmdline_utils.h>
#include <dtb_select.h>
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
	const char *panel_compatible;
};

/*
 * Vendor-prefix substrings used to recognise panel/display compatible
 * strings inside a flattened DT. The list is intentionally short and
 * targets the prefixes actually used by upstream sdm845-class panels;
 * extend as new SoCs are added.
 */
static const char * const qcom_panel_vendor_prefixes[] = {
	"novatek,",
	"ebbg,",
	"tianma,",
	"samsung,s6e",
	"sharp,ls",
	"boe,",
};

/**
 * qcom_find_panel_compat() - Walk a DT and return the first compatible
 * string that looks like a panel.
 *
 * Returns a pointer into the DT (still owned by caller) or NULL.
 */
static const char *qcom_find_panel_compat(const void *fdt)
{
	int offset = -1;
	int len;
	const char *compat;
	int i;

	if (!fdt || fdt_check_header(fdt))
		return NULL;

	while ((offset = fdt_next_node(fdt, offset, NULL)) >= 0) {
		compat = fdt_getprop(fdt, offset, "compatible", &len);
		if (!compat || len <= 0)
			continue;

		for (i = 0; i < ARRAY_SIZE(qcom_panel_vendor_prefixes); i++) {
			if (strstr(compat, qcom_panel_vendor_prefixes[i]))
				return compat;
		}
	}

	return NULL;
}

static struct dtb_entry dtb_list[CONFIG_QCOM_MAX_DTBS];
static int dtb_count = 0;

const char *qcom_get_android_bootargs(void)
{
	const void *fdt;
	phys_addr_t fdt_addr;
	const char *bootargs;
	int node;

	/*
	 * Primary source: the FDT passed by the prior bootloader (ABL on
	 * Qualcomm). ABL builds /chosen/bootargs from the boot.img command
	 * line and any bootconfig fragments, then hands the assembled DT
	 * to us in x0. save_boot_params() captures that pointer.
	 *
	 * This works in both SPL and full U-Boot, and is the only source
	 * available in SPL (no env, no DT chosen processing yet).
	 */
	fdt_addr = get_prev_bl_fdt_addr();
	if (fdt_addr) {
		fdt = (const void *)(uintptr_t)fdt_addr;
		if (!fdt_check_header(fdt)) {
			node = fdt_path_offset(fdt, "/chosen");
			if (node >= 0) {
				bootargs = fdt_getprop(fdt, node, "bootargs",
						       NULL);
				if (bootargs && *bootargs)
					return bootargs;
			}
		}
	}

#ifndef CONFIG_XPL_BUILD
	/*
	 * Fallback in full U-Boot: env may have been populated from the
	 * /chosen/bootargs node by board-level setup, or by the user.
	 */
	return env_get("bootargs");
#else
	return NULL;
#endif
}

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

				compatible = fdt_getprop(fdt, 0, "compatible", NULL);

				dtb_list[dtb_count].fdt = fdt;
				dtb_list[dtb_count].soc_id = soc_id;
				dtb_list[dtb_count].board_id = board_id;
				dtb_list[dtb_count].board_rev = board_rev;
				dtb_list[dtb_count].size = fdt_size;
				dtb_list[dtb_count].compatible = compatible;
				dtb_list[dtb_count].panel_compatible =
					qcom_find_panel_compat(fdt);

				printf("  [%d] DTB at 0x%lx: soc_id=0x%x board_id=%u rev=%u size=%zu\n",
				       dtb_count, addr, soc_id, board_id, board_rev, fdt_size);
				if (compatible)
					printf("      Compatible: %s\n", compatible);
				if (dtb_list[dtb_count].panel_compatible)
					printf("      Panel:      %s\n",
					       dtb_list[dtb_count].panel_compatible);

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


/*
 * Layout of qcom,board-id cell[0] as packed by the QC dtbtool / mkdtboimg:
 *
 *   bits  0..7  : platform type    (matches socinfo.hw_plat:    MTP=8, CDP=9,
 *                                   QRD=10, HDK=0x29, ...)
 *   bits  8..15 : platform minor   (= socinfo.plat_ver & 0xff)
 *   bits 16..23 : platform major   (= (socinfo.plat_ver >> 16) & 0xff)
 *   bits 24..31 : hardware subtype (matches socinfo.hw_plat_subtype)
 *
 * Each field in the DTB is either an exact value or zero, where zero acts as
 * a wildcard that accepts any runtime value. cell[1] of qcom,board-id encodes
 * PMIC info and is intentionally ignored here -- we have no PMIC info from
 * socinfo at this stage.
 */
#define QCOM_BOARDID_PLAT_TYPE(b)	((b) & 0xff)
#define QCOM_BOARDID_PLAT_MINOR(b)	(((b) >> 8) & 0xff)
#define QCOM_BOARDID_PLAT_MAJOR(b)	(((b) >> 16) & 0xff)
#define QCOM_BOARDID_HW_SUBTYPE(b)	(((b) >> 24) & 0xff)

/**
 * qcom_select_dtb_by_socinfo() - Select the best-matching DTB.
 *
 * @soc_id:      SoC ID (matches qcom,msm-id cell[0])
 * @hw_plat:     platform type from socinfo
 * @hw_subtype:  hardware subtype from socinfo
 * @plat_ver:    packed platform major/minor from socinfo (major in bits 16..31)
 *
 * Matching rules per qcom,board-id cell[0] field: each field must either
 * equal the runtime value or be zero (wildcard). DTBs where any non-wildcard
 * field disagrees are rejected. The remaining candidates are scored by how
 * many fields match exactly (non-wildcard); higher is better.
 *
 * Return: pointer to the best-matching FDT, or NULL if nothing matches.
 */
void *qcom_select_dtb_by_socinfo(u32 soc_id, u32 hw_plat,
				 u32 hw_subtype, u32 plat_ver)
{
	const u32 plat_minor = plat_ver & 0xff;
	const u32 plat_major = (plat_ver >> 16) & 0xff;
	void *best_match = NULL;
	u32 best_score = 0;
	const char *abl_panel = NULL;
	const char *abl_first_compat = NULL;
	phys_addr_t abl_fdt_addr;
	int i;

	abl_fdt_addr = get_prev_bl_fdt_addr();
	if (abl_fdt_addr) {
		const void *abl_fdt = (const void *)(uintptr_t)abl_fdt_addr;

		abl_panel = qcom_find_panel_compat(abl_fdt);
		if (!fdt_check_header(abl_fdt))
			abl_first_compat = fdt_getprop(abl_fdt, 0, "compatible", NULL);
	}
	if (abl_panel)
		printf("ABL panel compatible: %s\n", abl_panel);
	if (abl_first_compat)
		printf("ABL FDT first compatible: %s\n", abl_first_compat);

	printf("\nMatching DTBs against: soc=0x%x plat=%u subtype=%u ver=%u.%u\n",
	       soc_id, hw_plat, hw_subtype, plat_major, plat_minor);

	for (i = 0; i < dtb_count; i++) {
		u32 dtb_bid = dtb_list[i].board_id;
		u32 dtb_plat   = QCOM_BOARDID_PLAT_TYPE(dtb_bid);
		u32 dtb_minor  = QCOM_BOARDID_PLAT_MINOR(dtb_bid);
		u32 dtb_major  = QCOM_BOARDID_PLAT_MAJOR(dtb_bid);
		u32 dtb_subtyp = QCOM_BOARDID_HW_SUBTYPE(dtb_bid);
		u32 score = 0;

		/* SoC ID: must match, no wildcards. (DT cell is always set.) */
		if (dtb_list[i].soc_id != soc_id)
			continue;
		score += 1000;

		/* Platform type: must match unless DT says 0. */
		if (dtb_plat && dtb_plat != hw_plat)
			continue;
		if (dtb_plat == hw_plat)
			score += 500;

		/* Hardware subtype: must match unless DT says 0. */
		if (dtb_subtyp && dtb_subtyp != hw_subtype)
			continue;
		if (dtb_subtyp == hw_subtype)
			score += 200;

		/* Platform major/minor: must match unless DT says 0. */
		if (dtb_major && dtb_major != plat_major)
			continue;
		if (dtb_major == plat_major)
			score += 50;

		if (dtb_minor && dtb_minor != plat_minor)
			continue;
		if (dtb_minor == plat_minor)
			score += 25;

		/*
		 * Display panel tiebreaker. Two DTBs can declare identical
		 * qcom,board-id values (e.g. Xiaomi Poco F1 EBBG vs Tianma
		 * panel variants, both board_id=69) and only differ in the
		 * display panel. ABL probes the panel and bakes its compatible
		 * into the FDT it hands us in x0; bonus-score any candidate
		 * whose own panel compatible matches.
		 */
		if (abl_panel && dtb_list[i].panel_compatible &&
		    !strcmp(abl_panel, dtb_list[i].panel_compatible))
			score += 300;

		/*
		 * ABL-FDT top-compatible tiebreaker. Boards like OP6, OP6T, and
		 * Sony XZ3 all declare qcom,board-id plat=8 subtype=0 in upstream
		 * DT, so socinfo cannot disambiguate them. When the boot.img author
		 * packaged a device-native DTB in the v2 --dtb section, ABL hands
		 * us that DTB and its top-level `compatible` (e.g. "oneplus,enchilada")
		 * identifies the device unambiguously. Bonus the candidate whose
		 * first compatible matches. Kept smaller than +500 (plat) so that
		 * socinfo still trumps a misleading v2 --dtb section (e.g. the
		 * universal sdm845 boot.img uses blueline as the section even when
		 * booting on a OnePlus 6).
		 */
		if (abl_first_compat && dtb_list[i].compatible &&
		    !strcmp(abl_first_compat, dtb_list[i].compatible))
			score += 400;

		printf("  [%d] score=%u (board_id=0x%08x: plat=%u subtype=%u ver=%u.%u)\n",
		       i, score, dtb_bid, dtb_plat, dtb_subtyp,
		       dtb_major, dtb_minor);

		if (score > best_score) {
			best_score = score;
			best_match = dtb_list[i].fdt;
		}
	}

	if (best_match)
		printf("Selected DTB with score %u\n", best_score);
	else
		printf("No matching DTB found\n");

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
	u32 soc_id = 0, hw_plat = 0, hw_subtype = 0, plat_ver = 0;
	int ret;
	void *selected_dtb = NULL;
	int dtb_count = 0;

	/* Get socinfo data using getter functions */
	soc_id = qcom_socinfo_get_id();
	hw_plat = qcom_socinfo_get_hw_plat();
	hw_subtype = qcom_socinfo_get_hw_plat_subtype();
	plat_ver = qcom_socinfo_get_plat_ver();

	if (soc_id != 0) {
		printf("Socinfo: soc_id=0x%x hw_plat=%u hw_subtype=%u plat_ver=%u.%u\n",
		       soc_id, hw_plat, hw_subtype,
		       (plat_ver >> 16) & 0xff, plat_ver & 0xff);
	} else {
		printf("Warning: No socinfo available or not initialized\n");
	}

	/*
	 * On Qualcomm ABL the kernel section is the only thing reliably
	 * loaded to a known address (0x80080000 with our packaging).
	 * We pad the SPL binary to a 1 MiB boundary and append candidate
	 * DTBs after it, then gzip the whole thing. After ABL's gzip
	 * decompression, candidates are at SPL_load + 1 MiB. Scan from
	 * the known fixed address, skipping any spurious FDT-magic
	 * patterns inside the SPL binary itself.
	 */
	ulong dtb_scan_start = 0x80180000;
	dtb_count = qcom_scan_appended_dtbs(dtb_scan_start, SZ_4M);

	if (dtb_count == 0) {
		printf("No DTBs found for selection\n");
		goto parse_cmdline;
	}

	printf("Found %d DTB(s), proceeding with selection\n", dtb_count);

parse_cmdline:
	/* Parse Android boot parameters */
	memset(&boot_params, 0, sizeof(boot_params));
	bootargs = qcom_get_android_bootargs();
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

	/*
	 * Primary matcher: socinfo (with panel-compat tiebreaker for boards
	 * that share a board_id — see qcom_select_dtb_by_socinfo). Socinfo
	 * is read from on-chip non-volatile data and reflects the running
	 * hardware unambiguously, which makes it the only reliable signal
	 * when the boot.img author chose a *non-native* DTB for the v2
	 * `--dtb` section (e.g. the universal sdm845 boot.img uses
	 * sdm845-google-blueline.dtb because Pixel 3's ABL is the strictest
	 * verifier — but that DTB then becomes the "ABL-provided FDT" on
	 * every device that runs the image, including OnePlus 6, SHIFT,
	 * etc., misleading any matcher that trusts ABL FDT's compatible).
	 */
	if (soc_id) {
		selected_dtb = qcom_select_dtb_by_socinfo(soc_id, hw_plat,
							  hw_subtype, plat_ver);
	}

	/*
	 * Fallback: ABL-provided FDT compatible. Only useful when ABL
	 * really did select a board-native DTB from its dtbo_a partition
	 * and socinfo above didn't yield a match (rare — e.g. unknown
	 * SoC ID).
	 */
	if (!selected_dtb) {
		phys_addr_t prev_fdt = get_prev_bl_fdt_addr();
		const void *prev;
		const char *prev_compat;
		int len;

		if (prev_fdt) {
			prev = (const void *)(uintptr_t)prev_fdt;
			if (!fdt_check_header(prev)) {
				prev_compat = fdt_getprop(prev, 0, "compatible", &len);
				if (prev_compat && len > 0) {
					printf("ABL-provided FDT compatible: %s\n",
					       prev_compat);
					selected_dtb =
						qcom_select_dtb_by_compatible_string(prev_compat);
				}
			}
		}
	}

	/* Fall back to androidboot.hardware string */
	if (!selected_dtb && boot_params.hardware) {
		selected_dtb = qcom_select_dtb_by_compatible_string(boot_params.hardware);
	}

	/* Fall back to androidboot.hardware.platform */
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

/*
 * Find a memory node by trying several conventions in order:
 *   1. `device_type = "memory"` (canonical, used by Linux/ABL)
 *   2. Top-level node whose name starts with "memory" (covers
 *      `/memory@80000000` and bare `/memory`).
 * ABL on different Qualcomm SoCs uses different conventions, so we
 * accept any of these.
 */
static int qcom_fdt_find_memory_node(const void *fdt)
{
	int offset = -1;
	const char *type;
	int tlen;

	/* Pass 1: device_type = "memory" */
	while ((offset = fdt_next_node(fdt, offset, NULL)) >= 0) {
		type = fdt_getprop(fdt, offset, "device_type", &tlen);
		if (type && tlen > 0 && !strcmp(type, "memory"))
			return offset;
	}

	/* Pass 2: top-level node named memory* */
	offset = -1;
	while ((offset = fdt_next_subnode(fdt, offset)) >= 0) {
		const char *name = fdt_get_name(fdt, offset, NULL);

		if (name && !strncmp(name, "memory", 6))
			return offset;
	}

	return -ENOENT;
}

/*
 * Static scratch buffer for the patched device tree we hand to full
 * U-Boot. The selected DTB still lives in the gzip-decompressed image
 * region; copying it here gives us headroom to grow the /memory@...
 * reg property (ABL typically reports more banks than the upstream
 * placeholder reserves) without risk of stomping on adjacent DTBs.
 */
#define QCOM_PATCHED_FDT_SIZE	SZ_256K
static u8 qcom_patched_fdt_buf[QCOM_PATCHED_FDT_SIZE] __aligned(8);

/**
 * qcom_patch_dtb_with_abl_memory() - Copy /memory@... reg from ABL's FDT
 * into a writable copy of the caller's DTB.
 *
 * @selected: pointer to the SPL-chosen DTB (read-only)
 *
 * Returns a pointer to a patched DTB on success, or @selected unchanged
 * if patching fails (caller still works, just may panic on memory
 * detection in full U-Boot).
 */
void *qcom_patch_dtb_with_abl_memory(void *selected)
{
	const void *abl_fdt;
	phys_addr_t abl_addr;
	void *out = qcom_patched_fdt_buf;
	int src_off, dst_off, len, ret;
	const void *abl_reg;

	abl_addr = get_prev_bl_fdt_addr();
	if (!abl_addr)
		return selected;
	abl_fdt = (const void *)(uintptr_t)abl_addr;
	if (fdt_check_header(abl_fdt))
		return selected;

	src_off = qcom_fdt_find_memory_node(abl_fdt);
	if (src_off < 0) {
		printf("ABL FDT: no memory node found\n");
		return selected;
	}
	printf("ABL FDT memory node: %s\n",
	       fdt_get_name(abl_fdt, src_off, NULL));
	abl_reg = fdt_getprop(abl_fdt, src_off, "reg", &len);
	if (!abl_reg || len <= 0) {
		printf("ABL FDT: /memory@... has no reg\n");
		return selected;
	}

	ret = fdt_open_into(selected, out, QCOM_PATCHED_FDT_SIZE);
	if (ret) {
		printf("fdt_open_into failed: %d\n", ret);
		return selected;
	}

	dst_off = qcom_fdt_find_memory_node(out);
	if (dst_off < 0) {
		printf("Selected DTB: no memory node to patch\n");
		return selected;
	}
	ret = fdt_setprop(out, dst_off, "reg", abl_reg, len);
	if (ret) {
		printf("fdt_setprop(reg) failed: %d\n", ret);
		return selected;
	}

	printf("Patched /memory@... reg (%d bytes) from ABL FDT\n", len);
	return out;
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
