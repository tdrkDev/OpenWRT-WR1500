# SPDX-License-Identifier: GPL-2.0-or-later

include $(TOPDIR)/rules.mk

PKG_NAME:=cudy-bdinfo-read
PKG_RELEASE:=1
CMAKE_INSTALL:=1

PKG_LICENSE:=GPL-2.0-or-later

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/cudy-bdinfo-read
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Utility to read the board data of Cudy devices
endef

define Package/cudy-bdinfo-read/description
 Utility to read the factory board data (MAC address, country, Wi-Fi
 password, serial number) from the bdinfo partition of Cudy devices.
endef

define Package/cudy-bdinfo-read/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/cudy-bdinfo-read $(1)/usr/bin/
endef

$(eval $(call BuildPackage,cudy-bdinfo-read))
