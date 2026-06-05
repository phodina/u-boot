/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Qualcomm Snapdragon DTB selection functions
 *
 * Copyright (c) 2026 Petr Hodina <petr.hodina@protonmail.com>
 */

#ifndef __DTB_SELECT_H
#define __DTB_SELECT_H

#include <linux/types.h>

/**
 * qcom_scan_appended_dtbs() - Scan for appended DTBs after U-Boot image
 * @start_addr: Address to start scanning from (typically end of U-Boot)
 * @max_size: Maximum size to scan
 *
 * Return: number of DTBs found
 */
int qcom_scan_appended_dtbs(ulong start_addr, size_t max_size);

/**
 * qcom_select_dtb_by_socinfo() - Select the best-matching DTB.
 * @soc_id:     SoC ID from socinfo (matches qcom,msm-id cell[0])
 * @hw_plat:    platform type from socinfo (low 8 bits of qcom,board-id cell[0])
 * @hw_subtype: hardware subtype from socinfo (high 8 bits of board-id cell[0])
 * @plat_ver:   packed platform major/minor from socinfo (major in bits 16..31)
 * @codename:   optional vendor-supplied device codename from the kernel
 *              cmdline (androidboot.project_codename=, e.g. "enchilada").
 *              When non-NULL, candidates whose `compatible` contains this
 *              token get a tiebreaker bonus. Pass NULL when unavailable.
 *
 * Decomposes the candidate DTB's qcom,board-id cell[0] into its four fields
 * (platform type, minor version, major version, hardware subtype) and scores
 * candidates against the runtime socinfo. Each DTB field is exact-match or
 * zero-wildcard; any non-wildcard mismatch eliminates the candidate.
 *
 * Return: pointer to matching FDT, or NULL if not found.
 */
void *qcom_select_dtb_by_socinfo(u32 soc_id, u32 hw_plat,
				 u32 hw_subtype, u32 plat_ver,
				 const char *codename);

/**
 * qcom_select_dtb_from_socinfo_and_cmdline() - Select DTB using socinfo and Android cmdline
 *
 * This function combines socinfo data and Android command line parameters
 * to select the most appropriate DTB from the scanned list.
 *
 * Return: pointer to matching FDT, or NULL if not found
 */
void *qcom_select_dtb_from_socinfo_and_cmdline(void);

/**
 * qcom_select_dtb_by_compatible_string() - Select DTB by compatible string match
 * @target_string: String to match against DTB compatible property
 *
 * Return: pointer to matching FDT, or NULL if not found
 */
void *qcom_select_dtb_by_compatible_string(const char *target_string);

/**
 * qcom_patch_dtb_with_abl_memory() - Copy /memory@... reg from ABL's
 * FDT into a writable copy of @selected. Required because upstream
 * sdm845-class DTs ship with a placeholder /memory@80000000 reg
 * (`<0 0x80000000 0 0>`) that ABL populates only on the DTB *it*
 * picks; if SPL hands full U-Boot a different DTB, the placeholder
 * remains and parse_memory panics on "No valid memory ranges found".
 *
 * Return: pointer to the patched DTB on success, or @selected unchanged
 *         on failure.
 */
void *qcom_patch_dtb_with_abl_memory(void *selected);

/**
 * qcom_list_scanned_dtbs() - List all scanned DTBs with their properties
 */
void qcom_list_scanned_dtbs(void);

/**
 * qcom_get_dtb_by_index() - Get DTB by index from scanned list
 * @index: Index of the DTB to retrieve
 *
 * Return: pointer to FDT, or NULL if index is invalid
 */
void *qcom_get_dtb_by_index(int index);

/**
 * qcom_get_dtb_count() - Get number of scanned DTBs
 *
 * Return: number of DTBs in the scanned list
 */
int qcom_get_dtb_count(void);

/**
 * qcom_get_android_bootargs() - Get the Android boot command line.
 *
 * Reads /chosen/bootargs from the device tree handed over by the prior
 * bootloader (typically ABL on Qualcomm), which is the only source
 * available in SPL. Falls back to env_get("bootargs") in full U-Boot
 * when no prior-bl FDT is present.
 *
 * Return: pointer to the bootargs string (lives in the FDT blob or env
 *         storage; do not free), or NULL if not available.
 */
const char *qcom_get_android_bootargs(void);

#endif /* __DTB_SELECT_H */