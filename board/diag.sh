#!/bin/sh
KR=$(uname -r)
echo "=== kernel ==="
echo "$KR"
echo "=== funcs dir ==="
ls /lib/modules/$KR/kernel/drivers/usb/gadget/function/ 2>/dev/null || echo "(none / built-in)"
echo "=== kconfig (gadget) ==="
CFG=/boot/config-$KR
if [ -f "$CFG" ]; then
  grep -E "CONFIG_USB_LIBCOMPOSITE|CONFIG_USB_CONFIGFS|CONFIG_USB_CONFIGFS_F_HID|CONFIG_USB_CONFIGFS_MASS_STORAGE|CONFIG_USB_DWC3" "$CFG"
else
  echo "no $CFG"
  zcat /proc/config.gz 2>/dev/null | grep -E "LIBCOMPOSITE|CONFIGFS_F_HID|CONFIGFS_MASS_STORAGE" || echo "no /proc/config.gz"
fi
echo "=== existing gadget: rockchip ==="
ls -la /sys/kernel/config/usb_gadget/rockchip/ 2>/dev/null
echo "--- functions ---"
ls /sys/kernel/config/usb_gadget/rockchip/functions/ 2>/dev/null
echo "--- configs ---"
ls /sys/kernel/config/usb_gadget/rockchip/configs/ 2>/dev/null
echo "--- UDC bound ---"
cat /sys/kernel/config/usb_gadget/rockchip/UDC 2>/dev/null
echo "=== udcs available ==="
ls /sys/class/udc
echo "=== modprobe test ==="
modprobe libcomposite 2>&1; echo "libcomposite rc=$?"
modprobe usb_f_hid 2>&1; echo "usb_f_hid rc=$?"
modprobe usb_f_mass_storage 2>&1; echo "usb_f_mass_storage rc=$?"
echo "=== services managing usb gadget ==="
systemctl list-units --type=service --all 2>/dev/null | grep -iE "usb|adb|gadget"
echo "--- processes ---"
ps -ef 2>/dev/null | grep -iE "adbd|gadget" | grep -v grep
echo "=== DONE ==="
