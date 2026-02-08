/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __QCOM_SMEM_H__
#define __QCOM_SMEM_H__

#include <linux/types.h>

#define QCOM_SMEM_HOST_ANY -1

bool qcom_smem_is_available(void);
void *qcom_smem_get(unsigned host, unsigned item, size_t *size);

/*
 * qcom_smem_init_early() - Set up SMEM access without DM/devicetree.
 * @base: physical base address of the main SMEM region
 * @size: size of the SMEM region in bytes
 *
 * Used by chainloaded SPL where the SMEM base cannot be discovered via DT
 * because DT selection itself depends on socinfo (which lives in SMEM).
 * Safe to call multiple times; subsequent calls are a no-op if a SMEM handle
 * is already populated (e.g. by DM probe).
 *
 * Return: 0 on success, negative errno on failure.
 */
int qcom_smem_init_early(phys_addr_t base, size_t size);

int qcom_smem_get_soc_id(u32 *id);
int qcom_smem_get_feature_code(u32 *code);

#endif
