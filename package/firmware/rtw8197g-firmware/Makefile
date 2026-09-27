# SPDX-License-Identifier: GPL-2.0-or-later

include $(TOPDIR)/rules.mk

PKG_NAME:=rtw8197g-firmware
PKG_RELEASE:=1

PKG_LICENSE:=Proprietary

include $(INCLUDE_DIR)/package.mk

define Package/rtw8197g-firmware
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=Realtek RTL8197F WLAN firmware for rtw88
  DEPENDS:=@TARGET_realtek_rtl8197f
endef

define Package/rtw8197g-firmware/description
  Firmware for the 2.4 GHz WLAN MAC of the Realtek RTL8197F SoC (called
  RTL8197G by the vendor driver): the vendor access point firmware.
endef

define Build/Compile
endef

define Package/rtw8197g-firmware/install
	$(INSTALL_DIR) $(1)/lib/firmware/rtw88
	$(INSTALL_DATA) ./files/rtw8197g_fw.bin $(1)/lib/firmware/rtw88/
endef

$(eval $(call BuildPackage,rtw8197g-firmware))
