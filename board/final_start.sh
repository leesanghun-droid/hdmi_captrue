#!/bin/sh
systemctl disable adbd 2>/dev/null
systemctl enable kvm-hid 2>/dev/null
systemctl restart kvm-hid
sleep 5
echo "=== is-active ==="
systemctl is-active kvm-hid
echo "=== udc state ==="
cat /sys/class/udc/fcc00000.dwc3/state 2>/dev/null
echo "=== bound gadget ==="
for g in /sys/kernel/config/usb_gadget/g_*; do echo "$g UDC=$(cat "$g/UDC" 2>/dev/null)"; done
echo "=== journal ==="
journalctl -u kvm-hid --no-pager -n 8
echo DONE
