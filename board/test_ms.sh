#!/bin/sh
systemctl stop kvm-hid 2>/dev/null
echo "" > /sys/kernel/config/usb_gadget/rockchip/UDC 2>/dev/null
G=/sys/kernel/config/usb_gadget/test_ms
# best-effort cleanup of a previous run
if [ -d "$G" ]; then
  echo "" > "$G/UDC" 2>/dev/null
  rm -f "$G/configs/c.1/f1" 2>/dev/null
  rmdir "$G/configs/c.1/strings/0x409" 2>/dev/null
  rmdir "$G/configs/c.1" 2>/dev/null
  rmdir "$G/functions/mass_storage.0" 2>/dev/null
  rmdir "$G/strings/0x409" 2>/dev/null
  rmdir "$G" 2>/dev/null
fi
mkdir -p "$G"
echo 0x1d6b > "$G/idVendor"
echo 0x0104 > "$G/idProduct"
mkdir -p "$G/strings/0x409"
echo "0001" > "$G/strings/0x409/serialnumber"
echo "DIY" > "$G/strings/0x409/manufacturer"
echo "MS test" > "$G/strings/0x409/product"
mkdir -p "$G/functions/mass_storage.0"
echo "=== lun.0 dir ==="
ls -la "$G/functions/mass_storage.0/" 2>/dev/null
echo "=== set removable ==="
echo 1 > "$G/functions/mass_storage.0/lun.0/removable" 2>&1; echo "removable rc=$?"
echo "=== set file ==="
echo /opt/kvm/storage.img > "$G/functions/mass_storage.0/lun.0/file" 2>&1; echo "file rc=$?"
echo "file now: $(cat "$G/functions/mass_storage.0/lun.0/file" 2>/dev/null)"
mkdir -p "$G/configs/c.1/strings/0x409"
echo "cfg" > "$G/configs/c.1/strings/0x409/configuration"
echo 100 > "$G/configs/c.1/MaxPower" 2>/dev/null
ln -s "$G/functions/mass_storage.0" "$G/configs/c.1/f1" 2>&1; echo "link rc=$?"
UDCNAME=$(ls /sys/class/udc | head -1)
echo "=== bind to $UDCNAME ==="
echo "$UDCNAME" > "$G/UDC" 2>&1; echo "bind rc=$?"
sleep 1
echo "bound UDC: $(cat "$G/UDC" 2>/dev/null)"
echo "udc state: $(cat /sys/class/udc/$UDCNAME/state 2>/dev/null)"
echo "=== teardown ==="
echo "" > "$G/UDC" 2>/dev/null
rm -f "$G/configs/c.1/f1" 2>/dev/null
rmdir "$G/configs/c.1/strings/0x409" 2>/dev/null
rmdir "$G/configs/c.1" 2>/dev/null
rmdir "$G/functions/mass_storage.0" 2>/dev/null
rmdir "$G/strings/0x409" 2>/dev/null
rmdir "$G" 2>/dev/null
echo DONE
