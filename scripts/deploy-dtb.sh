#!/bin/sh
# deploy-dtb.sh - build the custom BBB DTB, install it on the SD boot
# partition, reboot the board and verify the new DTB is live.
#
# Usage: ./deploy-dtb.sh [dt-node-to-verify]
#        e.g. ./deploy-dtb.sh temperature-sensor@48

set -eu

BOARD=${BOARD:-root@192.168.50.100}
KDIR=${KDIR:-$HOME/linux-kernel-bbb-labs/src/linux}
DTB_TARGET=ti/omap/am335x-boneblack-custom.dtb
DTB_NAME=$(basename "$DTB_TARGET")
BOOTDEV=${BOOTDEV:-/dev/mmcblk0p1}
CHECK_NODE=${1:-}

# Always pass ARCH/CROSS_COMPILE: without them kbuild uses the host
# architecture (arm64) and rewrites the ARM .config.
make -C "$KDIR" ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- "$DTB_TARGET"

DTB_PATH="$KDIR/arch/arm/boot/dts/$DTB_TARGET"
SUM=$(md5sum "$DTB_PATH" | cut -d' ' -f1)
echo ">>> local md5: $SUM"

scp -O "$DTB_PATH" "$BOARD:/root/$DTB_NAME"

# Remote part. Local variables are expanded here; \$ is evaluated on the board.
# reboot drops the connection (ssh exits 255); any other failure is real.
set +e
ssh "$BOARD" sh -s <<EOF
set -e
mkdir -p /mnt/boot
grep -q ' /mnt/boot ' /proc/mounts || mount $BOOTDEV /mnt/boot
cd /mnt/boot
if [ ! -f $DTB_NAME ]; then
    echo "!!! $DTB_NAME not found on $BOOTDEV - wrong partition?" >&2
    cd /; umount /mnt/boot; exit 1
fi
# keep the first known-good copy; never overwrite it
[ -f $DTB_NAME.good ] || cp $DTB_NAME $DTB_NAME.good
cp /root/$DTB_NAME $DTB_NAME
REMOTE_SUM=\$(md5sum $DTB_NAME | cut -d' ' -f1)
if [ "\$REMOTE_SUM" != "$SUM" ]; then
    echo "!!! md5 mismatch: \$REMOTE_SUM" >&2
    cd /; umount /mnt/boot; exit 1
fi
echo ">>> board md5 OK"
sync
cd /
umount /mnt/boot
echo ">>> rebooting"
reboot
EOF
rc=$?
set -e
if [ "$rc" -ne 0 ] && [ "$rc" -ne 255 ]; then
    echo "!!! remote install failed (rc=$rc), board NOT rebooted" >&2
    exit 1
fi

echo ">>> waiting for board to come back..."
sleep 5
until ssh -o ConnectTimeout=2 -o BatchMode=yes "$BOARD" true 2>/dev/null; do
    sleep 2
done
echo ">>> board is up"

if [ -n "$CHECK_NODE" ]; then
    # trailing slash: /proc/device-tree is a symlink and find does not
    # descend into a symlinked starting point without it
    if ssh "$BOARD" "find /proc/device-tree/ -name '$CHECK_NODE'" | grep -q .; then
        echo ">>> OK: $CHECK_NODE present in live device tree"
    else
        echo "!!! $CHECK_NODE NOT found in /proc/device-tree - old DTB loaded?" >&2
        exit 1
    fi
fi
