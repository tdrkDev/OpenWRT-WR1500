# SPDX-License-Identifier: GPL-2.0-only
ARCH:=mipsel
SUBTARGET:=rtl8197f
CPU_TYPE:=24kc
BOARD:=realtek
BOARDNAME:=Realtek MIPS RTL8197F
DEVICE_TYPE:=router

define Target/Description
	Build firmware images for Realtek RTL8197F based routers.
endef

FEATURES := $(filter-out mips16,$(FEATURES))
