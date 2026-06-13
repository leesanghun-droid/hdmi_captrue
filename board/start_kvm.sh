#!/bin/sh
systemctl start kvm-hid
sleep 4
echo "=== is-active ==="
systemctl is-active kvm-hid
echo "=== journal (last 30) ==="
journalctl -u kvm-hid --no-pager -n 30
echo "=== gadget UDC bindings ==="
for g in /sys/kernel/config/usb_gadget/*/; do
  echo "$g UDC=$(cat "$g/UDC" 2>/dev/null)"
done
echo "=== udc state ==="
cat /sys/class/udc/fcc00000.dwc3/state 2>/dev/null
echo DONE
