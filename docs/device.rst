.. SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
.. SPDX-License-Identifier: Apache-2.0

USB device controller
#####################

The application becomes a real USB device, served by a USB device controller of the Linux host
through `raw-gadget <https://docs.kernel.org/usb/raw-gadget.html>`_.

* With a hardware controller, for example dwc2 on a Raspberry Pi, the application is a device on
  a USB cable.
* With ``dummy_hcd``, it is connected to the USB host stack of the same machine.

The driver serves the ``zephyr_udc0`` node that the board already has, so the USB device samples
of Zephyr build and run unchanged once the module is part of the build.

Requirements
************

* Zephyr with the USB device stack (``CONFIG_USB_DEVICE_STACK_NEXT``).
* Linux headers with ``linux/usb/raw_gadget.h`` to build.
* On the machine that runs the application: the ``raw_gadget`` kernel module, access to
  ``/dev/raw-gadget``, and a USB device controller.

For a device on the same machine:

.. code-block:: console

   $ sudo modprobe -a dummy_hcd raw_gadget

Not every distribution ships ``raw_gadget``. ``scripts/vm_test.sh`` shows how to build it for the
running kernel.

Usage
*****

Add the module to the build of any USB device application, then run the application: the device
shows up on the USB host.

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Option
     - Meaning
   * - ``--udc=<name>``
     - Host controller to use, by its name in ``/sys/class/udc``, in place of the one of the
       devicetree. The first one found when neither is set.
   * - ``--rt-period=1e-3``
     - ``native_sim`` option. Bus events and transfers are seen up to one real time period late,
       10 ms by default.

The device is high speed unless the ``zephyr_udc0`` node has ``maximum-speed = "full-speed"`` or
the USB host is slower: the driver takes the speed that the host and the controller have agreed
on from ``/sys/class/udc/<name>/current_speed``. A lost connection, such as a pulled cable, is
reported to the stack as a suspended bus.

CDC ACM
=======

The CDC ACM sample of Zephyr needs nothing but the module:

.. code-block:: console

   $ west build -b native_sim/native/64 zephyr/samples/subsys/usb/cdc_acm -- \
         -DZEPHYR_EXTRA_MODULES=/path/to/zephyr-native-sim-usb
   $ ./build/zephyr/zephyr.exe --rt-period=1e-3

The USB host gets a serial port, ``/dev/ttyACM0`` when it has no other CDC ACM device. The sample
waits until a program opens the port, then sends back what it receives:

.. code-block:: console

   $ picocom /dev/ttyACM0

Mass storage
============

The mass storage sample of Zephyr needs a disk, here its RAM disk:

.. code-block:: console

   $ west build -b native_sim/native/64 zephyr/samples/subsys/usb/mass -- \
         -DZEPHYR_EXTRA_MODULES=/path/to/zephyr-native-sim-usb \
         -DEXTRA_DTC_OVERLAY_FILE=ramdisk.overlay \
         -DCONFIG_APP_MSC_STORAGE_RAM=y
   $ ./build/zephyr/zephyr.exe --rt-period=1e-3

The USB host gets a disk of 96 KiB, ``/dev/sda`` when it has no other SCSI disk (``lsblk`` shows
it). ``CONFIG_APP_MSC_STORAGE_RAM=y`` makes the sample format the disk with FAT, so that a
desktop mounts it and lists it in its file manager.

Without that option, which is the default of the sample, the disk is blank: it is a block device
on the USB host, but a file manager only lists file systems. One can then be made from the USB
host, with the name that ``lsblk`` gives:

.. code-block:: console

   $ sudo mkfs.vfat /dev/sdX

The disk is in the memory of the application: what is on it is lost when the application ends.

Devicetree
**********

The driver serves the ``zephyr_udc0`` node of the board as it is. To name the host controller in
the devicetree, give the node the compatible of the module, which adds the ``host-udc`` property:

.. code-block:: devicetree

   &zephyr_udc0 {
           compatible = "zephyr,native-linux-udc";
           host-udc = "dummy_udc.0";
   };

Limitations
***********

* Linux controller drivers answer ``SET_ADDRESS``, ``GET_STATUS`` and the endpoint halt requests
  themselves. The driver makes up ``SET_ADDRESS`` and ``CLEAR_FEATURE(ENDPOINT_HALT)`` for the
  stack, which is told that a halt is cleared before the USB host has cleared it.
* The status of a control transfer with a data OUT stage is sent before the stack has seen the
  data, which can therefore not be refused.
* No remote wakeup and no start of frame events.
* The driver takes the real-time signal ``SIGRTMIN + 1`` to interrupt its host threads.
* Tested with ``dummy_hcd`` only, on Linux 7.3: CDC ACM and mass storage, on a high speed and on
  a full speed USB host. ``dummy_hcd`` has no isochronous endpoints.
