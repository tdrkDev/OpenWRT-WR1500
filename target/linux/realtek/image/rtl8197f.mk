# SPDX-License-Identifier: GPL-2.0-only

define Build/rt-loader-rtl8197f
	$(MAKE) all clean -C rt-loader CROSS_COMPILE="$(TARGET_CROSS)" \
		RT_LOADER_SOC=rtl8197f KERNEL_ADDR="$(KERNEL_LOADADDR)" \
		KERNEL_IMG_IN="$@" KERNEL_IMG_OUT="$@.new" BUILD_DIR="$@.build"
	mv "$@.new" "$@"
endef

# The Realtek boot code copies the image with this header from flash to the
# start address and jumps to it, after checking the signature and checksum.
define Build/realtek-cvimg
	$(SCRIPT_DIR)/realtek-cvimg.py \
		--signature cs6c \
		--start-addr 0x80a00000 \
		--burn-addr $(1) \
		$@ $@.new
	mv $@.new $@
endef

# The recovery mode of the Cudy boot code (reset button held at power-on) takes
# a vendor "flash.bin" by TFTP: boot code with its "OEM_BOOT" version at
# 127 KiB, a block checked only by the vendor firmware, then the firmware.
# The boot code writes all but the first 192 KiB to 0x40000 if the image ends
# with metadata that starts with "metadata_version" 12 bytes in. Minimal
# metadata keeps sysupgrade from taking the image.
define Build/cudy-recovery
	echo '{  "metadata_version": "1.1" }' > $@.meta
	fwtool -I $@.meta $@
	dd if=/dev/zero of=$@.head bs=1k count=127
	printf 'OEM_BOOT 00000000' >> $@.head
	dd if=$@.head of=$@.new bs=192k conv=sync
	cat $@ >> $@.new
	mv $@.new $@
	rm -f $@.meta $@.head
endef

# The Realtek boot code can load a raw image to RAM and jump to it. rt-loader
# relocates itself to the end of RAM and extracts the kernel to KERNEL_LOADADDR,
# so the image can be started from any RAM address.
define Device/rtl8197f-rt-loader
  KERNEL/rt-loader := \
	kernel-bin | \
	append-dtb | \
	rt-compress | \
	rt-loader-rtl8197f
  KERNEL := $$(KERNEL/rt-loader)
  KERNEL_INITRAMFS := $$(KERNEL/rt-loader)
endef

# The Cudy boot code looks for the kernel at 0x40000 and then in 4 KiB steps
# up to 0x2B0000, with its image header 64 bytes in. The 64 bytes hold a
# uImage header, which the boot code skips and the firmware splitter uses.
define Device/cudy_wr1500
  $(Device/rtl8197f-rt-loader)
  SOC := rtl8197fh
  DEVICE_VENDOR := Cudy
  DEVICE_MODEL := WR1500
  SUPPORTED_DEVICES += R76
  DEVICE_PACKAGES := kmod-dsa-rtl8365mb -uboot-envtools
  KERNEL := $$(KERNEL/rt-loader) | realtek-cvimg 0x40000 | uImage none
  IMAGE_SIZE := 16000k
  IMAGES += recovery.bin
  IMAGE/recovery.bin := append-kernel | pad-to 64k | append-rootfs | \
	pad-rootfs | check-size | cudy-recovery
endef
TARGET_DEVICES += cudy_wr1500
