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
 * qcom_scan_fit_dtbs() - Scan for DTBs in FIT image
 * @fit_addr: Address of the FIT image (typically ramdisk location)
 *
 * Return: number of DTBs found
 */
int qcom_scan_fit_dtbs(ulong fit_addr);

/**
 * qcom_select_dtb_by_socinfo() - Select DTB matching socinfo
 * @soc_id: SoC ID from socinfo
 * @board_id: Board ID from socinfo
 *
 * Return: pointer to matching FDT, or NULL if not found
 */
void *qcom_select_dtb_by_socinfo(u32 soc_id, u32 board_id);

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

#endif /* __DTB_SELECT_H */