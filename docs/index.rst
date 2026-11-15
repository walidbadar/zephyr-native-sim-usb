.. SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
.. SPDX-License-Identifier: Apache-2.0

USB for Zephyr native_sim
#########################

``zephyr-native-sim-usb`` is a Zephyr module with two USB controller drivers for the
``native_sim`` board. Both use the USB of the Linux machine that runs the application, so that
USB applications can be developed and debugged as a Linux program, against real hardware.

.. list-table::
   :header-rows: 1
   :widths: 20 40 40

   * - Driver
     - What the application becomes
     - Linux interface
   * - :doc:`USB device controller <device>`
     - A USB device, on a cable or on the USB host stack of the same machine
     - raw-gadget (``/dev/raw-gadget``)
   * - :doc:`USB host controller <host>`
     - The host of one USB device that is plugged into the machine
     - usbfs (``/dev/bus/usb``)

Getting the module
******************

Clone the module next to a Zephyr workspace and add it to the build of an application with
``ZEPHYR_EXTRA_MODULES``:

.. code-block:: console

   $ git clone https://github.com/walidbadar/zephyr-native-sim-usb
   $ west build -b native_sim/native/64 <application> -- \
         -DZEPHYR_EXTRA_MODULES=/path/to/zephyr-native-sim-usb

Give the path in full or with ``$HOME``: the shell does not expand ``~`` after the ``=``.

The module is developed against Zephyr ``main`` (``v4.5.0-2961-g7cd4b562b30``). The drivers use
``udc_common.h`` and ``uhc_common.h``, which are private to the USB controller drivers of Zephyr
and may change between versions.

.. toctree::
   :maxdepth: 2
   :caption: Contents

   device
   host
   testing
