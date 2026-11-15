.. SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
.. SPDX-License-Identifier: Apache-2.0

Testing
#######

Twister
*******

The tests of the module run anywhere ``native_sim`` builds:

.. code-block:: console

   $ west twister -p native_sim/native/64 -T /path/to/zephyr-native-sim-usb/tests

``tests/udc_native_linux``
   Runs the USB device controller with a fake raw-gadget behind the host calls of the driver and
   the test in the role of the USB host. It covers the enumeration, control transfers with data
   in both directions and stalled ones, bulk and interrupt transfers, an endpoint halt that the
   host controller refuses at first, a bus reset, a full speed USB host, a lost connection, and
   disabling and enabling the device. The fake counts every call that raw-gadget would have
   refused.

``tests/uhc_native_linux``
   Builds the USB host shell on the USB host controller. It is not run: it needs a USB device.

USB device controller without root
**********************************

For the real raw-gadget, ``scripts/vm_test.sh <zephyr.exe>`` boots the kernel of the host in a
KVM virtual machine, loads ``dummy_hcd`` and ``raw_gadget`` there, and runs the application
against the USB host stack of the guest. It needs ``qemu-system-x86_64``, a static ``busybox``,
a C compiler, the kernel headers of the running kernel and a readable
``/boot/vmlinuz-$(uname -r)``.

The default guest test, ``scripts/vm_test_echo.sh``, expects the CDC ACM sample: it checks the
enumeration, echoes data, then switches the power of the port off and on and echoes again.
``VM_TEST_DUMMY_HCD=is_high_speed=0`` makes the USB host of the guest a full speed one.
