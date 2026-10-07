#!/bin/sh
# lm75.sh - LM75B user-space probe (runs on BusyBox ash)
#
# Usage:
#   ./lm75.sh                 dump Conf / Temp / Thyst / Tos
#   ./lm75.sh set tos 30      set Tos   (integer degC)
#   ./lm75.sh set thyst 28    set Thyst (integer degC)
#
# Env: BUS (default 1), ADDR (default 0x48)
#
# Note: SMBus "word" is little-endian on the wire, LM75 sends MSByte first,
#       so every i2cget/i2cset word value must be byte-swapped.

BUS=${BUS:-1}
ADDR=${ADDR:-0x48}

swab16() { echo $(( (($1 & 0xff) << 8) | (($1 >> 8) & 0xff) )); }

# $1 = 16-bit register value (MSB:LSB), $2 = valid bits (Temp=11, Tos/Thyst=9)
# prints millidegree Celsius (same unit as hwmon temp1_input)
to_mc() {
    bits=$2
    v=$(( $1 >> (16 - bits) ))
    [ $v -ge $(( 1 << (bits - 1) )) ] && v=$(( v - (1 << bits) ))
    echo $(( v * 1000 / (1 << (bits - 8)) ))
}

# Like a driver: check every transfer, never compute on a failed read.
rd() {
    v=$(i2cget -y $BUS $ADDR $1 w) || return 1
    swab16 "$v"
}

# Fail early with a useful hint instead of a cascade of arithmetic errors.
if ! i2cget -y $BUS $ADDR 0x01 b >/dev/null 2>&1; then
    echo "lm75.sh: cannot access 0x${ADDR#0x} on i2c-$BUS" >&2
    if [ -e /sys/bus/i2c/devices/$BUS-00${ADDR#0x}/driver ]; then
        echo "  bound to kernel driver: $(basename "$(readlink /sys/bus/i2c/devices/$BUS-00${ADDR#0x}/driver)")" >&2
        echo "  use /sys/class/hwmon/*/temp1_* instead, or rmmod the driver" >&2
    fi
    exit 1
fi

# $1 = register, $2 = integer degC
wr_c() {
    r=$(( ($2 << 8) & 0xffff ))
    i2cset -y $BUS $ADDR $1 "$(printf 0x%04x $(swab16 $r))" w
}

case "$1" in
set)
    case "$2" in
    tos)   wr_c 0x03 "$3" ;;
    thyst) wr_c 0x02 "$3" ;;
    *) echo "usage: $0 set {tos|thyst} <degC>"; exit 1 ;;
    esac
    ;;
*)
    t=$(rd 0x00)  || exit 1
    hy=$(rd 0x02) || exit 1
    os=$(rd 0x03) || exit 1
    conf=$(i2cget -y $BUS $ADDR 0x01 b) || exit 1
    printf 'Conf  = %s\n' "$conf"
    printf 'Temp  = 0x%04x -> %6d mC\n' $t  $(to_mc $t 11)
    printf 'Thyst = 0x%04x -> %6d mC\n' $hy $(to_mc $hy 9)
    printf 'Tos   = 0x%04x -> %6d mC\n' $os $(to_mc $os 9)
    ;;
esac
