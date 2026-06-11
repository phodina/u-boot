// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Philippe Reynes <philippe.reynes@softathome.com>
 *
 * Based on led.c
 */

#include <command.h>
#include <dm.h>
#include <button.h>
#include <stdio.h>
#include <time.h>
#include <vsprintf.h>
#include <dm/uclass-internal.h>
#include <linux/delay.h>

static const char *const state_label[] = {
	[BUTTON_OFF]	= "off",
	[BUTTON_ON]	= "on",
};

static int show_button_state(struct udevice *dev)
{
	int ret;

	ret = button_get_state(dev);
	if (ret >= BUTTON_COUNT)
		ret = -EINVAL;
	if (ret >= 0)
		printf("%s\n", state_label[ret]);

	return ret;
}

static int list_buttons(void)
{
	struct udevice *dev;
	int ret;

	for (uclass_find_first_device(UCLASS_BUTTON, &dev);
	     dev;
	     uclass_find_next_device(&dev)) {
		struct button_uc_plat *plat = dev_get_uclass_plat(dev);

		if (!plat->label)
			continue;
		printf("%-15s ", plat->label);
		if (device_active(dev)) {
			ret = show_button_state(dev);
			if (ret < 0)
				printf("Error %d\n", ret);
		} else {
			printf("<inactive>\n");
		}
	}

	return 0;
}

/*
 * Live button monitor: keep redrawing a table of every UCLASS_BUTTON device
 * with its current on/off state until the timeout expires. Uses ANSI
 * cursor-home so the table stays in place instead of scrolling the console.
 * Useful for verifying button wiring / GPIO polarity from the phonemenu
 * without a debug serial cable.
 */
static int monitor_buttons(unsigned long timeout_ms)
{
	ulong start = get_timer(0);
	ulong elapsed;

	puts("\x1b[2J\x1b[H");
	puts("Button monitor\r\n\r\n");

	for (;;) {
		struct udevice *dev;

		elapsed = get_timer(start);
		if (elapsed >= timeout_ms)
			break;

		/* Cursor to row 3, col 1 - just below the header line. */
		puts("\x1b[3;1H");
		printf("%lu.%lus left\x1b[K\r\n",
		       (timeout_ms - elapsed) / 1000,
		       ((timeout_ms - elapsed) % 1000) / 100);
		puts("--------------------\r\n");

		for (uclass_first_device(UCLASS_BUTTON, &dev);
		     dev;
		     uclass_next_device(&dev)) {
			struct button_uc_plat *plat = dev_get_uclass_plat(dev);
			int state;

			/* Skip umbrella nodes (e.g. qcom PMIC's "pon" parent)
			 * which are bound under UCLASS_BUTTON but have no
			 * label and no probed state to query - asking them
			 * dereferences a NULL priv->pmic.
			 */
			if (!plat->label)
				continue;
			state = button_get_state(dev);
			/* \x1b[K clears to end of line so a long previous label
			 * does not leave residue when the table changes width.
			 */
			printf("%-15s %-3s\x1b[K\r\n", plat->label,
			       (state == BUTTON_ON) ? "ON" : "off");
		}

		mdelay(100);
	}

	puts("\r\nMonitor done.\r\n");
	return 0;
}

int do_button(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	const char *button_label;
	struct udevice *dev;
	int ret;

	/* Validate arguments */
	if (argc < 2)
		return CMD_RET_USAGE;
	button_label = argv[1];
	if (strncmp(button_label, "list", 4) == 0)
		return list_buttons();
	if (strncmp(button_label, "monitor", 7) == 0) {
		unsigned long timeout_ms = 5000;

		if (argc >= 3)
			timeout_ms = dectoul(argv[2], NULL);
		return monitor_buttons(timeout_ms);
	}

	ret = button_get_by_label(button_label, &dev);
	if (ret) {
		printf("Button '%s' not found (err=%d)\n", button_label, ret);
		return CMD_RET_FAILURE;
	}

	ret = show_button_state(dev);

	return !ret;
}

U_BOOT_CMD(
	button, 3, 1, do_button,
	"manage buttons",
	"<button_label>            \tGet button state\n"
	"button list                  \tShow a list of buttons\n"
	"button monitor [timeout_ms]  \tLive table, updates every 100ms,\n"
	"                             \tquits on timeout (default 5000ms)"
);
