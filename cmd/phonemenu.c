// SPDX-License-Identifier: GPL-2.0+
/*
 * Pixel-drawn sectioned boot menu for phone-form-factor U-Boot.
 *
 * Renders directly into the framebuffer via the video uclass:
 *  - solid rectangles for the background, dividers and active-row pill
 *    (video_fill_part)
 *  - bitmap text at arbitrary pixel coordinates with arbitrary fg/bg
 *    (vidconsole_set_cursor_pos + vidconsole_put_string, with priv->colour_fg
 *    set to a packed RGB pixel)
 *
 * The TrueType renderer is grayscale-only at the moment
 * (console_truetype.c:402-420 treats colour_fg/bg as booleans), so we stick
 * with the 16x32 bitmap font and get full RGB per element.
 *
 * Navigation uses bootmenu_loop() so volume up/down/power work via
 * BUTTON_REMAP_PHONE_KEYS without any extra wiring.
 */

#include <android_ab.h>
#include <bcb.h>
#include <blk.h>
#include <cli.h>
#include <power/pmic.h>
#include <sysreset.h>
#include <command.h>
#include <dm.h>
#include <dm/ofnode.h>
#include <dm/uclass.h>
#include <env.h>
#include <log.h>
#include <malloc.h>
#include <menu.h>
#include <net-common.h>
#include <part.h>
#include <stdio.h>
#include <stdio_dev.h>
#include <video.h>
#include <video_console.h>
#include <version.h>
#include <asm/system.h>
#include <linux/delay.h>
#include <linux/string.h>

enum pm_section {
	PM_SEC_BOOT,
	PM_SEC_USB,
	PM_SEC_DIAG,
	PM_SEC_SYS,
	PM_SEC_COUNT,
};

struct pm_item {
	const char *label;
	const char *command;
	enum pm_section section;
};

static const char * const pm_section_label[PM_SEC_COUNT] = {
	[PM_SEC_BOOT] = "BOOT",
	[PM_SEC_USB]  = "USB MODES",
	[PM_SEC_DIAG] = "DIAGNOSTICS",
	[PM_SEC_SYS]  = "SYSTEM",
};

static const struct pm_item pm_main[] = {
	{ "Boot",             "run bootcmd",                              PM_SEC_BOOT },
	{ "TFTP boot",        "run tftp_boot; phonemenu",                 PM_SEC_BOOT },
	{ "PXE boot",         "run pxe_boot; phonemenu",                  PM_SEC_BOOT },
	/*
	 * Serial gadget intentionally does NOT re-enter the menu: serial_gadget
	 * also sets bootretry=-1, so once it returns pm_run exits, menucmd
	 * finishes, and u-boot falls through to the CLI prompt on serial/usbacm
	 * — which is what the user actually wants when they enable the gadget.
	 */
	{ "Serial gadget",    "run serial_gadget",                        PM_SEC_USB  },
	{ "USB mass storage", "ums 0 ${storage} 0; phonemenu",            PM_SEC_USB  },
	{ "Fastboot",         "run fastboot; phonemenu",                  PM_SEC_USB  },
	{ "DFU",              "run dfu; phonemenu",                       PM_SEC_USB  },
	{ "Diagnostics >",    "phonemenu diag",                           PM_SEC_DIAG },
	/*
	 * Shell: disable bootretry so the 1 s readline timeout doesn't
	 * auto-inject "run bootcmd" and bounce us back into the menu.
	 */
	{ "Shell",            "setenv bootretry -1",                      PM_SEC_SYS  },
	{ "Power off",        "qcom_poweroff",                            PM_SEC_SYS  },
	{ "Reboot",           "reset",                                    PM_SEC_SYS  },
};

static const struct pm_item pm_diag[] = {
	{ "Dump clocks",      "clk dump; pause; phonemenu diag",          PM_SEC_DIAG },
	{ "Dump environment", "printenv; pause; phonemenu diag",          PM_SEC_DIAG },
	{ "Bootargs",         "printenv bootargs; pause; phonemenu diag", PM_SEC_DIAG },
	{ "Board info",       "bdinfo; pause; phonemenu diag",            PM_SEC_DIAG },
	{ "Buttons",          "button list; pause; phonemenu diag",       PM_SEC_DIAG },
	{ "< Back",           "phonemenu",                                PM_SEC_SYS  },
};

/* Layout (pixels) */
#define CARD_MARGIN_X		40
#define HEADER_TITLE_Y		150
#define HEADER_SLOT_Y		200
#define HEADER_VERSION_Y	240
#define HR_Y			290
#define HR_THICK		2
#define BODY_TOP		(HR_Y + HR_THICK + 36)
#define SECTION_LABEL_H		44
#define ITEM_H			72
#define FOOTER_PAD		120
#define INDENT_SECTION		88
#define INDENT_ITEM		120

/*
 * Colours, packed for X8R8G8B8 (which is what the FP5 simple-framebuffer
 * advertises). The bitmap text renderer writes this value verbatim into
 * each lit pixel and the bg value into each unlit pixel.
 */
#define RGB(r, g, b)		(((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))
#define COL_BG			RGB(0x0d, 0x11, 0x17)
#define COL_HR			RGB(0x30, 0x36, 0x3d)
#define COL_TITLE		RGB(0xf0, 0xf3, 0xf6)
#define COL_SUB			RGB(0x7d, 0x85, 0x90)
#define COL_SECTION		RGB(0x58, 0xa6, 0xff)
#define COL_ITEM		RGB(0xc9, 0xd1, 0xd9)
#define COL_HL_BG		RGB(0x1f, 0x6f, 0xeb)
#define COL_HL_FG		RGB(0xff, 0xff, 0xff)
#define COL_FOOTER		RGB(0x7d, 0x85, 0x90)

/* The 16x32 bitmap font is 16 px wide / 32 px tall. */
#define FONT_W			16
#define FONT_H			32

struct pm_state {
	struct udevice *vid;
	struct udevice *vc;
	struct video_priv *vp;
	int w, h;
	const struct pm_item *items;
	int count;
	int active;
	int last_active;
	int *row_y;
	int *section_y;		/* y of the section label above item[i], -1 if none */
	int footer_y;
	const char *title;	/* codename, e.g. "sargo" */
	const char *subtitle;	/* slot line, e.g. "slot _b" - NULL/"" to skip */
	const char *version;	/* U-Boot version line - NULL/"" to skip */
};

static int pm_init_dev(struct pm_state *s)
{
	int ret;

	ret = uclass_first_device_err(UCLASS_VIDEO, &s->vid);
	if (ret)
		return ret;
	ret = uclass_first_device_err(UCLASS_VIDEO_CONSOLE, &s->vc);
	if (ret)
		return ret;

	s->vp = dev_get_uclass_priv(s->vid);
	s->w = video_get_xsize(s->vid);
	s->h = video_get_ysize(s->vid);
	return 0;
}

static void pm_text(struct pm_state *s, int x, int y, u32 fg, u32 bg,
		    const char *str)
{
	s->vp->colour_fg = fg;
	s->vp->colour_bg = bg;
	vidconsole_set_cursor_pos(s->vc, x, y);
	vidconsole_put_string(s->vc, (char *)str);
}

static int pm_center_x(struct pm_state *s, const char *str)
{
	int px = (int)strlen(str) * FONT_W;

	return (s->w - px) / 2;
}

static void pm_layout(struct pm_state *s)
{
	enum pm_section prev = PM_SEC_COUNT;
	int y = BODY_TOP;
	int i;

	for (i = 0; i < s->count; i++) {
		if (s->items[i].section != prev) {
			s->section_y[i] = y;
			y += SECTION_LABEL_H;
			prev = s->items[i].section;
		} else {
			s->section_y[i] = -1;
		}
		s->row_y[i] = y;
		y += ITEM_H;
	}
	s->footer_y = s->h - FOOTER_PAD;
}

static void pm_draw_item(struct pm_state *s, int i, bool active)
{
	int y = s->row_y[i];
	int text_y = y + (ITEM_H - FONT_H) / 2;
	u32 fg, bg;

	if (active) {
		video_fill_part(s->vid, CARD_MARGIN_X, y,
				s->w - CARD_MARGIN_X, y + ITEM_H,
				COL_HL_BG);
		fg = COL_HL_FG;
		bg = COL_HL_BG;
	} else {
		video_fill_part(s->vid, CARD_MARGIN_X, y,
				s->w - CARD_MARGIN_X, y + ITEM_H,
				COL_BG);
		fg = COL_ITEM;
		bg = COL_BG;
	}

	pm_text(s, INDENT_ITEM, text_y, fg, bg, s->items[i].label);
}

static void pm_draw_chrome(struct pm_state *s)
{
	int hr_x0 = CARD_MARGIN_X + 20;
	int hr_x1 = s->w - CARD_MARGIN_X - 20;
	const char *foot = "Vol up/down to move    Power to select";
	int i;

	video_fill_part(s->vid, 0, 0, s->w, s->h, COL_BG);

	pm_text(s, pm_center_x(s, s->title), HEADER_TITLE_Y,
		COL_TITLE, COL_BG, s->title);

	if (s->subtitle && *s->subtitle)
		pm_text(s, pm_center_x(s, s->subtitle), HEADER_SLOT_Y,
			COL_SUB, COL_BG, s->subtitle);

	if (s->version && *s->version)
		pm_text(s, pm_center_x(s, s->version), HEADER_VERSION_Y,
			COL_SUB, COL_BG, s->version);

	video_fill_part(s->vid, hr_x0, HR_Y, hr_x1, HR_Y + HR_THICK, COL_HR);

	for (i = 0; i < s->count; i++) {
		if (s->section_y[i] < 0)
			continue;
		pm_text(s, INDENT_SECTION, s->section_y[i],
			COL_SECTION, COL_BG,
			pm_section_label[s->items[i].section]);
	}

	video_fill_part(s->vid, hr_x0, s->footer_y - 36,
			hr_x1, s->footer_y - 36 + HR_THICK, COL_HR);
	pm_text(s, pm_center_x(s, foot), s->footer_y,
		COL_FOOTER, COL_BG, foot);
}

/*
 * Mirror the menu to byte-stream consoles (serial UART, USB CDC ACM) so the
 * user can see the menu over a terminal — the framebuffer rendering above is
 * invisible to anyone connected by serial. We deliberately bypass stdout
 * (which would also drive vidconsole) and write straight to the named stdio
 * devices, otherwise the ANSI escapes corrupt the bitmap-text layout.
 */
static void pm_term_puts(const char *str)
{
	static const char * const names[] = { "serial", "usbacm" };
	struct stdio_dev *d;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		d = stdio_get_by_name(names[i]);
		if (d && d->puts)
			d->puts(d, str);
	}
}

static void pm_term_render(struct pm_state *s)
{
	enum pm_section prev = PM_SEC_COUNT;
	char buf[160];
	int i;

	/* Clear screen, cursor home, hide cursor while we paint. */
	pm_term_puts("\x1b[2J\x1b[H\x1b[?25l");

	snprintf(buf, sizeof(buf), "  %s\r\n", s->title);
	pm_term_puts(buf);
	if (s->subtitle && *s->subtitle) {
		snprintf(buf, sizeof(buf), "  %s\r\n", s->subtitle);
		pm_term_puts(buf);
	}
	if (s->version && *s->version) {
		snprintf(buf, sizeof(buf), "  %s\r\n", s->version);
		pm_term_puts(buf);
	}
	pm_term_puts("\r\n");

	for (i = 0; i < s->count; i++) {
		if (s->items[i].section != prev) {
			snprintf(buf, sizeof(buf), "  [%s]\r\n",
				 pm_section_label[s->items[i].section]);
			pm_term_puts(buf);
			prev = s->items[i].section;
		}
		if (i == s->active)
			snprintf(buf, sizeof(buf),
				 "    \x1b[7m> %s\x1b[0m\r\n",
				 s->items[i].label);
		else
			snprintf(buf, sizeof(buf), "      %s\r\n",
				 s->items[i].label);
		pm_term_puts(buf);
	}

	pm_term_puts("\r\n  Vol up/down or arrows to move    Power or Enter to select\r\n");
}

static void pm_render_all(struct pm_state *s)
{
	int i;

	pm_draw_chrome(s);
	for (i = 0; i < s->count; i++)
		pm_draw_item(s, i, i == s->active);
	video_sync(s->vid, true);

	pm_term_render(s);
}

static int pm_run(const struct pm_item *items, int count,
		  const char *title, const char *subtitle, const char *version)
{
	struct pm_state s = { 0 };
	struct cli_ch_state cch;
	struct bootmenu_data nav = {
		.delay = -1,
		.active = 0,
		.last_active = 0,
		.count = count,
		.first = NULL,
	};
	const char *cmd;
	bool running = true;
	int ret = CMD_RET_SUCCESS;

	if (pm_init_dev(&s))
		return CMD_RET_FAILURE;

	s.items = items;
	s.count = count;
	s.title = title;
	s.subtitle = subtitle;
	s.version = version;
	s.row_y = calloc(count, sizeof(int));
	s.section_y = calloc(count, sizeof(int));
	if (!s.row_y || !s.section_y) {
		ret = CMD_RET_FAILURE;
		goto cleanup;
	}

	pm_layout(&s);
	pm_render_all(&s);

	cli_ch_init(&cch);
	while (running) {
		enum bootmenu_key k = bootmenu_loop(&nav, &cch);

		switch (k) {
		case BKEY_UP:
			/* Wrap from item 0 around to the last item. */
			s.last_active = s.active;
			s.active = s.active ? s.active - 1 : count - 1;
			pm_draw_item(&s, s.last_active, false);
			pm_draw_item(&s, s.active, true);
			video_sync(s.vid, true);
			pm_term_render(&s);
			break;
		case BKEY_DOWN:
			/* Wrap from the last item back to item 0. */
			s.last_active = s.active;
			s.active = (s.active < count - 1) ? s.active + 1 : 0;
			pm_draw_item(&s, s.last_active, false);
			pm_draw_item(&s, s.active, true);
			video_sync(s.vid, true);
			pm_term_render(&s);
			break;
		case BKEY_SELECT:
			running = false;
			break;
		case BKEY_QUIT:
			s.active = count - 1;
			running = false;
			break;
		default:
			break;
		}
	}

	/* Repaint to BG before running the chosen command so the next
	 * screen (pause, shell, etc) starts on a clean canvas. */
	video_fill_part(s.vid, 0, 0, s.w, s.h, COL_BG);
	vidconsole_set_cursor_pos(s.vc, 16, 16);
	s.vp->colour_fg = COL_ITEM;
	s.vp->colour_bg = COL_BG;
	video_sync(s.vid, true);

	/* Reset the terminal: clear screen, show cursor, reset colours. */
	pm_term_puts("\x1b[2J\x1b[H\x1b[?25h\x1b[0m");

	cmd = items[s.active].command;
	if (cmd && cmd[0])
		run_command((char *)cmd, 0);
	/* empty command (e.g. Shell) falls through to the console */

cleanup:
	free(s.row_y);
	free(s.section_y);
	return ret;
}

/*
 * Codename from the DT root compatible string ("google,sargo" -> "sargo").
 * Falls back to the empty string when the DT is missing or unexpected so the
 * header still renders cleanly.
 */
static const char *pm_codename(void)
{
	static char buf[32];
	const char *compat;
	const char *p;

	compat = ofnode_read_string(ofnode_root(), "compatible");
	if (!compat || !*compat)
		return "unknown";

	p = strchr(compat, ',');
	if (p)
		compat = p + 1;

	strlcpy(buf, compat, sizeof(buf));
	return buf;
}

/*
 * PMIC PON ("Power-On Notification") boot trigger from the QPNP-PON block
 * in the primary PMIC. Two registers are interesting:
 *
 *   PON_REASON1 (PON base + 0x0c) - cold-reset trigger:
 *     0x80 KPDPWR   = power button press
 *     0x40 CBLPWR   = cable inserted while off
 *     0x20 PON1     = generic PON
 *     0x10 USB_CHG  = USB charger inserted
 *     0x08 DC_CHG   = DC charger inserted
 *     0x04 RTC      = RTC alarm
 *     0x02 SMPL     = sudden mains power loss / auto-restart
 *     0x01 HARDRST  = hard reset requested
 *
 *   WARM_RESET_REASON1 (PON base + 0x0a) - warm-reset trigger:
 *     0x80 KPDPWR        = power-button reset
 *     0x40 RESIN         = volume-down (or assigned) reset
 *     0x20 KPDPWR_RESIN  = power + vol-down combo (force reset)
 *     0x04 PMIC_WD       = PMIC watchdog
 *     0x02 PS_HOLD       = SoC pulled PS_HOLD low (normal reboot)
 *     0x01 SOFT          = soft reset request
 *
 * If WARM_RESET_REASON1 is non-zero this was a warm reboot and we use it;
 * otherwise PON_REASON1 wins. Returned strings are short for the menu
 * subtitle. NULL if we can't find a PM660-class PMIC at all.
 */
static const char *pm_pon(void)
{
	struct udevice *pmic;
	int r;

	for (uclass_first_device(UCLASS_PMIC, &pmic);
	     pmic;
	     uclass_next_device(&pmic)) {
		if (ofnode_device_is_compatible(dev_ofnode(pmic),
						"qcom,pm660"))
			break;
	}
	if (!pmic)
		return NULL;

	r = pmic_reg_read(pmic, 0x80a);
	if (r > 0) {
		if (r & 0x20) return "warm: pwr+vol-";
		if (r & 0x80) return "warm: pwr";
		if (r & 0x40) return "warm: vol-";
		if (r & 0x04) return "warm: wdog";
		if (r & 0x02) return "warm: reboot";
		if (r & 0x01) return "warm: soft";
		return "warm";
	}

	r = pmic_reg_read(pmic, 0x80c);
	if (r > 0) {
		if (r & 0x80) return "cold: pwr";
		if (r & 0x40) return "cold: cable";
		if (r & 0x10) return "cold: usb";
		if (r & 0x08) return "cold: dc";
		if (r & 0x04) return "cold: rtc";
		if (r & 0x02) return "cold: smpl";
		if (r & 0x01) return "cold: hardrst";
		if (r & 0x20) return "cold";
	}
	return NULL;
}

/*
 * Boot reason from the Android Bootloader Control Block on mmc 0's misc
 * partition. Maps the raw "command" field to short labels:
 *   ""                     -> "normal"
 *   "boot-recovery"        -> "recovery"
 *   "bootonce-bootloader"  -> "fastboot"
 *   "boot-fastboot"        -> "fastbootd"
 *   anything else          -> the raw command (truncated)
 *
 * Falls back to "normal" on any error (no BCB cmd compiled in, no misc
 * partition, load failure, ...) since on a healthy device that's almost
 * always the right answer.
 */
static const char *pm_reason(void)
{
	static char buf[32];
	char cmd[32];

	if (bcb_find_partition_and_load("mmc", 0, "misc"))
		return "normal";
	if (bcb_get(BCB_FIELD_COMMAND, cmd, sizeof(cmd)))
		return "normal";

	if (!cmd[0])
		return "normal";
	if (!strncmp(cmd, "boot-recovery", sizeof(cmd)))
		return "recovery";
	if (!strncmp(cmd, "bootonce-bootloader", sizeof(cmd)))
		return "fastboot";
	if (!strncmp(cmd, "boot-fastboot", sizeof(cmd)))
		return "fastbootd";

	strlcpy(buf, cmd, sizeof(buf));
	return buf;
}

/*
 * Active A/B slot via ab_select_slot() against mmc 0's misc partition.
 * dec_tries = false because we only want to *read* the current state; the
 * real boot path will decrement when it commits to a slot.
 */
static const char *pm_slot(void)
{
	struct blk_desc *desc;
	struct disk_partition misc;
	int slot;

	if (!IS_ENABLED(CONFIG_ANDROID_AB))
		return NULL;

	desc = blk_get_devnum_by_uclass_id(UCLASS_MMC, 0);
	if (!desc)
		return NULL;
	if (part_get_info_by_name(desc, "misc", &misc) < 0)
		return NULL;

	slot = ab_select_slot(desc, &misc, false);
	if (slot < 0)
		return NULL;

	return (slot == 0) ? "_a" : "_b";
}

static int do_phonemenu(struct cmd_tbl *cmdtp, int flag, int argc,
			char *const argv[])
{
	static char slot_line[96];
	const char *slot = pm_slot();
	const char *reason = pm_reason();
	const char *pon = pm_pon();
	const char *sub = (argc > 1) ? argv[1] : "main";
	int n = 0;

	/*
	 * Compose up to three fields separated by "  -  ":
	 *   slot _b  -  normal  -  warm: reboot
	 * If a field is NULL/"" we just skip it; the leading separator is
	 * suppressed for whichever field happens to be first.
	 */
	if (slot)
		n += snprintf(slot_line + n, sizeof(slot_line) - n,
			      "slot %s", slot);
	if (reason)
		n += snprintf(slot_line + n, sizeof(slot_line) - n,
			      "%s%s", n ? "  -  " : "", reason);
	if (pon)
		n += snprintf(slot_line + n, sizeof(slot_line) - n,
			      "%s%s", n ? "  -  " : "", pon);

	if (!strcmp(sub, "diag"))
		return pm_run(pm_diag, ARRAY_SIZE(pm_diag),
			      "Diagnostics", slot_line, U_BOOT_VERSION);

	return pm_run(pm_main, ARRAY_SIZE(pm_main),
		      pm_codename(), slot_line, U_BOOT_VERSION);
}

U_BOOT_CMD(phonemenu, 2, 0, do_phonemenu,
	   "pixel-drawn sectioned boot menu",
	   "[diag]\n"
	   "    - Render the main menu (default) or the diagnostics submenu.\n"
	   "      Navigation: Vol up/down to move, Power to select.\n");

#if CONFIG_IS_ENABLED(USB_ETHER)
/*
 * Bind + probe the USB Ethernet (RNDIS) gadget so the network stack has an
 * eth0 to drive. usb_ether_init() attaches the "usb_ether" driver to the
 * first UCLASS_USB_GADGET_GENERIC device; probing it then calls
 * usb_gadget_register_driver() which actually enumerates the gadget over USB.
 *
 * After this, `setenv ethact usb_ether; tftpboot ...` works. The previous
 * gadget function (fastboot/ACM) is replaced - by design, only one gadget
 * function is active at a time on this hardware.
 */
static int do_usbether(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	int ret = usb_ether_init();

	if (ret < 0) {
		printf("usb_ether init failed: %d\n", ret);
		return CMD_RET_FAILURE;
	}
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(usbether, 1, 0, do_usbether,
	   "start USB Ethernet (RNDIS) gadget for tftp/pxe boot",
	   "\n"
	   "    - Bind the USB Ethernet gadget driver. After running, set\n"
	   "      ethact=usb_ether and the network stack will route through\n"
	   "      RNDIS to the USB host.");
#endif

/*
 * Power off via PSCI. Direct PMIC writes to the QPNP-PON
 * PS_HOLD_RESET_CTL register hang on Pixel-class devices because the SPMI
 * peripheral is TZ-owned; the arbiter swallows the write and never sets
 * STATUS_DONE. PSCI lands directly in TZ and TZ handles the PMIC
 * sequencing for us.
 */
static int do_qcom_poweroff(struct cmd_tbl *cmdtp, int flag, int argc,
			    char *const argv[])
{
	puts("\nPowering off via PSCI SYSTEM_OFF\n");
	mdelay(50);                      /* flush the puts() */
	psci_system_off();
	return CMD_RET_FAILURE;          /* unreachable */
}

U_BOOT_CMD(qcom_poweroff, 1, 0, do_qcom_poweroff,
	   "shut down the SoC via PSCI SYSTEM_OFF",
	   "\n"
	   "    - Invoke PSCI 0.2 SYSTEM_OFF. TZ handles the PMIC sequencing\n"
	   "      (PS_HOLD_RESET_TYPE -> SHUTDOWN, PS_HOLD low) so the\n"
	   "      device powers off cleanly rather than cold-rebooting.");
