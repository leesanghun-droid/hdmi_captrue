#!/bin/sh
# Deploy the KVM HID gadget: install daemon, create storage image, install
# systemd unit, disable adbd (frees the OTG UDC for our gadget).
set -e
mkdir -p /opt/kvm
cp /tmp/kvm_hid.py /opt/kvm/kvm_hid.py
# File-transfer helper (push/pull files to the target's USB drive).
[ -f /tmp/kvm-share.sh ] && cp /tmp/kvm-share.sh /opt/kvm/kvm-share.sh && chmod +x /opt/kvm/kvm-share.sh

# dosfstools (mkfs.vfat) is needed to create a Windows-friendly FAT32; parted
# builds the MBR partition table. Best-effort install (needs internet once).
if ! command -v mkfs.vfat >/dev/null 2>&1; then
  apt-get install -y dosfstools parted >/dev/null 2>&1 || \
    { apt-get update -y >/dev/null 2>&1; apt-get install -y dosfstools parted >/dev/null 2>&1; } || true
fi

if [ ! -f /opt/kvm/storage.img ]; then
  echo "creating 8GB storage image (MBR + FAT32)..."
  # sparse file: instant to create, only consumes space as data is written
  truncate -s 8G /opt/kvm/storage.img
  if command -v mkfs.vfat >/dev/null 2>&1 && command -v parted >/dev/null 2>&1; then
    # Windows wants a partition table; a bare "superfloppy" FAT is often rejected.
    parted -s /opt/kvm/storage.img mklabel msdos
    parted -s /opt/kvm/storage.img mkpart primary fat32 1MiB 100%
    parted -s /opt/kvm/storage.img set 1 lba on
    # parted may tag the partition 0x83; force 0x0c (W95 FAT32 LBA) so Windows mounts it.
    printf '\x0c' | dd of=/opt/kvm/storage.img bs=1 seek=450 count=1 conv=notrunc status=none
    LP=$(losetup --find --show -P /opt/kvm/storage.img); sleep 1
    mkfs.vfat -F 32 -n KVMSHARE "${LP}p1" >/dev/null 2>&1 && echo "formatted FAT32" || echo "format failed"
    losetup -d "$LP" || true
  else
    echo "mkfs.vfat/parted not found; leaving raw image (format on target)"
  fi
else
  echo "storage image already exists"
fi

cat > /etc/systemd/system/kvm-hid.service <<'EOF'
[Unit]
Description=RK3568 KVM HID + Mass Storage gadget
After=network.target
Conflicts=adbd.service

[Service]
Type=simple
ExecStartPre=/bin/sh -c 'systemctl stop adbd 2>/dev/null || true; if [ -e /sys/kernel/config/usb_gadget/rockchip/UDC ]; then echo "" > /sys/kernel/config/usb_gadget/rockchip/UDC 2>/dev/null || true; fi; sleep 1'
ExecStart=/usr/bin/python3 /opt/kvm/kvm_hid.py
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
# Free the OTG UDC from adbd and make the gadget start on every boot.
systemctl disable --now adbd 2>/dev/null || true
systemctl enable kvm-hid
systemctl restart kvm-hid
echo "deploy done (kvm-hid enabled + started; persists across reboot)"
systemctl is-active kvm-hid
ls -la /opt/kvm/
