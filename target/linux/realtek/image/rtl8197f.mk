# SPDX-License-Identifier: GPL-2.0-only

define Build/rt-loader-rtl8197f
	$(MAKE) all clean -C rt-loader CROSS_COMPILE="$(TARGET_CROSS)" \
		RT_LOADER_SOC=rtl8197f KERNEL_ADDR="$(KERNEL_LOADADDR)" \
		KERNEL_IMG_IN="$@" KERNEL_IMG_OUT="$@.new" BUILD_DIR="$@.build"
	mv "$@.new" "$@"
endef

# The Realtek boot code can load a raw image to RAM and jump to it. rt-loader
# relocates itself to the end of RAM and extracts the kernel to KERNEL_LOADADDR,
# so the image can be started from any RAM address.
define Device/rtl8197f-rt-loader
  KERNEL := \
	kernel-bin | \
	append-dtb | \
	rt-compress | \
	rt-loader-rtl8197f
  KERNEL_INITRAMFS := $$(KERNEL)
endef

define Device/cudy_wr1500
  $(Device/rtl8197f-rt-loader)
  SOC := rtl8197fh
  DEVICE_VENDOR := Cudy
  DEVICE_MODEL := WR1500
  SUPPORTED_DEVICES += R76
  DEVICE_PACKAGES := kmod-dsa-rtl8365mb -uboot-envtools
  IMAGES :=
endef
TARGET_DEVICES += cudy_wr1500
