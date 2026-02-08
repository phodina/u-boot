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

/**
 * struct cmdline_mask_param - Structure for masking/replacing command line parameters
 * @key: The parameter key/name
 * @new_value: New value to replace with or NULL to only mask (remove from cmdline)
 */
struct cmdline_mask_param {
	const char *key;
	const char *new_value;
};

/**
 * mask_cmdline_params() - Mask or replace selected parameters in a command line
 * @cmdline: The original command line string
 * @params: Array of parameters to mask/replace
 * @count: Number of parameters in the array
 *
 * This function creates a new command line string with specified parameters
 * either masked (removed from cmdline) or replaced with new values.
 *
 * Return: Newly allocated string with masked/replaced parameters, or NULL on error
 *         The caller must free the returned string.
 *
 * Example usage:
 *   struct cmdline_mask_param mask_params[] = {
 *       {"androidboot.serialno", NULL},       // Mask
 *       {"androidboot.hardware", "newboard"}, // Replace value
 *   };
 */
char *mask_cmdline_params(const char *cmdline, struct cmdline_mask_param *params, int count);

#endif /* _CMDLINE_UTILS_H_ */
