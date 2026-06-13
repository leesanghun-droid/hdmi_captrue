#!/bin/sh
export DEBIAN_FRONTEND=noninteractive
echo "=== apt update ==="
apt-get update -y >/tmp/apt_update.log 2>&1; echo "apt update rc=$?"
echo "=== install python3-pip ==="
apt-get install -y python3-pip >/tmp/apt_pip.log 2>&1; echo "pip install rc=$?"
which pip3 && pip3 --version
echo "=== pip install functionfs ==="
pip3 install --no-input functionfs ioctl-opt >/tmp/pip_ffs.log 2>&1; echo "pip ffs rc=$?"
tail -n 5 /tmp/pip_ffs.log
echo "=== verify ==="
python3 -c "import functionfs, functionfs.gadget, ioctl_opt; print('functionfs OK', getattr(functionfs,'__version__','?'))" 2>&1
echo DONE
