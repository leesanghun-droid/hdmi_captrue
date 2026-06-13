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
cd /
KVM_WITH_MSC=0 timeout 12 python3 /opt/kvm/kvm_hid.py >/tmp/hidonly.log 2>&1 &
PYPID=$!
sleep 5
echo "=== udc state mid-run ==="
cat /sys/class/udc/fcc00000.dwc3/state 2>/dev/null
echo "=== gadgets ==="
ls /sys/kernel/config/usb_gadget/
for g in /sys/kernel/config/usb_gadget/g_*; do echo "$g UDC=$(cat "$g/UDC" 2>/dev/null)"; done
wait $PYPID
echo "=== daemon log ==="
cat /tmp/hidonly.log
echo DONE
