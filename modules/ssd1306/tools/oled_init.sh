#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# oled_init.sh - SSD1306 128x32 bring-up from user space (stage 1)
#
# Usage:
#   ./oled_init.sh            # init + clear
#   ./oled_init.sh fill FF    # init + fill every byte with 0xFF
#   ./oled_init.sh ramp       # init + incrementing pattern
#   ./oled_init.sh off        # display off (what remove() will do)
#
# Control byte: 0x00 = commands follow, 0x40 = GDDRAM data follows
# Tip: if ACKs are fine but nothing lights up, power-cycle the module
#      (the on-board RC reset may not have fired cleanly).

BUS=${BUS:-1}
ADDR=${ADDR:-0x3c}

cmd()  { i2ctransfer -y "$BUS" "w$(( $# + 1 ))@$ADDR" 0x00 "$@"; }

init() {
	cmd 0xAE \
	    0xD5 0x80 \
	    0xA8 0x1F \
	    0xD3 0x00 \
	    0x40 \
	    0x8D 0x14 \
	    0x20 0x00 \
	    0xA1 0xC8 \
	    0xDA 0x02 \
	    0x81 0x8F \
	    0xD9 0xF1 \
	    0xDB 0x40 \
	    0xA4 0xA6 0x2E \
	    0xAF
}

window() { cmd 0x21 0x00 0x7F 0x22 0x00 0x03; }

# $1 = i2ctransfer data spec for 512 bytes, e.g. "0x00=" or "0x00+"
frame() { window && i2ctransfer -y "$BUS" "w513@$ADDR" 0x40 "$1"; }

set -e
case "${1:-clear}" in
off)	cmd 0xAE ;;
fill)	init; frame "0x${2:-FF}=" ;;
ramp)	init; frame "0x00+" ;;
clear)	init; frame "0x00=" ;;
*)	echo "usage: $0 [clear|fill XX|ramp|off]" >&2; exit 1 ;;
esac
