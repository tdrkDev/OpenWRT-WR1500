# SPDX-License-Identifier: GPL-2.0-or-later

include $(TOPDIR)/rules.mk

PKG_NAME:=rtw8832br-firmware
PKG_RELEASE:=1

PKG_LICENSE:=Proprietary

include $(INCLUDE_DIR)/package.mk

define Package/rtw8832br-firmware
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=Realtek RTL8832BR firmware for rtw89
  DEPENDS:=@TARGET_realtek_rtl8197f
endef

define Package/rtw8832br-firmware/description
  Firmware for the Realtek RTL8832BR 5 GHz radio, as rtw89 loads it: the
  vendor access point firmware of the chip in an MFW container, with the
  baseband, radio, NCTL and TX power tables of the Cudy WR1500 (RFE type
  50) as firmware elements.
endef

define Build/Compile
endef

define Package/rtw8832br-firmware/install
	$(INSTALL_DIR) $(1)/lib/firmware/rtw89
	$(INSTALL_DATA) ./files/rtw8832br_fw.bin $(1)/lib/firmware/rtw89/
endef

$(eval $(call BuildPackage,rtw8832br-firmware))
