#!/bin/sh
mkdir -p /root/.ssh
chmod 700 /root/.ssh
touch /root/.ssh/authorized_keys
cat /tmp/kvm.pub >> /root/.ssh/authorized_keys
sort -u /root/.ssh/authorized_keys -o /root/.ssh/authorized_keys
chmod 600 /root/.ssh/authorized_keys
echo "=== authorized_keys ==="
cat /root/.ssh/authorized_keys
echo "=== restart ssh ==="
systemctl restart ssh
systemctl is-active ssh
echo DONE
