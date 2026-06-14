#!/bin/bash
# kvm-drop.sh - stage the files in /opt/kvm/inbox into D:\_drop on the target's
# USB drive and drop a go.bat launcher next to them. PC1 then triggers go.bat
# over KVM, which copies the files onto the target PC's Desktop.
#
# Safety: a cleanup trap ALWAYS re-attaches the USB gadget (restores KVM) even if
# anything fails, and we verify the files fit before copying.
set -e
IMG=/opt/kvm/storage.img
MNT=/opt/kvm/mnt
INBOX=/opt/kvm/inbox
mkdir -p "$MNT" "$INBOX"

LOOP=""
cleanup() {
    sync
    umount "$MNT" 2>/dev/null || true
    [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null || true
    systemctl start kvm-hid 2>/dev/null || true   # never leave KVM dead
}
trap cleanup EXIT

# Reject single files larger than the FAT32 limit (4 GiB) up front.
BIG=$(find "$INBOX" -type f -size +4294967040c -print -quit 2>/dev/null || true)
if [ -n "$BIG" ]; then
    echo "ERROR: '$BIG' exceeds the FAT32 4GB single-file limit"; exit 2
fi

systemctl stop kvm-hid
sleep 2
for g in /sys/kernel/config/usb_gadget/*/; do
    [ -e "$g/UDC" ] && echo "" > "$g/UDC" 2>/dev/null || true
done
losetup -j "$IMG" 2>/dev/null | cut -d: -f1 | xargs -r losetup -d || true
LOOP=$(losetup --find --show -P "$IMG")
sleep 1
mount "${LOOP}p1" "$MNT"

# Make sure the payload fits in the drive's free space.
NEED=$(du -sb "$INBOX" 2>/dev/null | cut -f1); NEED=${NEED:-0}
FREE=$(df -B1 --output=avail "$MNT" 2>/dev/null | tail -1); FREE=${FREE:-0}
if [ "$NEED" -gt "$FREE" ]; then
    echo "ERROR: need $NEED bytes but drive has only $FREE free"; exit 3
fi

rm -rf "$MNT/_drop"
mkdir -p "$MNT/_drop"
cp -rf "$INBOX"/. "$MNT/_drop"/

# Windows launcher: copy everything in this folder (except itself) to the Desktop.
printf '@echo off\r\nfor %%%%f in ("%%~dp0*") do if /i not "%%%%~nxf"=="go.bat" copy /y "%%%%f" "%%USERPROFILE%%\\Desktop\\" >nul\r\n' > "$MNT/_drop/go.bat"

echo "staged $(find "$INBOX" -type f | wc -l) file(s) into D:\\_drop + go.bat"
# cleanup trap unmounts, detaches, and restarts the gadget on exit
