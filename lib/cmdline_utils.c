// SPDX-License-Identifier: GPL-2.0+
/*
 * Command line parsing utilities
 *
 * Copyright (C) 2026 Petr Hodina
 */

#include <cmdline_utils.h>
#include <malloc.h>
#include <string.h>
#include <errno.h>
#include <linux/kernel.h>

/**
 * parse_cmdline_params() - Extract multiple parameters from a command line string
 */
int parse_cmdline_params(const char *cmdline, struct cmdline_param *params, int count)
{
	int i;
	const char *p, *p_end;
	int key_len, val_len;
	char *key_with_equals;

	if (!cmdline || !params || count <= 0)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		params[i].found = false;
		params[i].value = NULL;
	}

	for (i = 0; i < count; i++) {
		if (!params[i].key)
			continue;

		key_len = strlen(params[i].key);
		if (params[i].key[key_len - 1] == '=') {
			p = strstr(cmdline, params[i].key);
			if (!p)
				continue;
			p += key_len;
		} else {
			key_with_equals = malloc(key_len + 2);
			if (!key_with_equals)
				goto cleanup_error;

			strcpy(key_with_equals, params[i].key);
			strcat(key_with_equals, "=");

			p = strstr(cmdline, key_with_equals);
			free(key_with_equals);

			if (!p)
				continue;
			p += key_len + 1;
		}

		p_end = strstr(p, " ");
		if (!p_end) {
			val_len = strlen(p);
		} else {
			val_len = p_end - p;
		}

		params[i].value = malloc(val_len + 1);
		if (!params[i].value)
			goto cleanup_error;

		strncpy(params[i].value, p, val_len);
		params[i].value[val_len] = '\0';
		params[i].found = true;
	}

	return 0;

cleanup_error:
	for (i = 0; i < count; i++) {
		if (params[i].value) {
			free(params[i].value);
			params[i].value = NULL;
		}
		params[i].found = false;
	}
	return -ENOMEM;
}

/**
 * mask_cmdline_params() - Mask or replace selected parameters in a command line
 */
char *mask_cmdline_params(const char *cmdline, struct cmdline_mask_param *params, int count)
{
	char *result, *temp;
	const char *src_pos;
	char *dst_pos;
	int i, result_len, cmdline_len;
	const char *param_start, *param_end, *value_start, *value_end;
	char *key_with_equals;
	int key_len, old_value_len, new_value_len;
	bool found_match;

	if (!cmdline || !params || count <= 0)
		return NULL;

	cmdline_len = strlen(cmdline);
	result_len = cmdline_len * 2;
	result = malloc(result_len + 1);
	if (!result)
		return NULL;

	src_pos = cmdline;
	dst_pos = result;

	while (*src_pos) {
		found_match = false;

		for (i = 0; i < count; i++) {
			if (!params[i].key)
				continue;

			key_len = strlen(params[i].key);
			if (params[i].key[key_len - 1] == '=') {
				key_with_equals = (char *)params[i].key;
			} else {
				key_with_equals = malloc(key_len + 2);
				if (!key_with_equals) {
					free(result);
					return NULL;
				}
				strcpy(key_with_equals, params[i].key);
				strcat(key_with_equals, "=");
			}

			if (strncmp(src_pos, key_with_equals, strlen(key_with_equals)) == 0 &&
			    (src_pos == cmdline || *(src_pos - 1) == ' ')) {
				found_match = true;
				param_start = src_pos;
				value_start = src_pos + strlen(key_with_equals);

				value_end = strstr(value_start, " ");
				if (!value_end)
					value_end = value_start + strlen(value_start);
				param_end = value_end;

				old_value_len = value_end - value_start;

				if (!params[i].new_value) {
					if (*param_end == ' ')
						src_pos++;
				} else {
					strncpy(dst_pos, param_start, strlen(key_with_equals));
					dst_pos += strlen(key_with_equals);

					new_value_len = strlen(params[i].new_value);
					strcpy(dst_pos, params[i].new_value);
					dst_pos += new_value_len;
				}

				src_pos = param_end;

				if (key_with_equals != params[i].key)
					free(key_with_equals);
				break;
			}

			if (key_with_equals != params[i].key)
				free(key_with_equals);
		}

		if (!found_match) {
			*dst_pos++ = *src_pos++;
		}

		if (dst_pos - result >= result_len - 10) {
			int current_pos = dst_pos - result;
			result_len *= 2;
			temp = realloc(result, result_len + 1);
			if (!temp) {
				free(result);
				return NULL;
			}
			result = temp;
			dst_pos = result + current_pos;
		}
	}

	*dst_pos = '\0';
	return result;
}

/**
 * parse_androidboot_params() - Parse Android boot parameters from command line
 */
int parse_androidboot_params(const char *cmdline, struct androidboot_params *params)
{
	struct cmdline_param cmdline_params[] = {
		{"androidboot.hardware.ddr", NULL, 0},
		{"androidboot.ddr_info", NULL, 0},
		{"androidboot.ddr_size", NULL, 0},
		{"msm_drm.dsi_display0", NULL, 0},
		{"androidboot.slot_suffix", NULL, 0},
		{"androidboot.slot_retry_count", NULL, 0},
		{"androidboot.slot_successful", NULL, 0},
		{"androidboot.hardware.platform", NULL, 0},
		{"androidboot.hardware", NULL, 0},
		{"androidboot.revision", NULL, 0},
		{"androidboot.bootloader", NULL, 0},
		{"androidboot.hardware.sku", NULL, 0},
		{"androidboot.secure_boot", NULL, 0},
		{"androidboot.cdt_hwid", NULL, 0},
		{"androidboot.hardware.majorid", NULL, 0},
		{"androidboot.dtb_idx", NULL, 0},
		{"androidboot.mode", NULL, 0},
		{"androidboot.bootreason", NULL, 0},
		{"androidboot.serialno", NULL, 0},
	};
	int ret, count = sizeof(cmdline_params) / sizeof(cmdline_params[0]);

	if (!cmdline || !params)
		return -EINVAL;

	memset(params, 0, sizeof(struct androidboot_params));

	ret = parse_cmdline_params(cmdline, cmdline_params, count);
	if (ret < 0)
		return ret;

	params->hardware_ddr = cmdline_params[0].value;
	params->ddr_info = cmdline_params[1].value;
	params->ddr_size = cmdline_params[2].value;
	params->dsi_display0 = cmdline_params[3].value;
	params->slot_suffix = cmdline_params[4].value;
	params->slot_retry_count = cmdline_params[5].value;
	params->slot_successful = cmdline_params[6].value;
	params->hardware_platform = cmdline_params[7].value;
	params->hardware = cmdline_params[8].value;
	params->revision = cmdline_params[9].value;
	params->bootloader = cmdline_params[10].value;
	params->hardware_sku = cmdline_params[11].value;
	params->secure_boot = cmdline_params[12].value;
	params->cdt_hwid = cmdline_params[13].value;
	params->hardware_majorid = cmdline_params[14].value;
	params->dtb_idx = cmdline_params[15].value;
	params->mode = cmdline_params[16].value;
	params->bootreason = cmdline_params[17].value;
	params->serialno = cmdline_params[18].value;

	return 0;
}

/**
 * free_androidboot_params() - Free memory allocated for Android boot parameters
 */
void free_androidboot_params(struct androidboot_params *params)
{
	if (!params)
		return;

	if (params->hardware_ddr) free(params->hardware_ddr);
	if (params->ddr_info) free(params->ddr_info);
	if (params->ddr_size) free(params->ddr_size);
	if (params->dsi_display0) free(params->dsi_display0);
	if (params->slot_suffix) free(params->slot_suffix);
	if (params->slot_retry_count) free(params->slot_retry_count);
	if (params->slot_successful) free(params->slot_successful);
	if (params->hardware_platform) free(params->hardware_platform);
	if (params->hardware) free(params->hardware);
	if (params->revision) free(params->revision);
	if (params->bootloader) free(params->bootloader);
	if (params->hardware_sku) free(params->hardware_sku);
	if (params->secure_boot) free(params->secure_boot);
	if (params->cdt_hwid) free(params->cdt_hwid);
	if (params->hardware_majorid) free(params->hardware_majorid);
	if (params->dtb_idx) free(params->dtb_idx);
	if (params->mode) free(params->mode);
	if (params->bootreason) free(params->bootreason);
	if (params->serialno) free(params->serialno);

	memset(params, 0, sizeof(struct androidboot_params));
}
