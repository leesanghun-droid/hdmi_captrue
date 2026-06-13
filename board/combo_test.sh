#!/bin/sh
systemctl stop kvm-hid 2>/dev/null
for g in /sys/kernel/config/usb_gadget/g_*; do
  [ -d "$g" ] || continue
  echo "" > "$g/UDC" 2>/dev/null
  for f in "$g"/functions/mass_storage.*; do [ -d "$f" ] && echo "" > "$f/lun.0/file" 2>/dev/null; done
  for l in "$g"/configs/*/f*; do [ -L "$l" ] && rm -f "$l"; done
  for d in "$g"/configs/*/strings/*; do [ -d "$d" ] && rmdir "$d" 2>/dev/null; done
  for d in "$g"/configs/*; do [ -d "$d" ] && rmdir "$d" 2>/dev/null; done
  for d in "$g"/functions/*; do [ -d "$d" ] && rmdir "$d" 2>/dev/null; done
  for d in "$g"/strings/*; do [ -d "$d" ] && rmdir "$d" 2>/dev/null; done
  rmdir "$g" 2>/dev/null
done
echo "" > /sys/kernel/config/usb_gadget/rockchip/UDC 2>/dev/null
echo "=== who holds storage.img ==="
fuser -v /opt/kvm/storage.img 2>&1 || echo "(fuser: none/na)"
cd /
echo "########## RUN A: HID then MSC (default order) ##########"
timeout 8 python3 /opt/kvm/kvm_hid.py 2>&1 | head -40
echo "rcA done"
echo "########## RUN B: MSC first ##########"
KVM_MSC_FIRST=1 timeout 8 python3 /opt/kvm/kvm_hid.py 2>&1 | head -40
echo "rcB done"
echo DONE
