# SPDX-License-Identifier: GPL-2.0-or-later

include $(TOPDIR)/rules.mk

LUCI_TITLE:=CPU clock setting for RTL8197F routers
LUCI_DESCRIPTION:=Chooses the CPU clock of an RTL8197F router, from the stock \
	1000 MHz up to 1200 MHz, which the kernel applies at boot.
LUCI_DEPENDS:=@TARGET_realtek_rtl8197f +luci-base
LUCI_PKGARCH:=all

PKG_LICENSE:=GPL-2.0-or-later

define Package/luci-app-cpuclk/conffiles
/etc/config/cpuclk
endef

ifneq ($(wildcard $(TOPDIR)/feeds/luci/luci.mk),)
include $(TOPDIR)/feeds/luci/luci.mk
endif

# call BuildPackage - OpenWrt buildroot signature
