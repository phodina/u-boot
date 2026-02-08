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
