#!/bin/sh
# Deploy the KVM HID gadget: install daemon, create storage image, install
# systemd unit, disable adbd (frees the OTG UDC for our gadget).
set -e
mkdir -p /opt/kvm
cp /tmp/kvm_hid.py /opt/kvm/kvm_hid.py

if [ ! -f /opt/kvm/storage.img ]; then
  echo "creating 512MB storage image..."
  dd if=/dev/zero of=/opt/kvm/storage.img bs=1M count=512 status=none
  if command -v mkfs.vfat >/dev/null 2>&1; then
    mkfs.vfat -F 32 -n KVMDISK /opt/kvm/storage.img >/dev/null 2>&1 && echo "formatted FAT32" || echo "format failed (raw image)"
  else
    echo "mkfs.vfat not found; leaving raw image (format on target)"
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
echo "deploy done"
ls -la /opt/kvm/
