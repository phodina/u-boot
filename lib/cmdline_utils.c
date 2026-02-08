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
