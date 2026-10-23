#!/bin/sh
# Load / unload the ADXL345 modules in dependency order (stage 3.6+)
#   sh load.sh spi [core params]   e.g. sh load.sh spi bug=1
#   sh load.sh i2c [core params]
#   sh load.sh both [core params]
#   sh load.sh unload
DIR=$(dirname "$0")

case "$1" in
spi|i2c|both)
	bus=$1; shift
	lsmod | grep -q '^leo_adxl345_core' || insmod "$DIR/leo_adxl345_core.ko" "$@" || exit 1
	[ "$bus" != i2c ] && insmod "$DIR/leo_adxl345_spi.ko"
	[ "$bus" != spi ] && insmod "$DIR/leo_adxl345_i2c.ko"
	;;
unload)
	# bus modules first: they use the core
	lsmod | grep -q '^leo_adxl345_spi' && rmmod leo_adxl345_spi
	lsmod | grep -q '^leo_adxl345_i2c' && rmmod leo_adxl345_i2c
	lsmod | grep -q '^leo_adxl345_core' && rmmod leo_adxl345_core
	;;
*)
	echo "usage: $0 spi|i2c|both [core params] | unload" >&2
	exit 1
	;;
esac
lsmod | grep leo_adxl345
