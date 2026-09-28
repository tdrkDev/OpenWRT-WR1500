#!/usr/bin/env python3
"""TFTP-put an image to the WR1500 boot code (e.g. in reset-button recovery mode).

Self-contained: needs only Python 3.7+ standard library; runs on macOS, Linux, Windows.

Usage:
    python3 recovery_install.py <image> [router-ip]      (router-ip default 192.168.1.6)

The host needs an address on the router's subnet (e.g. 192.168.1.50/24) and a cable
on a LAN port. On Windows, allow python through the firewall (UDP) if no answer.
"""

import argparse
import os
import socket
import struct
import sys
import time

DEFAULT_IP = "192.168.1.6"
DEFAULT_NAME = "recovery.bin"
TFTP_PORT = 69
TFTP_WRQ, TFTP_DATA, TFTP_ACK, TFTP_ERR = 2, 3, 4, 5
TFTP_BLKSIZE = 512


def tftp_put(data, host, filename, timeout=2.0, retries=8):
    """Write-only TFTP client (octet mode, 512-byte blocks). Returns bytes sent."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        wrq = struct.pack("!H", TFTP_WRQ) + filename.encode() + b"\0octet\0"
        for _ in range(retries):
            sock.sendto(wrq, (host, TFTP_PORT))
            try:
                pkt, peer = sock.recvfrom(1024)
            except socket.timeout:
                continue
            except ConnectionResetError:   # Windows: ICMP port unreachable
                time.sleep(timeout)
                continue
            if len(pkt) < 4:
                continue
            op, blk = struct.unpack("!HH", pkt[:4])
            if op == TFTP_ERR:
                raise RuntimeError("TFTP error: %r" % pkt[4:].rstrip(b"\0"))
            if op == TFTP_ACK and blk == 0:
                break
        else:
            raise RuntimeError(
                "No answer from %s. Check that:\n"
                "  - the router is in recovery mode (LED blue+red),\n"
                "  - the cable is in the router's WAN port,\n"
                "  - this computer has a static 192.168.1.x/24 address on that adapter\n"
                "    and Wi-Fi/other networks are off (above all if they use 192.168.1.x),\n"
                "  - on Windows, Python is allowed in the firewall.\n"
                "Then run it again: the first try after powering on sometimes fails." % host)

        block, off = 1, 0
        t0 = time.time()
        while True:
            chunk = data[off:off + TFTP_BLKSIZE]
            pkt = struct.pack("!HH", TFTP_DATA, block & 0xFFFF) + chunk
            for _ in range(retries):
                sock.sendto(pkt, peer)
                try:
                    resp, addr = sock.recvfrom(1024)
                except (socket.timeout, ConnectionResetError):
                    continue
                if len(resp) < 4:
                    continue
                op, blk = struct.unpack("!HH", resp[:4])
                if addr == peer and op == TFTP_ACK and blk == block & 0xFFFF:
                    break
                if op == TFTP_ERR:
                    raise RuntimeError("TFTP error at block %d: %r" % (block, resp[4:]))
            else:
                raise RuntimeError("TFTP block %d not acknowledged" % block)
            off += len(chunk)
            if block % 256 == 0 or len(chunk) < TFTP_BLKSIZE:
                rate = off / max(time.time() - t0, 1e-3) / 1024
                pct = off * 100 // len(data) if data else 100
                sys.stdout.write("\r  %d/%d bytes (%d%%) %.0f KiB/s " % (off, len(data), pct, rate))
                sys.stdout.flush()
            if len(chunk) < TFTP_BLKSIZE:
                print()
                return off
            block += 1
    finally:
        sock.close()


def check_host_ip(router_ip):
    """Warn if the route to the router doesn't use an address on its /24 (no packets sent)."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect((router_ip, TFTP_PORT))
            local = s.getsockname()[0]
        finally:
            s.close()
    except OSError:
        print("WARNING: no route to %s; TFTP will fail." % router_ip)
        return
    net = router_ip.rsplit(".", 1)[0] + "."
    if local.startswith(net):
        print("Host address: %s (must be the adapter wired to the router)" % local)
    else:
        print("WARNING: route to %s uses %s, not a %sx address; TFTP will likely fail.\n"
              "  Give this computer e.g. %s50/24 on the interface wired to the router."
              % (router_ip, local, net, net))


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("image", help="firmware image to send")
    p.add_argument("router_ip", nargs="?", default=DEFAULT_IP,
                   help="boot code IP (default %s)" % DEFAULT_IP)
    p.add_argument("--name", default=DEFAULT_NAME,
                   help="remote filename in the WRQ (default %s)" % DEFAULT_NAME)
    p.add_argument("--timeout", type=float, default=2.0, help="per-packet timeout, s")
    p.add_argument("--retries", type=int, default=8, help="retries per packet")
    args = p.parse_args()

    if not os.path.isfile(args.image):
        sys.exit("No such file: %s" % args.image)
    with open(args.image, "rb") as f:
        img = f.read()
    if not img:
        sys.exit("Image is empty: %s" % args.image)
    if args.name == DEFAULT_NAME and img[0x1FC00:0x1FC08] != b"OEM_BOOT":
        print("WARNING: %s is not a recovery image (no OEM_BOOT marker at 0x1FC00);\n"
              "  the router will most likely ignore it. Send the *-squashfs-recovery.bin."
              % args.image)

    print("Image: %s (%d bytes)" % (args.image, len(img)))
    check_host_ip(args.router_ip)
    print("TFTP put -> %s:%s" % (args.router_ip, args.name))
    try:
        sent = tftp_put(img, args.router_ip, args.name, args.timeout, args.retries)
    except RuntimeError as e:
        sys.exit("\n%s" % e)
    except KeyboardInterrupt:
        sys.exit("\ninterrupted")
    print("sent %d bytes to %s" % (sent, args.router_ip))
    print("\nThe router now writes the image to flash and reboots by itself: don't power it off.\n"
          "The first boot takes a few minutes; with OpenWrt the LED turns blue when it's ready.\n"
          "Then set this computer back to automatic IP (DHCP), move the cable to a LAN port\n"
          "and open http://192.168.1.1 (user root, no password).")


if __name__ == "__main__":
    main()
