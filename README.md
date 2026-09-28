# OpenWRT for Cudy WR1500

![Cudy WR1500](cudy.png "They've promised OpenWRT for their routers, but WR1500 was left out.")

<center>

🌐 *English* | [Русский](README.ru.md)

</center>

> **⚠️ IMPORTANT NOTICE**: Cudy AX1500 is a DIFFERENT router and it's not supported.
>
> Only Cudy WR1500 V1.0 was tested and supported.

## ✨ Features

* Modern kernel 6.18 instead of stock 4.4
* Modern OpenWRT (development snapshot with the `apk` package manager) instead of stock core's 21.02 one
* Focused on improving performance and stability over stock code,
  considering potato CPU
* OSS `rtw88` and `rtw89` Wi-Fi driver forks providing better throughput and stability
* OSS `rtl8365mb` Ethernet driver fork providing modern kernel support
* Hardware NAT for wired LAN <-> WAN traffic (see `Highly recommended`)
* 5 GHz DFS channels with radar detection (was not possible on stock)
* Watchdog: a hung router reboots by itself, and the kernel log survives the reboot
* Overclocking up to 1.2 GHz (see `Overclocking`)
* Additions over default OpenWRT:
  * DDNS
  * DoH/DoT (smartdns, off by default: `Services -> SmartDNS`)
  * SQM QoS
  * WireGuard VPN (+ LuCI policy-based routing)
  * LuCI OpenWrt2020 (default) and Proton2025 themes
  * LuCI dashboard / statistics
  * LuCI Russian translation
  * NFQUEUE support for DPI bypass tools such as zapret
  * iperf3 and tcpdump

## ⬇️ Installation

> **⚠️ IMPORTANT NOTICE**: Due to limited bootloader, backup of stock flash
> partitions at somewhat reasonable speed **IS NOT POSSIBLE**.
> This software is provided **AS IS** and if your router breaks and
> you'll lose your MAC/Cudy credentials, only you are responsible for recovering it.
> Cudy Cloud credentials are stored only in `bdinfo`, encrypted and signed, in a single copy:
> without them you can't get stock updates or use the Cudy app.
>
> The installer only overwrites the firmware: the bootloader, `cfg` (MACs, Wi-Fi calibration)
> and `bdinfo` stay untouched, and OpenWRT keeps them read-only. Still, save a copy of them
> right after installing (see `Highly recommended`).

### Easy Way

1. Ensure you have Python 3.7 or newer installed
2. Download `install_wr1500_*.zip` from the [releases page](https://github.com/tdrkDev/OpenWRT-WR1500/releases) and unpack it
3. Connect PC to the WAN port of the router
4. Set a static IP on your PC, e.g., 192.168.1.50/24, and disconnect it from Wi-Fi and other networks
5. Hold RESET and power on the router, keep holding until the LED becomes blue+red for more than 5 seconds, then release the button
6. In the unpacked folder, run `sh tftp.sh` in a terminal (Linux/Mac) or double-click `tftp.bat` (Windows)
7. Wait until the router writes the firmware and reboots by itself: don't power it off. The first boot takes a few minutes; the LED stays blue when OpenWRT is ready
8. Set your PC back to automatic IP (DHCP), then reconnect Ethernet to LAN or use Wi-Fi. See `Router Credentials`.

### Manual Way

1. Install TFTP client on your PC.
2. Download `*-squashfs-recovery.bin` file (not `sysupgrade` or `initramfs`) from the [releases page](https://github.com/tdrkDev/OpenWRT-WR1500/releases).
3. Connect PC to the WAN port of the router.
4. Set a static IP on your PC, e.g., 192.168.1.50/24.
5. Hold RESET and power on the router, keep holding until the LED becomes blue+red for more than 5 seconds, then release the button.
6. Use TFTP client to upload the `*-recovery.bin` file in binary mode to `192.168.1.6` as `recovery.bin`.
7. Wait until the router writes the firmware and reboots by itself: don't power it off. The first boot takes a few minutes; the LED stays blue when OpenWRT is ready.
8. Set your PC back to automatic IP (DHCP), then reconnect Ethernet to LAN or use Wi-Fi. See `Router Credentials`.

### Router Credentials

* User: `root`, no password.
* Router IP: `192.168.1.1` (LuCI web interface: http://192.168.1.1)
* Wi-Fi: the network names and password from the label on the back panel
  (`Cudy-XXXX` and `Cudy-XXXX-5G`, WPA2/WPA3), same as stock
* The Wi-Fi country comes from the factory data too. If it doesn't match where you live,
  set yours in `Network -> Wireless -> Edit -> Advanced Settings -> Country Code`

## ℹ️ Highly recommended

* **Save the factory data** right after installing.

  In `System -> Backup / Flash Firmware -> Save mtdblock contents`, download `u-boot`,
  `cfg` and `bdinfo`. OpenWRT never writes them, but nothing can bring them back if
  the flash gets wiped.

* Enable **Hardware Flow Offloading** in `Network -> Firewall -> General Settings ->
  Routing/NAT Offloading`.

  This can significantly improve network performance on the router, if your plan
  is 400 Mbps or higher. Most wired LAN <-> WAN traffic is then handled by the switch
  chip, bypassing the CPU (~600 Mbps with an idle CPU in tests). Wi-Fi traffic always
  goes through the CPU. Offloaded traffic also bypasses SQM, so use one or the other.
  See [OpenWRT Wiki](https://openwrt.org/docs/guide-user/perf_and_log/flow_offloading).

* **Set `System -> System -> Logging -> Log output level` to `Warning`.**

  The kernel prints its messages to the serial console at 38400 baud, and the router
  forwards nothing while a line is printed. If you save that page without picking a
  level, LuCI stores `Debug`, which prints everything.

* **Put some heatsinks** on the SoC, Wi-Fi and Switch chips.

  Their temperature can get quite high during heavy usage, and adding heatsinks can help maintain stable performance and highest possible throughput.

  Note that you'd need to put some tape on capacitors near the chips. They're higher than the chips and could touch the heatsinks, causing a short circuit.

## Overclocking

Our kernel fork can set CPU clock up to 1.2GHz on boot. Though in most tests
performance only improves by 5-7%, you may still want to try it.

Overclocking is handled during early kernel boot, so applying new changes requires reboot.
**Settings are located in** `System -> CPU clock`.

**If something goes wrong**: power the router on while holding the WPS button, and keep
holding it until OpenWRT boots. It then starts at the 1.0 GHz default CPU clock, and the
overclock setting is reset.

If the router hangs or reboots by itself, go back to 1.0 GHz before anything else.

**Additional heat sink is highly recommended if you go with overclocking**.

## ⚠️ Known limitations

* Wi-Fi traffic can't be offloaded to hardware, so the single CPU core limits Wi-Fi
  throughput to a few hundred Mbps.
* WPS (push-button pairing) isn't supported; the WPS button only turns the overclock off.
* Extra packages (`apk`) come only from this project's repository; the routing, telephony
  and video feeds aren't built.

## 📶 Wi-Fi tips

* Apple devices with AirDrop or Handoff leave the Wi-Fi channel for a moment many times a
  second. Channel 149 is the one they use for that, so a 5 GHz network on channel 149
  (where your country allows it) makes them leave less often.
* DFS channels (52-144) need a one-minute radar check before the network comes up, and a
  radar nearby moves the network to another channel.

## 🔄 Updating

* Flash `*-squashfs-sysupgrade.bin` from the [releases page](https://github.com/tdrkDev/OpenWRT-WR1500/releases)
  in `System -> Backup / Flash Firmware -> Flash image...`. Keeping settings works.
* `*-recovery.bin` is only for the bootloader's recovery mode: `sysupgrade` refuses it.
* Packages you installed yourself are gone after the update: install them again.
  Each release has its own package repository (https://packages.tdrk.dev/<release>/),
  because kernel modules must match the kernel.

## 🛟 Troubleshooting

* **LED**: blue+red = bootloader recovery mode; blinking red = OpenWRT is booting or
  updating; solid blue = ready; fast blinking red = OpenWRT failsafe mode.
* **The TFTP script says "No answer"**: check that the LED is blue+red, the cable is in
  the **WAN** port and the PC has 192.168.1.50/24 on its Ethernet adapter. Turn off Wi-Fi
  and other networks, especially if your home network is 192.168.1.x too. On Windows,
  allow Python in the firewall. Then just run the script again: the first try after
  powering on sometimes fails.
* **The router doesn't come back after flashing**: give it a few minutes, the first boot
  is slow. If the LED never turns blue, flash again in recovery mode: the bootloader is
  never overwritten, so recovery mode always works.
* **The router hangs**: a complete hang now ends in a reboot after about 35 seconds
  (watchdog). The kernel log of the boot that hung or crashed is kept in `/sys/fs/pstore/`
  until the power is cut: attach it to bug reports, with the output of `logread` and `dmesg`.

## ⚙️ WR1500 hardware

* RTL8197FH-VE5-CG SoC
  * MIPS 24Kc V8.5 single-core, 1.0 GHz
  * Integrated 128MB DDR2-533 RAM
  * Integrated Wi-Fi 4 (802.11b/g/n 2T2R)
    * Uses 2 side antennas
  * 2-wire UART
    * Requires soldering jumpers on R26 and R27
    * 38400 8N1
  * Unused:
    * 2x USB 2.0 ports
    * SPI-NAND footprint on board
* GPIO peripherals:
  * Red+blue LED on the back
  * WPS and Reset buttons on the back
* XM25QH128DHIQ 16MB SPI-NOR flash
* RTL8367RB-VC 1GbE switch (1 WAN + 3 LAN ports)
  * Connected through RGMII port
* RTL8832BR(E) PCIe 1.1 Wi-Fi 6 (802.11ac/ax)
  * 2T2R, channels up to 80 MHz wide
  * Uses 2 back antennas

## 🏭 Stock software

* OpenWRT 21.02-based proprietary firmware
* Realtek semi-proprietary kernel 4.4
  * Sources are either incomplete or very outdated
  * Modules:
    * RTL8832BR -> rtk_wifi6 (Wi-Fi 5 GHz)
    * RTL8197G -> rtl8192cd (Wi-Fi 2.4 GHz)
    * RTL8197H -> rtl819x (SoC Ethernet)
    * RTL8367RB -> rtl819x (1GbE switch)

* Flash layout:

  | Name     | Start    | End       | Size      | Description                                    |
  | -------- | -------- | --------- | --------- | ---------------------------------------------- |
  | u-boot   | 0x000000 | 0x020000  | 128 KiB   | Realtek bootloader v3.4.13                     |
  | cfg      | 0x020000 | 0x030000  | 64 KiB    | H601; Wi-Fi calibration, MACs                  |
  | bdinfo   | 0x030000 | 0x040000  | 64 KiB    | Cudy Cloud credentials, region, Wi-Fi password |
  | firmware | 0x040000 | 0xF00000  | 14.75 MiB | kernel + rootfs                                |
  | overlay  | 0xF00000 | 0xFE0000  | 896 KiB   | JFFS2 overlay                                  |
  | reserved | 0xFE0000 | 0xFF0000  | 64 KiB    | Unknown, JFFS2 with "default" file with '0'    |
  | backup   | 0xFF0000 | 0x1000000 | 64 KiB    | Unknown, empty JFFS2                           |

  OpenWRT uses `firmware` and `overlay` as one 15.6 MiB `firmware` partition and keeps
  the CPU clock setting in `backup` (as `cpuclk`). `u-boot`, `cfg`, `bdinfo` and
  `reserved` are read-only there.

* "recovery" mode
  * Hold reset button, plug in the power, wait for LED to become blue+red, release button
  * Now device accepts TFTP file `recovery.bin` upload on the WAN port, bootloader IP - 192.168.1.6.
  * After successful upload, the device will automatically flash and reboot, without signature checks.
  * It only writes the flash from 0x40000 on: `u-boot`, `cfg` and `bdinfo` stay untouched.

## 🤖 Usage of AI in the project

**This project was built with the help of Claude Opus 5.5 LLM.** I'm a developer,
I have C and Linux kernel knowledge and have done some code review of the AI-generated
contributions. This project *would not have been possible without AI tools*, cause there's
just too much work for a single person to handle alone.

If you're uncomfortable with AI-generated code, feel free to not use this project.
That's your choice anyway.
