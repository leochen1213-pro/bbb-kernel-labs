#!/bin/sh
# Experiment A: in every round two writers change the scale at the same
# time (one write each), then check that the cached scale matches
# DATA_FORMAT (reg 0x31).
# usage: sh exp_a.sh [rounds]
R=/sys/kernel/debug/regmap/spi1.0/registers
for d in /sys/bus/iio/devices/iio:device*; do
	[ "$(cat "$d/name" 2>/dev/null)" = "leo-adxl345" ] && D=$d
done
[ -n "$D" ] || { echo "leo-adxl345 not found" >&2; exit 1; }
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug

bad=0
n=${1:-10}
r=1
while [ $r -le "$n" ]; do
	# one write each, started together: only these two can interleave,
	# no later write can "heal" the result
	echo 0.038300 > "$D/in_accel_scale" &
	echo 0.306400 > "$D/in_accel_scale" &
	wait

	scale=$(cat "$D/in_accel_scale")
	reg=$(grep '^31:' $R | cut -d' ' -f2)
	case "$scale:$reg" in
	0.038300:00|0.306400:03) res=ok ;;
	*) res=MISMATCH; bad=$((bad + 1)) ;;
	esac
	z=$(cat "$D/in_accel_z_raw")
	awk -v r=$r -v s=$scale -v g=$reg -v z=$z -v t=$res 'BEGIN {
		printf "round %2d: scale %s  reg31 %s  z_raw %4d  -> %7.3f m/s^2  %s\n",
		       r, s, g, z, z * s, t }'
	r=$((r + 1))
done
echo "mismatch: $bad / $n"
