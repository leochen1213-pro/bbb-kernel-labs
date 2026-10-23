#!/bin/sh
# Print the ADXL345 axes in raw LSB and m/s^2 (stage 3.4+)
for d in /sys/bus/iio/devices/iio:device*; do
	[ "$(cat "$d/name" 2>/dev/null)" = "leo-adxl345" ] && D=$d
done
[ -n "$D" ] || { echo "leo-adxl345 not found" >&2; exit 1; }

s=$(cat "$D/in_accel_scale")
echo "$D  scale=$s"
for a in x y z; do
	r=$(cat "$D/in_accel_${a}_raw")
	awk -v a="$a" -v r="$r" -v s="$s" \
	    'BEGIN { printf "%s %6d LSB %9.3f m/s^2\n", a, r, r * s }'
done
