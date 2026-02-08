// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm socinfo driver for U-Boot
 *
 * Copyright (c) 2009-2017, The Linux Foundation. All rights reserved.
 * Copyright (c) 2017-2019, Linaro Ltd.
 * Copyright (c) 2026, Adapted for U-Boot
 */

#include <dm.h>
#include <errno.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/compat.h>
#include <linux/err.h>
#include <soc/qcom/smem.h>
#include <soc/qcom/socinfo.h>

static struct socinfo *socinfo_data;
static size_t socinfo_size;

int qcom_socinfo_init(void)
{
	void *info;
	size_t size;

	/* Read socinfo from SMEM */
	info = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_HW_SW_BUILD_ID, &size);
	if (IS_ERR(info)) {
		pr_err("Couldn't find socinfo in SMEM\n");
		return PTR_ERR(info);
	}

	socinfo_data = info;
	socinfo_size = size;

	pr_debug("Socinfo found: fmt=%u, id=%u, ver=%u.%u\n",
		 le32_to_cpu(socinfo_data->fmt),
		 le32_to_cpu(socinfo_data->id),
		 SOCINFO_MAJOR(le32_to_cpu(socinfo_data->ver)),
		 SOCINFO_MINOR(le32_to_cpu(socinfo_data->ver)));

	return 0;
}

u32 qcom_socinfo_get_id(void)
{
	if (!socinfo_data)
		return 0;

	return le32_to_cpu(socinfo_data->id);
}

u32 qcom_socinfo_get_hw_plat(void)
{
	if (!socinfo_data)
		return 0;

	/* hw_plat is available from version 3 onwards */
	if (le32_to_cpu(socinfo_data->fmt) < SOCINFO_VERSION(0, 3))
		return 0;

	return le32_to_cpu(socinfo_data->hw_plat);
}

u32 qcom_socinfo_get_hw_plat_subtype(void)
{
	if (!socinfo_data)
		return 0;

	/* hw_plat_subtype is available from version 6 onwards */
	if (le32_to_cpu(socinfo_data->fmt) < SOCINFO_VERSION(0, 6))
		return 0;

	return le32_to_cpu(socinfo_data->hw_plat_subtype);
}

u32 qcom_socinfo_get_plat_ver(void)
{
	if (!socinfo_data)
		return 0;

	/* plat_ver is available from version 4 onwards */
	if (le32_to_cpu(socinfo_data->fmt) < SOCINFO_VERSION(0, 4))
		return 0;

	return le32_to_cpu(socinfo_data->plat_ver);
}

u32 qcom_socinfo_get_serial_num(void)
{
	if (!socinfo_data)
		return 0;

	/* serial_num is available from version 10 onwards */
	if (le32_to_cpu(socinfo_data->fmt) < SOCINFO_VERSION(0, 10))
		return 0;

	return le32_to_cpu(socinfo_data->serial_num);
}

void qcom_socinfo_print(void)
{
	u32 soc_id, hw_plat, hw_plat_subtype;
	u32 version_major, version_minor;

	if (!socinfo_data) {
		printf("Socinfo: Not initialized\n");
		return;
	}

	soc_id = qcom_socinfo_get_id();
	hw_plat = qcom_socinfo_get_hw_plat();
	hw_plat_subtype = qcom_socinfo_get_hw_plat_subtype();
	version_major = SOCINFO_MAJOR(le32_to_cpu(socinfo_data->ver));
	version_minor = SOCINFO_MINOR(le32_to_cpu(socinfo_data->ver));

	printf("Socinfo:\n");
	printf("  SoC ID:          0x%x\n", soc_id);
	printf("  SoC version:     %u.%u\n", version_major, version_minor);
	printf("  HW Platform:     %u\n", hw_plat);
	printf("  HW Subtype:      %u\n", hw_plat_subtype);
	printf("  Format version:  %u\n", le32_to_cpu(socinfo_data->fmt));

	if (le32_to_cpu(socinfo_data->fmt) >= SOCINFO_VERSION(0, 1))
		printf("  Build ID:        %s\n", socinfo_data->build_id);

	if (le32_to_cpu(socinfo_data->fmt) >= SOCINFO_VERSION(0, 10)) {
		u32 serial = qcom_socinfo_get_serial_num();
		if (serial)
			printf("  Serial number:   0x%x\n", serial);
	}
}
