.. SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
.. SPDX-License-Identifier: Apache-2.0

USB host controller
###################

The USB host stack of Zephyr gets one USB device of the Linux host, through usbfs. The
application talks to the real device: its descriptors, its control requests and its bulk and
interrupt endpoints.

Requirements
************

* Zephyr with the USB host stack (``CONFIG_USB_HOST_STACK``).
* Read and write access to the node of the device in ``/dev/bus/usb``. Debug probes and many
  development boards get it from the udev rules of their tools. For another device, a rule such
  as this one gives it to the user who is logged in:

  .. code-block:: none

     SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="374b", TAG+="uaccess"

No kernel module is needed.

Devicetree
**********

``native_sim`` has no USB host controller node, so the application adds one. The USB host
samples of Zephyr look for the ``zephyr_uhc0`` label:

.. code-block:: devicetree

   / {
           zephyr_uhc0: uhc {
                   compatible = "zephyr,native-linux-uhc";
                   host-device = "0483:374b";
           };
   };

``host-device`` is the device as ``vendor:product`` in hexadecimal, the way ``lsusb`` prints it,
or as its usbfs node, for example ``/dev/bus/usb/001/004``. The first device found is used when
several have the same vendor and product.

Usage
*****

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Option
     - Meaning
   * - ``--uhc-device=<id>``
     - USB device of the host to use, in place of the one of the devicetree.

``tests/uhc_native_linux`` is the USB host shell of Zephyr on this controller. It shows the
device as the stack sees it:

.. code-block:: console

   $ west build -b native_sim/native/64 /path/to/zephyr-native-sim-usb/tests/uhc_native_linux
   $ ./build/zephyr/zephyr.exe --uhc-device=1366:1065
   uart connected to pseudotty: /dev/pts/3

Then, in a terminal on that pseudo terminal:

.. code-block:: console

   uart:~$ usbh init
   host: USB host initialized
   uart:~$ usbh enable
   <inf> usbh_dev: New device with address 1 state 2
   <inf> usbh_dev: Configuration 1 bNumInterfaces 8
   host: USB host enabled
   uart:~$ usbh device info 1
   Device Descriptor:
     bLength                        18
     bDescriptorType                 1
     bcdUSB                       2.00
     ...
     idVendor                   0x1366
     idProduct                  0x1065

In an application, the controller is used like any other one:

.. code-block:: c

   USBH_CONTROLLER_DEFINE(uhs_ctx, DEVICE_DT_GET(DT_NODELABEL(zephyr_uhc0)));

   int main(void)
   {
           usbh_init(&uhs_ctx);
           usbh_enable(&uhs_ctx);
           ...
   }

How the device is shared with Linux
***********************************

Linux has enumerated the device before the application starts, and stays in charge of it.

* The device keeps the address that Linux gave it. The driver answers ``SET_ADDRESS`` itself,
  does not pass a bus reset on, and reports the device as connected as soon as the controller is
  enabled.
* ``SET_CONFIGURATION`` goes through usbfs and does nothing for the configuration that is
  active. Another one is refused while kernel drivers use the device.
* An interface is taken from its kernel driver when the first transfer is sent to one of its
  endpoints, or the first control request to it. A serial port or a disk that the kernel driver
  provided disappears at that moment.
* All kernel drivers are bound again when the controller is disabled or the application exits.
  They are not when it is killed with ``SIGKILL``: unplug the device and plug it in again.

Limitations
***********

* Control, bulk and interrupt transfers only: no isochronous transfers, so no audio and no video
  streaming.
* A control transfer blocks the simulation until the device has answered, for up to one second.
* One device per controller, no hubs, and bus suspend and resume do nothing.
* A device that is unplugged is only noticed while a bulk or interrupt transfer is pending.
* Tested on Linux 7.3 with a J-Link probe: enumeration, bulk transfers on its vendor interface
  and on a CDC ACM interface taken from ``cdc_acm``.
