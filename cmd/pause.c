// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2021
 * Samuel Dionne-Riel <samuel@dionne-riel.com>
 */

#include <command.h>
#include <stdio.h>

static int do_pause(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	char *message = "Press any key to continue...";

	if (argc == 2)
		message = argv[1];

	/*
	 * Two leading newlines so the prompt is visually separated from
	 * whatever the preceding command spilled to the console - on a phone
	 * panel the prompt otherwise butts right against the diagnostic
	 * output above it. No trailing newline; the prompt stays on its own
	 * line and the cursor sits at the end of the message.
	 */
	printf("\n\n%s", message);

	/* Wait on "any" key... */
	(void) getchar();

	/* Since there was no newline, we need it now */
	printf("\n");

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(pause, 2, 1, do_pause,
	"delay until user input",
	"[prompt] - Wait until users presses any key. [prompt] can be used to customize the message.\n"
);
