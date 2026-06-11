/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * UFS device quirks
 *
 * Ported from the Linux kernel (include/ufs/ufs_quirks.h) and the
 * Sony Yoshino/Tama downstream UFS quirks/hacks.
 */
#ifndef _UFS_QUIRKS_H
#define _UFS_QUIRKS_H

/* return true if s1 (the fixup pattern) is a prefix of s2 (the device value) */
#define STR_PRFX_EQUAL(s1, s2) (strncmp(s1, s2, strlen(s1)) == 0)

#define UFS_ANY_VENDOR		0xFFFF
#define UFS_ANY_MODEL		"ANY_MODEL"
#define UFS_ANY_VER		"ANY_VER"

#define UFS_VENDOR_TOSHIBA	0x198
#define UFS_VENDOR_SAMSUNG	0x1CE
#define UFS_VENDOR_SKHYNIX	0x1AD
#define UFS_VENDOR_MICRON	0x12C
#define UFS_VENDOR_WDC		0x145

/* UFS Samsung models */
#define UFS_MODEL_SAMSUNG_64GB	"KLUDG4U1EA-B0C1"
#define UFS_REVISION_SAMSUNG	"0100"

/* UFS SK Hynix models */
#define UFS_MODEL_HYNIX_32GB	"hB8aL1"
#define UFS_MODEL_HYNIX_64GB	"hC8aL1"
#define UFS_REVISION_HYNIX	"D001"

/*
 * Devices with a UFS spec version older than this must not be issued PURGE /
 * UNMAP (secure erase) operations: on the Sony Yoshino/Tama platforms doing so
 * erases the bootloader and permanently bricks the device.
 */
#define UFS_PURGE_SPEC_VER	0x210

/**
 * struct ufs_dev_quirk - ufs device quirk info
 * @wmanufacturerid: card details (UFS_ANY_VENDOR matches any vendor)
 * @model: card model (UFS_ANY_MODEL matches any model)
 * @revision: card fw revision (UFS_ANY_VER matches any revision)
 * @quirk: device quirk bitmask
 */
struct ufs_dev_quirk {
	u16 wmanufacturerid;
	const char *model;
	const char *revision;
	unsigned int quirk;
};

/* terminator for the fixup table */
#define END_FIX { 0, NULL, NULL, 0 }

/* add a quirk for any revision of a specific model */
#define UFS_FIX(_vendor, _model, _quirk) {	\
	.wmanufacturerid = (_vendor),		\
	.model = (_model),			\
	.revision = (UFS_ANY_VER),		\
	.quirk = (_quirk),			\
}

/* add a quirk for a specific model and fw revision */
#define UFS_FIX_REVISION(_vendor, _model, _revision, _quirk) {	\
	.wmanufacturerid = (_vendor),				\
	.model = (_model),					\
	.revision = (_revision),				\
	.quirk = (_quirk),					\
}

/*
 * Some Sony UFS devices must not use the PURGE / UNMAP (secure erase)
 * operation as it would erase the bootloader and permanently brick the
 * device. This quirk disables PURGE-related functionality for affected
 * devices.
 */
#define UFS_DEVICE_QUIRK_NO_PURGE		(1 << 0)

/*
 * Some SK Hynix devices on Sony Yoshino/Tama need an extended HS sync length
 * (PA_TxHsG{1,2,3}SyncLength) programmed before the power-mode change for a
 * reliable HS link.
 */
#define UFS_DEVICE_QUIRK_EXTEND_SYNC_LENGTH	(1 << 1)

#endif /* _UFS_QUIRKS_H */
