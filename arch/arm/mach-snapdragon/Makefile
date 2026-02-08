# SPDX-License-Identifier: GPL-2.0+
#
# (C) Copyright 2015 Mateusz Kulikowski <mateusz.kulikowski@gmail.com>

obj-y += board.o
ifndef CONFIG_XPL_BUILD
obj-$(CONFIG_EFI_HAVE_CAPSULE_SUPPORT) += capsule_update.o
obj-$(CONFIG_OF_LIVE) += of_fixup.o
endif
obj-$(CONFIG_SPL_QCOM_SMEM) += dtb_select.o
ifdef CONFIG_XPL_BUILD
obj-y += spl_board.o
endif
