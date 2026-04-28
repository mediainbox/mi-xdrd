#!/bin/bash
# Adapted from balena-io-library/base-images/scripts/assets/entry.sh
#
# When UDEV=on and the container is privileged, remount /dev as devtmpfs
# and start udevd so hot-plugged USB devices appear inside the container.
# Without this, libusb-based tools fail with LIBUSB_ERROR_NO_DEVICE for
# devices that appeared after container start.

tmp_mount='/tmp/_balena'
mkdir -p "$tmp_mount"
if mount -t devtmpfs none "$tmp_mount" &> /dev/null; then
    PRIVILEGED=true
    umount "$tmp_mount"
else
    PRIVILEGED=false
fi
rm -rf "$tmp_mount"

mount_dev() {
    tmp_dir='/tmp/tmpmount'
    mkdir -p "$tmp_dir"
    mount -t devtmpfs none "$tmp_dir"
    mkdir -p "$tmp_dir/shm"
    mount --move /dev/shm "$tmp_dir/shm"
    mkdir -p "$tmp_dir/mqueue"
    mount --move /dev/mqueue "$tmp_dir/mqueue"
    mkdir -p "$tmp_dir/pts"
    mount --move /dev/pts "$tmp_dir/pts"
    touch "$tmp_dir/console"
    mount --move /dev/console "$tmp_dir/console"
    umount /dev || true
    mount --move "$tmp_dir" /dev

    ln -sf /dev/pts/ptmx /dev/ptmx

    sysfs_dir='/sys/kernel/debug'
    if ! mountpoint -q "$sysfs_dir"; then
        mount -t debugfs nodev "$sysfs_dir" || true
    fi
}

start_udev() {
    if [ "$UDEV" == "on" ]; then
        if $PRIVILEGED; then
            mount_dev
            if command -v udevd &>/dev/null; then
                unshare --net udevd --daemon &> /dev/null
            else
                unshare --net /lib/systemd/systemd-udevd --daemon &> /dev/null
            fi
            udevadm trigger &> /dev/null
            echo "[entry.sh] udev started, /dev remounted as devtmpfs"
        else
            echo "[entry.sh] UDEV=on but container not privileged — skipping udev setup"
        fi
    fi
}

UDEV=$(echo "${UDEV:-}" | awk '{print tolower($0)}')
case "$UDEV" in
    '1' | 'true')
        UDEV='on'
    ;;
esac

start_udev
exec "$@"
