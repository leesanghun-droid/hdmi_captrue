#!/bin/bash
# kvm-share.sh - move files between the board and the target PC's USB drive (D:).
#
# The board exposes /opt/kvm/storage.img to the target as a removable USB drive.
# That image is a block-level disk owned by the target while connected, so we
# must briefly detach it (stop the gadget) before the board may safely read or
# write it, then re-attach so the target re-reads the new contents. While the
# gadget is stopped the KVM keyboard/mouse drop for a few seconds and recover.
#
# Usage:
#   kvm-share.sh push <path...>   copy files/dirs INTO the drive (appear on target D:)
#   kvm-share.sh pull             copy the whole drive OUT to /opt/kvm/outbox
#   kvm-share.sh list             list what is currently on the drive
#
# Typical PC1 -> target flow:
#   scp -i ~/.ssh/kvm myfile.zip root@192.168.0.8:/tmp/
#   ssh -i ~/.ssh/kvm root@192.168.0.8 '/opt/kvm/kvm-share.sh push /tmp/myfile.zip'
set -e

IMG=/opt/kvm/storage.img
MNT=/opt/kvm/mnt
OUT=/opt/kvm/outbox
mkdir -p "$MNT" "$OUT"

LOOP=""

# Safety net: whatever happens, leave the gadget running so KVM never stays dead.
cleanup() {
    sync
    umount "$MNT" 2>/dev/null || true
    [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null || true
    systemctl start kvm-hid 2>/dev/null || true
}
trap cleanup EXIT

stop_gadget() {
    systemctl stop kvm-hid
    sleep 2
    # ensure no gadget still bound and no stale loop holds the image
    for g in /sys/kernel/config/usb_gadget/*/; do
        [ -e "$g/UDC" ] && echo "" > "$g/UDC" 2>/dev/null || true
    done
    losetup -j "$IMG" 2>/dev/null | cut -d: -f1 | xargs -r losetup -d || true
}

start_gadget() { systemctl start kvm-hid; }

mount_img() {
    LOOP=$(losetup --find --show -P "$IMG")
    sleep 1
    mount "${LOOP}p1" "$MNT"
}

umount_img() {
    sync
    umount "$MNT" 2>/dev/null || true
    [ -n "$LOOP" ] && losetup -d "$LOOP" 2>/dev/null || true
}

case "${1:-}" in
    push)
        shift
        [ $# -ge 1 ] || { echo "push needs at least one path"; exit 1; }
        stop_gadget; mount_img
        cp -rfv "$@" "$MNT"/
        umount_img; start_gadget
        echo "pushed; target will re-read D: in a few seconds"
        ;;
    pull)
        stop_gadget; mount_img
        cp -rfv "$MNT"/. "$OUT"/ 2>/dev/null || true
        umount_img; start_gadget
        echo "pulled drive contents into $OUT"
        ;;
    list)
        stop_gadget; mount_img
        ls -la "$MNT"
        umount_img; start_gadget
        ;;
    reverse)
        # Target agent staged file(s) into D:\_pull; copy them out to revout for
        # PC1 to fetch, then clear _pull. One stop-window (one KVM blip).
        REV=/opt/kvm/revout; mkdir -p "$REV"
        stop_gadget; mount_img
        if [ -d "$MNT/_pull" ]; then
            find "$MNT/_pull" -maxdepth 1 -type f ! -name '.tmp*' -exec cp -f {} "$REV"/ \; 2>/dev/null || true
            rm -f "$MNT/_pull"/* 2>/dev/null || true
        fi
        umount_img; start_gadget
        echo "reverse done: $(find "$REV" -maxdepth 1 -type f 2>/dev/null | wc -l) file(s) staged"
        ;;
    sync)
        # One stop-window bidirectional reconcile used by the PC1 auto-sync daemon:
        #   1. copy everything staged in /opt/kvm/inbox INTO the drive (PC1 -> target)
        #   2. mirror the whole drive OUT to /opt/kvm/outbox      (target -> PC1)
        # The gadget is stopped only once per cycle, so the target's D: blips a
        # single time regardless of how many files move in either direction.
        INBOX=/opt/kvm/inbox
        mkdir -p "$INBOX"
        stop_gadget; mount_img
        if [ -n "$(ls -A "$INBOX" 2>/dev/null)" ]; then
            cp -rf "$INBOX"/. "$MNT"/ 2>/dev/null || true
        fi
        cp -rf "$MNT"/. "$OUT"/ 2>/dev/null || true
        umount_img; start_gadget
        echo "sync done"
        ;;
    *)
        echo "usage: $0 {push <path...>|pull|list|sync}"
        exit 1
        ;;
esac
