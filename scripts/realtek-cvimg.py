#!/usr/bin/env python3
"""
# SPDX-License-Identifier: GPL-2.0-or-later
#
# realtek-cvimg.py: Adds the image header of the Realtek RTL819x boot code
#
# The Realtek boot code of RTL819x router SoCs (RTL8196, RTL8197, ...) finds
# the kernel in flash by this header, verifies the checksum, copies the
# payload to the start address and jumps to it. The format is the one of
# Realtek's "cvimg" tool:
#
#   0x00  signature, e.g. "cs6c" (Linux kernel)
#   0x04  start address (be32), where the payload is copied to and started
#   0x08  burn address (be32), the flash address the image is meant for
#   0x0c  length (be32) of the payload and the checksum
#   0x10  payload, padded to an even length
#   ...   checksum (be16), makes the sum of all be16 words of the payload
#         and the checksum zero
"""

import argparse
import struct

SIGNATURE_LEN = 4


def auto_int(x):
	return int(x, 0)


def checksum(data):
	words = struct.unpack(">%dH" % (len(data) // 2), data)
	return -sum(words) & 0xffff


def main():
	parser = argparse.ArgumentParser(
		description="Add a Realtek RTL819x boot code image header")
	parser.add_argument("--signature", default="cs6c",
			    help="image signature (default: cs6c)")
	parser.add_argument("--start-addr", type=auto_int, required=True,
			    help="RAM address to copy the payload to and start")
	parser.add_argument("--burn-addr", type=auto_int, required=True,
			    help="flash address of the image")
	parser.add_argument("input_file")
	parser.add_argument("output_file")
	args = parser.parse_args()

	signature = args.signature.encode("ascii")
	if len(signature) != SIGNATURE_LEN:
		parser.error("the signature must be %d characters" % SIGNATURE_LEN)

	with open(args.input_file, "rb") as f:
		payload = f.read()
	if len(payload) % 2:
		payload += b"\0"

	header = signature + struct.pack(">LLL", args.start_addr,
					 args.burn_addr, len(payload) + 2)
	with open(args.output_file, "wb") as f:
		f.write(header)
		f.write(payload)
		f.write(struct.pack(">H", checksum(payload)))


if __name__ == "__main__":
	main()
