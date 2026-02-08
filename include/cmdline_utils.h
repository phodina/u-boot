/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Command line parsing utilities
 *
 * Copyright (c) 2026 Petr Hodina
 */

#ifndef _CMDLINE_UTILS_H_
#define _CMDLINE_UTILS_H_

#include <linux/types.h>

/**
 * struct cmdline_param - Structure to hold a command line parameter key-value pair
 * @key: The parameter key/name
 * @value: The parameter value (allocated, must be freed by caller)
 * @found: Whether this parameter was found in the command line
 */
struct cmdline_param {
	const char *key;
	char *value;
	bool found;
};

/**
 * parse_cmdline_params() - Extract multiple parameters from a command line string
 * @cmdline: The command line string to parse
 * @params: Array of cmdline_param structures with keys to search for
 * @count: Number of parameters in the array
 *
 * This function searches for multiple parameters in a command line string.
 * Each parameter should be in the format "key=value" and separated by spaces.
 * The function will allocate memory for the values found, which must be freed
 * by the caller.
 *
 * Return: 0 on success, negative error code on failure
 *
 */
int parse_cmdline_params(const char *cmdline, struct cmdline_param *params, int count);

#endif /* _CMDLINE_UTILS_H_ */
