#!/bin/sh
cd /tmp
echo "=== download get-pip ==="
python3 -c "import urllib.request; urllib.request.urlretrieve('https://bootstrap.pypa.io/pip/get-pip.py','/tmp/get-pip.py')" 2>/tmp/dl.log && echo "downloaded" || (echo "download failed"; cat /tmp/dl.log)
echo "=== run get-pip ==="
python3 /tmp/get-pip.py >/tmp/getpip.log 2>&1; echo "getpip rc=$?"
tail -n 4 /tmp/getpip.log
echo "=== pip install functionfs ==="
python3 -m pip install --no-input functionfs ioctl-opt >/tmp/pip_ffs2.log 2>&1; echo "pip rc=$?"
tail -n 8 /tmp/pip_ffs2.log
echo "=== verify ==="
python3 -c "import functionfs, functionfs.gadget, ioctl_opt; print('functionfs OK', getattr(functionfs,'__version__','?'))" 2>&1
echo DONE
