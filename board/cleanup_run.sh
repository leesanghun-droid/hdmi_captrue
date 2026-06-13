#!/bin/sh
systemctl stop kvm-hid 2>/dev/null
systemctl disable kvm-hid 2>/dev/null
# Clean leftover temp gadgets (g_*) from the restart loop.
for g in /sys/kernel/config/usb_gadget/g_*; do
  [ -d "$g" ] || continue
  echo "cleaning $g"
  echo "" > "$g/UDC" 2>/dev/null
  for f in "$g"/functions/mass_storage.*; do
    [ -d "$f" ] && echo "" > "$f/lun.0/file" 2>/dev/null
  done
  for l in "$g"/configs/*/f*; do
    [ -L "$l" ] && rm -f "$l"
  done
  for d in "$g"/configs/*/strings/*; do [ -d "$d" ] && rmdir "$d" 2>/dev/null; done
  for d in "$g"/configs/*; do [ -d "$d" ] && rmdir "$d" 2>/dev/null; done
  for d in "$g"/functions/*; do [ -d "$d" ] && rmdir "$d" 2>/dev/null; done
  for d in "$g"/strings/*; do [ -d "$d" ] && rmdir "$d" 2>/dev/null; done
  rmdir "$g" 2>/dev/null
done
echo "" > /sys/kernel/config/usb_gadget/rockchip/UDC 2>/dev/null
echo "=== udc state ==="
cat /sys/class/udc/fcc00000.dwc3/state 2>/dev/null
echo "=== gadgets present ==="
ls /sys/kernel/config/usb_gadget/
echo "=== run daemon (timeout 8s, full output) ==="
cd /
timeout 8 python3 /opt/kvm/kvm_hid.py 2>&1
echo "rc=$?"
echo DONE
