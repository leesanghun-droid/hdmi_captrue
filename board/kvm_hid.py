#!/usr/bin/env python3
# KVM HID gadget for RK3568 (Firefly EC-R3568PC), Ubuntu 22.04, kernel 5.10.
#
# The board's stock kernel has CONFIG_USB_CONFIGFS_F_HID disabled, so we cannot
# use /dev/hidg*. Instead we implement HID (keyboard + absolute mouse) in user
# space via FunctionFS (python-functionfs), and combine it in one composite
# gadget with a kernel mass_storage function. The single USB-C OTG cable then
# presents to the target PC as: keyboard + mouse + USB drive.
#
# Input events arrive over UDP (LAN) and are turned into HID reports.
#
# UDP wire protocol (little-endian, one datagram per event):
#   keyboard : [0x01, modifiers, k0, k1, k2, k3, k4, k5]            (8 bytes)
#              modifiers = HID modifier bitmask (LCtrl=1, LShift=2, ...)
#              k0..k5    = up to 6 simultaneous HID key usage codes
#   mouse    : [0x02, buttons, Xlo, Xhi, Ylo, Yhi, wheel]           (7 bytes)
#              buttons = bit0 left, bit1 right, bit2 middle, ...
#              X,Y     = absolute position, uint16, range 0..32767
#              wheel   = int8 relative wheel step
#
# HID reports emitted on the interrupt IN endpoint (Report IDs):
#   ID 1 keyboard : [0x01, modifiers, 0x00, k0..k5]                 (9 bytes)
#   ID 2 mouse    : [0x02, buttons, X(u16), Y(u16), wheel(i8)]      (7 bytes)

import errno
import functools
import select
import socket
import struct
import sys

import functionfs
import functionfs.ch9 as ch9
from functionfs import HIDFunction
from functionfs.gadget import (
    GadgetSubprocessManager,
    ConfigFunctionFFSSubprocess,
    ConfigFunctionKernel,
)

UDP_IP = '0.0.0.0'
UDP_PORT = 50000

IMG_PATH = '/opt/kvm/storage.img'

# Combined report descriptor: keyboard (Report ID 1) + absolute mouse (ID 2).
REPORT_DESCRIPTOR = bytes([
    # ---- Keyboard, Report ID 1 ----
    0x05, 0x01,        # Usage Page (Generic Desktop)
    0x09, 0x06,        # Usage (Keyboard)
    0xA1, 0x01,        # Collection (Application)
    0x85, 0x01,        #   Report ID (1)
    0x05, 0x07,        #   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        #   Usage Minimum (Left Control)
    0x29, 0xE7,        #   Usage Maximum (Right GUI)
    0x15, 0x00,        #   Logical Minimum (0)
    0x25, 0x01,        #   Logical Maximum (1)
    0x75, 0x01,        #   Report Size (1)
    0x95, 0x08,        #   Report Count (8)
    0x81, 0x02,        #   Input (Data,Var,Abs)  -- modifier byte
    0x95, 0x01,        #   Report Count (1)
    0x75, 0x08,        #   Report Size (8)
    0x81, 0x03,        #   Input (Const)         -- reserved byte
    0x95, 0x06,        #   Report Count (6)
    0x75, 0x08,        #   Report Size (8)
    0x15, 0x00,        #   Logical Minimum (0)
    0x26, 0xFF, 0x00,  #   Logical Maximum (255)
    0x05, 0x07,        #   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        #   Usage Minimum (0)
    0x2A, 0xFF, 0x00,  #   Usage Maximum (255)
    0x81, 0x00,        #   Input (Data,Array)    -- 6 key codes
    0xC0,              # End Collection
    # ---- Mouse (absolute), Report ID 2 ----
    0x05, 0x01,        # Usage Page (Generic Desktop)
    0x09, 0x02,        # Usage (Mouse)
    0xA1, 0x01,        # Collection (Application)
    0x85, 0x02,        #   Report ID (2)
    0x09, 0x01,        #   Usage (Pointer)
    0xA1, 0x00,        #   Collection (Physical)
    0x05, 0x09,        #     Usage Page (Button)
    0x19, 0x01,        #     Usage Minimum (Button 1)
    0x29, 0x05,        #     Usage Maximum (Button 5)
    0x15, 0x00,        #     Logical Minimum (0)
    0x25, 0x01,        #     Logical Maximum (1)
    0x95, 0x05,        #     Report Count (5)
    0x75, 0x01,        #     Report Size (1)
    0x81, 0x02,        #     Input (Data,Var,Abs)  -- 5 buttons
    0x95, 0x01,        #     Report Count (1)
    0x75, 0x03,        #     Report Size (3)
    0x81, 0x03,        #     Input (Const)         -- padding
    0x05, 0x01,        #     Usage Page (Generic Desktop)
    0x09, 0x30,        #     Usage (X)
    0x09, 0x31,        #     Usage (Y)
    0x16, 0x00, 0x00,  #     Logical Minimum (0)
    0x26, 0xFF, 0x7F,  #     Logical Maximum (32767)
    0x75, 0x10,        #     Report Size (16)
    0x95, 0x02,        #     Report Count (2)
    0x81, 0x02,        #     Input (Data,Var,Abs)  -- absolute X,Y
    0x09, 0x38,        #     Usage (Wheel)
    0x15, 0x81,        #     Logical Minimum (-127)
    0x25, 0x7F,        #     Logical Maximum (127)
    0x75, 0x08,        #     Report Size (8)
    0x95, 0x01,        #     Report Count (1)
    0x81, 0x06,        #     Input (Data,Var,Rel)  -- wheel
    0xC0,              #   End Collection
    0xC0,              # End Collection
])

# Longest report we emit (keyboard, with Report ID) = 9 bytes.
IN_REPORT_MAX_LENGTH = 9


class KVMHIDFunction(HIDFunction):
    def __init__(self, path, udp_sock):
        self._udp = udp_sock
        self._enabled = False
        super().__init__(
            path,
            report_descriptor=REPORT_DESCRIPTOR,
            in_report_max_length=IN_REPORT_MAX_LENGTH,
            all_ctrl_recip=True,
            # high_speed_interval in 2**(n-1)*125us units: 1 => 125us polling.
            high_speed_interval=1,
            full_speed_interval=1,
        )

    # --- HID control requests: ACK instead of stalling --------------------
    def setHIDIdle(self, value, index, length):
        self.ep0.read(0)

    def setHIDProtocol(self, value, index, length):
        self.ep0.read(0)

    def setHIDReport(self, value, index, length):
        # e.g. keyboard LED output report; consume and ignore.
        if length:
            self.ep0.read(length)
        else:
            self.ep0.read(0)

    def getHIDReport(self, value, index, length):
        self.ep0.write(bytes(length))

    def getHIDIdle(self, value, index, length):
        self.ep0.write(bytes(length))

    def getHIDProtocol(self, value, index, length):
        self.ep0.write(bytes(length))

    # --- lifecycle --------------------------------------------------------
    def onEnable(self):
        super().onEnable()
        self._enabled = True
        sys.stderr.write('kvm_hid: enabled (host configured gadget)\n')
        sys.stderr.flush()

    def onDisable(self):
        self._enabled = False
        super().onDisable()

    # --- reports ----------------------------------------------------------
    def _send(self, report):
        if not self._enabled:
            return
        try:
            self.getEndpoint(1).submit([bytearray(report)])
        except OSError as exc:
            if exc.errno != errno.EAGAIN:
                raise

    def _handle_udp(self):
        try:
            data = self._udp.recv(64)
        except BlockingIOError:
            return
        if not data:
            return
        kind = data[0]
        if kind == 0x01 and len(data) >= 8:
            mod = data[1]
            keys = data[2:8]
            self._send(bytes([0x01, mod, 0x00]) + keys)
        elif kind == 0x02 and len(data) >= 7:
            buttons = data[1] & 0x1F
            x = (data[2] | (data[3] << 8)) & 0x7FFF
            y = (data[4] | (data[5] << 8)) & 0x7FFF
            wheel = data[6]
            if wheel > 127:
                wheel -= 256
            self._send(struct.pack('<BBHHb', 0x02, buttons, x, y, wheel))

    # --- event loop: functionfs ep0/AIO events + UDP ----------------------
    def serve(self):
        ev_fd = self.eventfd.fileno()
        udp_fd = self._udp.fileno()
        poller = select.epoll()
        poller.register(ev_fd, select.EPOLLIN)
        poller.register(udp_fd, select.EPOLLIN)
        sys.stderr.write('kvm_hid: serving UDP on %s:%d\n' % (UDP_IP, UDP_PORT))
        sys.stderr.flush()
        while True:
            try:
                events = poller.poll()
            except (OSError, IOError) as exc:
                if exc.errno == errno.EINTR:
                    continue
                raise
            for fd, _ in events:
                if fd == ev_fd:
                    self.processEvents()
                elif fd == udp_fd:
                    self._handle_udp()


class HIDConfigFunction(ConfigFunctionFFSSubprocess):
    """FunctionFS HID function, run in its own subprocess with a UDP listener."""

    def getFunction(self, path):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((UDP_IP, UDP_PORT))
        sock.setblocking(False)
        return KVMHIDFunction(path, sock)

    def run(self):
        try:
            self.function.serve()
        except KeyboardInterrupt:
            pass


class MassStorageFunction(ConfigFunctionKernel):
    type_name = 'mass_storage'

    def __init__(self, config_dict=(), name=None, uid=None, gid=None):
        self._cfg = dict(config_dict)
        super().__init__(config_dict=config_dict, name=name, uid=uid, gid=gid)

    def start(self, path):
        import os as _os
        sys.stderr.write('MSC: start path=%s\n' % path)
        try:
            sys.stderr.write('MSC: dir=%r\n' % (_os.listdir(path),))
            sys.stderr.write('MSC: lun.0=%r\n' % (_os.listdir(_os.path.join(path, 'lun.0')),))
        except OSError as exc:
            sys.stderr.write('MSC: listdir err %r\n' % (exc,))
        sys.stderr.flush()
        for key, value in self._cfg.items():
            opt = _os.path.join(path, key)
            sys.stderr.write('MSC: writing %s = %r\n' % (opt, value))
            sys.stderr.flush()
            with open(opt, 'w') as opt_file:
                opt_file.write(value)
        sys.stderr.write('MSC: all options written OK\n')
        sys.stderr.flush()


def main():
    import os
    with_msc = os.environ.get('KVM_WITH_MSC', '1') != '0'
    msc_first = os.environ.get('KVM_MSC_FIRST', '0') == '1'
    parser = GadgetSubprocessManager.getArgumentParser(
        description='RK3568 KVM HID + mass storage gadget',
    )
    args = parser.parse_args()
    function_list = [HIDConfigFunction]
    if with_msc:
        # NOTE: order matters. 'removable'/'ro' can only be changed while no
        # backing file is open, so 'file' MUST be written last.
        msc = functools.partial(
            MassStorageFunction,
            config_dict={
                'lun.0/removable': '1',
                'lun.0/ro': '0',
                'lun.0/file': IMG_PATH,
            },
        )
        if msc_first:
            function_list.insert(0, msc)
        else:
            function_list.append(msc)
    with GadgetSubprocessManager(
        args=args,
        config_list=[
            {
                'function_list': function_list,
                'MaxPower': 100,
                'lang_dict': {
                    0x409: {
                        'configuration': 'KVM',
                    },
                },
            },
        ],
        idVendor=0x1d6b,   # Linux Foundation
        idProduct=0x0104,  # Multifunction Composite Gadget
        bcdDevice=0x0100,
        lang_dict={
            0x409: {
                'serialnumber': 'rk3568-kvm-0001',
                'product': 'RK3568 KVM',
                'manufacturer': 'DIY',
            },
        },
    ) as gadget:
        sys.stderr.write('kvm_hid: gadget bound, servicing forever\n')
        sys.stderr.flush()
        gadget.waitForever()


if __name__ == '__main__':
    main()
