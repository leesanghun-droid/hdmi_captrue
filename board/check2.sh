#!/bin/sh
echo "=== pip ==="
which pip3 && pip3 --version 2>/dev/null || echo "no pip3"
echo "=== sshd config (root/pubkey/pass) ==="
grep -iE "PermitRootLogin|PubkeyAuthentication|PasswordAuthentication" /etc/ssh/sshd_config 2>/dev/null
echo "=== /root/.ssh ==="
ls -la /root/.ssh 2>/dev/null || echo "no /root/.ssh"
echo "=== python-functionfs? ==="
python3 -c "import functionfs, functionfs.gadget; print('functionfs OK')" 2>/dev/null || echo "functionfs NOT installed"
echo "=== libaio (apt) ==="
dpkg -l 2>/dev/null | awk '/libaio/{print $2, $3}'
echo "=== gadget dir ==="
ls /sys/kernel/config/usb_gadget
echo DONE
