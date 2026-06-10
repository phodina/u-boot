// SPDX-License-Identifier: GPL-2.0+
/*
 * Diagnostic QR code rendered into the framebuffer at multiple stages
 * of full-U-Boot init.
 *
 * On boards where UART, USB and even on-screen text are all unusable
 * (e.g. SDM845 Poco F1 in early bring-up), this is the only side-channel
 * left to surface bring-up errors: gather the relevant state into a text
 * payload, drop it into a QR Code, and blit modules directly into the
 * simplefb framebuffer (bypassing vidconsole, which is exactly the
 * subsystem we want to report on).
 *
 * Scan with a phone to read off the payload, which carries:
 *   - the stage tag (LSI / PPB / BM) telling us which hook fired,
 *   - framebuffer geometry, pixel format and base address,
 *   - return codes from probing the video uclass and vidconsole, doing
 *     a test text write, and calling usb_init(),
 *   - the ABL-handed FDT's top-compatible and any panel compatible
 *     found in that FDT,
 *   - androidboot.project_codename parsed out of /chosen/bootargs,
 *   - the selected DTB's top-compatible,
 *   - the EFI subsystem init status,
 *   - count of UCLASS_SCSI and UCLASS_SERIAL devices,
 *   - stdio binding state (env stdout/stdin/stderr + actual bound dev),
 *   - return of a probe printf() into stdout,
 *   - RAM size detected,
 *   - selected DTB physical address.
 */

#include <android_ab.h>
#include <blk.h>
#include <command.h>
#include <dm.h>
#include <dm/ofnode.h>
#include <env.h>
#include <errno.h>
#include <event.h>
#include <init.h>
#include <log.h>
#include <mmc.h>
#include <part.h>
#include <stdio.h>
#include <qrcodegen.h>
#include <scsi.h>
#include <stdio_dev.h>
#include <usb.h>
#include <version.h>
#include <vsprintf.h>
#include <video.h>
#include <video_console.h>
#include <asm/global_data.h>
#include <linux/delay.h>
#include <linux/libfdt.h>

#ifdef CONFIG_EFI_LOADER
#include <efi_loader.h>
#endif

DECLARE_GLOBAL_DATA_PTR;

#define QR_SCALE	CONFIG_BOOTMENU_QR_DIAG_SCALE
#define QR_QUIET	4	/* quiet-zone width in modules */
/*
 * Cap version 15 (77x77 modules). At QR_SCALE=12 that's (77+8)*12 = 1020 px,
 * fits on a 1080-wide panel. v15 byte-mode capacity is 520 B (Ecc_LOW),
 * comfortably holding the codename / slot / serial / storage / version fields
 * added on top of the original framebuffer / DT / stdio diagnostic dump.
 *
 * The encoder auto-picks the smallest version that fits the payload, so when
 * the dump fits in v10 (e.g. on hardware that has no panel compatible) it
 * still renders at the smaller, denser size.
 */
#define QR_MAX_VERSION	15
#define QR_PAYLOAD_MAX	512
#define QR_Y_OFFSET	CONFIG_BOOTMENU_QR_DIAG_Y_OFFSET

static void qr_fill_block(struct video_priv *vid, int px, int py,
			  int sz, u32 colour)
{
	int x, y;

	for (y = 0; y < sz; y++) {
		u8 *row;

		if (py + y >= vid->ysize)
			break;
		row = (u8 *)vid->fb + (py + y) * vid->line_length;
		for (x = 0; x < sz; x++) {
			int xx = px + x;

			if (xx >= vid->xsize)
				break;
			if (vid->bpix == VIDEO_BPP32)
				((u32 *)row)[xx] = colour;
			else if (vid->bpix == VIDEO_BPP16)
				((u16 *)row)[xx] = (u16)colour;
		}
	}
}

static void qr_render(struct video_priv *vid, const u8 *qr,
		      int origin_x, int origin_y)
{
	int n = qrcodegen_getSize(qr);
	int total = (n + QR_QUIET * 2) * QR_SCALE;
	u32 white, black;
	int mx, my;

	if (vid->bpix == VIDEO_BPP16) {
		white = 0xffff;
		black = 0x0000;
	} else {
		white = 0x00ffffff;
		black = 0x00000000;
	}

	qr_fill_block(vid, origin_x, origin_y, total, white);

	for (my = 0; my < n; my++) {
		for (mx = 0; mx < n; mx++) {
			if (qrcodegen_getModule(qr, mx, my))
				qr_fill_block(vid,
					      origin_x + (QR_QUIET + mx) * QR_SCALE,
					      origin_y + (QR_QUIET + my) * QR_SCALE,
					      QR_SCALE, black);
		}
	}
}

/*
 * Codename from the *selected* DT root compatible string ("google,sargo" ->
 * "sargo"). Independent of androidboot.project_codename in the cmdline,
 * which on this build is just "earlycon".
 */
static const char *qrdiag_dt_codename(char *buf, size_t n)
{
	const char *compat = ofnode_read_string(ofnode_root(), "compatible");
	const char *p;

	if (!compat || !*compat)
		return "?";
	p = strchr(compat, ',');
	if (p)
		compat = p + 1;
	snprintf(buf, n, "%s", compat);
	return buf;
}

/*
 * Active A/B slot via misc partition on mmc 0. Returns "_a" / "_b" or "?" if
 * we can't resolve it (no ANDROID_AB, mmc not bound yet, no misc partition,
 * etc). Always called with dec_tries=false so reading the slot in the QR
 * doesn't burn one of Android's boot attempts.
 */
static const char *qrdiag_slot(char *buf, size_t n)
{
	struct blk_desc *desc;
	struct disk_partition misc;
	int slot;

	if (!IS_ENABLED(CONFIG_ANDROID_AB))
		return "?";
	desc = blk_get_devnum_by_uclass_id(UCLASS_MMC, 0);
	if (!desc)
		return "?";
	if (part_get_info_by_name(desc, "misc", &misc) < 0)
		return "?";
	slot = ab_select_slot(desc, &misc, false);
	if (slot < 0)
		return "?";

	snprintf(buf, n, "_%c", 'a' + slot);
	return buf;
}

/*
 * "<type> <size>G" for the first block device we can find - prefers UCLASS_MMC
 * (eMMC / SD) over UCLASS_SCSI (UFS on Qualcomm). Size is the user-area
 * capacity in GiB (LBA * block size, shifted by 30). Returns "?" if no block
 * device is bound at this stage.
 */
static const char *qrdiag_storage(char *buf, size_t n)
{
	struct blk_desc *desc;
	u64 bytes;
	unsigned int gib;

	desc = blk_get_devnum_by_uclass_id(UCLASS_MMC, 0);
	if (desc && desc->lba && desc->blksz) {
		bytes = (u64)desc->lba * desc->blksz;
		gib = (unsigned int)(bytes >> 30);
		snprintf(buf, n, "mmc %uG", gib);
		return buf;
	}
	desc = blk_get_devnum_by_uclass_id(UCLASS_SCSI, 0);
	if (desc && desc->lba && desc->blksz) {
		bytes = (u64)desc->lba * desc->blksz;
		gib = (unsigned int)(bytes >> 30);
		snprintf(buf, n, "ufs %uG", gib);
		return buf;
	}
	return "?";
}

/*
 * eMMC CID Product Serial Number (32 bits) printed as hex - same field the
 * `mmc info` text dump exposes. Falls back to env $serial# (set by some
 * boards) and finally "?".
 */
static const char *qrdiag_serial(char *buf, size_t n)
{
	const char *env_sn = env_get("serial#");
	struct blk_desc *desc;

	if (env_sn && *env_sn)
		return env_sn;

	desc = blk_get_devnum_by_uclass_id(UCLASS_MMC, 0);
	if (desc) {
		struct mmc *mmc = find_mmc_device(desc->devnum);

		if (mmc) {
			/* CID PSN: bits 16..47, spans cid[2] low + cid[3] high. */
			u32 psn = ((mmc->cid[2] & 0xffff) << 16) |
				  ((mmc->cid[3] >> 16) & 0xffff);

			snprintf(buf, n, "%08x", psn);
			return buf;
		}
	}
	return "?";
}

int bootmenu_qr_diag(void)
{
	static u8 qrbuf[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
	static u8 tmpbuf[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
	struct udevice *vid_dev = NULL;
	struct video_priv *priv = NULL;
	char payload[QR_PAYLOAD_MAX];
	char cn_buf[24], slot_buf[8], stor_buf[24], sn_buf[20];
	const char *cn_str, *slot_str, *stor_str, *sn_str;
	unsigned long ram_mb;
	int total, origin_x, origin_y, n;

	if (uclass_first_device_err(UCLASS_VIDEO, &vid_dev) == 0 && vid_dev)
		priv = dev_get_uclass_priv(vid_dev);

	ram_mb = gd ? (unsigned long)(gd->ram_size >> 20) : 0;
	cn_str = qrdiag_dt_codename(cn_buf, sizeof(cn_buf));
	slot_str = qrdiag_slot(slot_buf, sizeof(slot_buf));
	stor_str = qrdiag_storage(stor_buf, sizeof(stor_buf));
	sn_str = qrdiag_serial(sn_buf, sizeof(sn_buf));

	/*
	 * Payload fields:
	 *   ver  = U-Boot PLAIN_VERSION
	 *   cn   = codename from DT root compatible
	 *   slot = active A/B slot ("_a" / "_b") from misc partition
	 *   sn   = eMMC CID PSN, or env $serial# when set
	 *   ram  = gd->ram_size in MiB
	 *   stor = first bound block device + capacity ("mmc 58G" / "ufs 64G")
	 */
	snprintf(payload, sizeof(payload),
		 "ver=%s\n"
		 "cn=%s\n"
		 "slot=%s sn=%s\n"
		 "ram=%luMB stor=%s\n",
		 PLAIN_VERSION,
		 cn_str,
		 slot_str, sn_str,
		 ram_mb, stor_str);

	if (!qrcodegen_encodeText(payload, tmpbuf, qrbuf,
				  qrcodegen_Ecc_LOW,
				  qrcodegen_VERSION_MIN, QR_MAX_VERSION,
				  qrcodegen_Mask_AUTO, true))
		return -ENOSPC;

	if (!priv || (priv->bpix != VIDEO_BPP32 && priv->bpix != VIDEO_BPP16))
		return -ENODEV;

	/*
	 * Centre the QR horizontally, anchor it to the bottom of the panel
	 * with a QR_Y_OFFSET pixel margin from the bottom edge. This keeps
	 * the top of the panel clear for U-Boot console / bootmenu text
	 * (which is the bit we are *also* trying to debug — painting the
	 * QR over it would defeat the purpose). The margin clears the
	 * bottom rounded corner crop on phone-shaped panels (Poco F1).
	 */
	n = qrcodegen_getSize(qrbuf);
	total = (n + QR_QUIET * 2) * QR_SCALE;
	origin_x = (priv->xsize > total) ? (priv->xsize - total) / 2 : 0;
	origin_y = (priv->ysize > total + QR_Y_OFFSET)
		   ? priv->ysize - total - QR_Y_OFFSET
		   : 0;

	qr_render(priv, qrbuf, origin_x, origin_y);

	video_damage(vid_dev, origin_x, origin_y, total, total);
	video_sync(vid_dev, true);

	/* Block until any key is pressed (volume button via button-kbd, or
	 * anything on serial / usbacm). The diagnostic is only triggered
	 * by the user from the menu so a working input device is always
	 * already bound at this point.
	 */
	while (!tstc())
		mdelay(20);
	getchar();

	return 0;
}

/*
 * On-demand QR diagnostic, invoked from the phonemenu's Diagnostics
 * submenu (or directly from the CLI).
 */
static int do_qrdiag(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	return bootmenu_qr_diag() ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_CMD(qrdiag, 1, 0, do_qrdiag,
	   "render the device-info QR on the framebuffer",
	   "\n"
	   "    - Paint codename / slot / sn / ram / stor / version as a QR.\n"
	   "      Blocks until any key is pressed.");
