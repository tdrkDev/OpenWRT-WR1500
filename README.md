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
* Modern OpenWRT instead of stock core's 21.02 one
* Focused on improving performance and stability over stock code,
  considering potato CPU
* OSS `rtw88` and `rtw89` Wi-Fi driver forks providing better throughput and stability
* OSS `rtl8365mb` Ethernet driver fork providing modern kernel support

## ⬇️ Installation

> **⚠️ IMPORTANT NOTICE**: Due to limited bootloader, backup of stock flash
> partitions at somewhat reasonable speed **IS NOT POSSIBLE**.
> This software is provided **AS IS** and if your router breaks and
> you'll lose your MAC/Cudy credentials, only you are responsible for recovering it

### Easy Way

1. Ensure you have Python 3 installed
2. Download archive from the [releases page](https://github.com/tdrkDev/OpenWRT-WR1500/releases)
3. Connect PC to the WAN port of the router
4. Set a static IP on your PC, e.g., 192.168.1.50/24
5. Hold RESET and power on the router, keep holding until the LED becomes blue+red for more than 5 seconds, then release the button
6. Run `tftp.sh` (Linux/Mac) or `tftp.bat` (Windows)
7. Reconnect Ethernet to LAN or use Wi-Fi next. See `Router Credentials`.

### Manual Way

1. Install TFTP client on your PC.
2. Download `*-recovery.bin` file from the [releases page](https://github.com/tdrkDev/OpenWRT-WR1500/releases).
3. Connect PC to the WAN port of the router.
4. Set a static IP on your PC, e.g., 192.168.1.50/24.
5. Hold RESET and power on the router, keep holding until the LED becomes blue+red for more than 5 seconds, then release the button.
6. Use TFTP client to upload the `*-recovery.bin` file to the router as `recovery.bin`.
7. Reconnect Ethernet to LAN or use Wi-Fi next. See `Router Credentials`.

### Router Credentials

* User: `root`, no password.
* Router IP: `192.168.1.1`
* Wi-Fi: See the back panel of your router

## ℹ️ Highly recommended

* Enable **Hardware Flow Offloading**.

  This can significantly improve network performance on the router, if your plan
  is 400 Mbps or higher. This makes traffic bypass CPU in much of the cases.
  See [OpenWRT Wiki](https://openwrt.org/docs/guide-user/perf_and_log/flow_offloading).

* **Put some heatsinks** on the SoC, Wi-Fi and Switch chips.

  Their temperature can get quite high during heavy usage, and adding heatsinks can help maintain stable performance and highest possible throughput. CPU starts to throttle at 75°C.

  Note that you'd need to put some tape on capacitors near the chips. They're higher than the chips and could touch the heatsinks, causing a short circuit.

## ⚙️ WR1500 hardware

* RT8197FH-VE5-CG SoC
  * MIPS 24Kc V8.5 single-core, 1.0 GHz
  * Integrated 128MB DDR2-533 RAM
  * Integrated Wi-Fi 4 (802.11b/g/n 2T2R)
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
* RTL8367RB 1GbE (1 WAN + 3 LAN ports)

  Connected through RGMII port
* RTL8832BR(E) PCIe 1.1 Wi-Fi 6 (802.11ac/ax)

## 🏭 Stock software

* OpenWRT 21.02-based proprietary firmware
* Realtek semi-proprietary kernel 4.4
  * Modules:
    * RTL8832BR -> rtk_wifi6 (Wi-Fi 5 GHz)
    * RTL8197G -> rtl8192cd (Wi-Fi 2.4 GHz)
    * RTL8197H -> rtl819x (SoC Ethernet)
    * RTL8367RB -> rtl819x (1GbE switch)

* Flash layout:

  | Name     | Start    | End      | Size      | Description                                 |
  | -------- | -------- | -------- | --------- | ------------------------------------------- |
  | u-boot   | 0x000000 | 0x020000 | 128 KiB   | Realtek bootloader v3.4.13                  |
  | cfg      | 0x020000 | 0x030000 | 64 KiB    | H601; Wi-Fi calibration, MACs               |
  | bdinfo   | 0x030000 | 0x040000 | 64 KiB    | Cudy Cloud encrypted credentials            |
  | firmware | 0x040000 | 0xF00000 | 14.75 MiB | kernel + rootfs                             |
  | overlay  | 0xF00000 | 0xFE0000 | 896 KiB   | JFFS2 overlay                               |
  | reserved | 0xFE0000 | 0xFF0000 | 64 KiB    | Unknown, JFFS2 with "default" file with '0' |
  | backup   | 0xFF0000 | 0x100000 | 64 KiB    | Unknown, empty JFFS2                        |

* "recovery" mode
  * Hold reset button, plug in the power, wait for LED to become blue+red, release button
  * Now device accepts TFTP file `recovery.bin` upload on the WAN port, bootloader IP - 192.168.1.6.
  * After successful upload, the device will automatically flash and reboot, without signature checks.

## 🤖 Usage of AI in the project

**This project was built with the help of Claude Opus 5.5 LLM.** I'm a developer,
I have C and Linux kernel knowledge and have done some code review of the AI-generated
contributions. This project *would not have been possible without AI tools*, cause there's
just too much work for a single person to handle alone.

If you're uncomfortable with AI-generated code, feel free to not use this project.
That's your choice anyway.
