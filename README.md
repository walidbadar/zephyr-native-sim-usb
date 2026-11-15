# zephyr-native-sim-usb

A Zephyr module that gives `native_sim` a working USB device controller: the
application becomes a real USB device, served by a USB device controller of the
Linux host through [raw-gadget](https://docs.kernel.org/usb/raw-gadget.html).

- With a hardware controller (for example dwc2 on a Raspberry Pi), the
  application is a device on a USB cable.
- With `dummy_hcd`, it is connected to the USB host stack of the same machine.

The driver serves the `zephyr_udc0` node that the board already has, so the USB
device samples of Zephyr build and run unchanged once the module is part of the
build.

## Requirements

- Zephyr `main` with the USB device stack (`CONFIG_USB_DEVICE_STACK_NEXT`).
  Developed against `v4.5.0-2961-g7cd4b562b30`. The driver uses `udc_common.h`,
  which is private to Zephyr's UDC drivers and may change between versions.
- Linux headers with `linux/usb/raw_gadget.h` to build. On the machine that runs
  the application: the `raw_gadget` kernel module, access to `/dev/raw-gadget`,
  and a USB device controller.

For a device on the same machine:

```sh
sudo modprobe -a dummy_hcd raw_gadget
```

Not every distribution ships `raw_gadget`. `scripts/vm_test.sh` shows how to
build it for the running kernel.

## Usage

Add the module to the build of any USB device application with
`-DZEPHYR_EXTRA_MODULES=/path/to/zephyr-native-sim-usb`, then run the
application: the device shows up on the USB host. Give the path in full or with
`$HOME`: the shell does not expand `~` after the `=`.

| Option | Meaning |
| --- | --- |
| `--udc=<name>` | Host controller to use, by its name in `/sys/class/udc`, in place of the one of the devicetree. The first one found when neither is set. |
| `--rt-period=1e-3` | native_sim option. Bus events and transfers are seen up to one real time period late, 10 ms by default. |

The device is high speed unless the `zephyr_udc0` node has
`maximum-speed = "full-speed"` or the USB host is slower: the driver takes the
speed that the host and the controller have agreed on from
`/sys/class/udc/<name>/current_speed`. A lost connection, such as a pulled
cable, is reported to the stack as a suspended bus.

### CDC ACM

The CDC ACM sample of Zephyr needs nothing but the module:

```sh
west build -b native_sim/native/64 zephyr/samples/subsys/usb/cdc_acm -- \
    -DZEPHYR_EXTRA_MODULES=/path/to/zephyr-native-sim-usb
./build/zephyr/zephyr.exe --rt-period=1e-3
```

The USB host gets a serial port, `/dev/ttyACM0` when it has no other CDC ACM
device. The sample waits until a program opens the port, then sends back what
it receives:

```sh
picocom /dev/ttyACM0
```

### Mass storage

The mass storage sample of Zephyr needs a disk, here its RAM disk:

```sh
west build -b native_sim/native/64 zephyr/samples/subsys/usb/mass -- \
    -DZEPHYR_EXTRA_MODULES=/path/to/zephyr-native-sim-usb \
    -DEXTRA_DTC_OVERLAY_FILE=ramdisk.overlay \
    -DCONFIG_APP_MSC_STORAGE_RAM=y
./build/zephyr/zephyr.exe --rt-period=1e-3
```

The USB host gets a disk of 96 KiB, `/dev/sda` when it has no other SCSI disk
(`lsblk` shows it). `CONFIG_APP_MSC_STORAGE_RAM=y` makes the sample format the
disk with FAT, so that a desktop mounts it and lists it in its file manager.

Without that option, which is the default of the sample, the disk is blank: it
is a block device on the USB host, but a file manager only lists file systems.
One can then be made from the USB host, with the name that `lsblk` gives:

```sh
sudo mkfs.vfat /dev/sdX
```

The disk is in the memory of the application: what is on it is lost when the
application ends.

## Devicetree

The driver serves the `zephyr_udc0` node of the board as it is. To name the host
controller in the devicetree, give the node the compatible of the module, which
adds the `host-udc` property:

```dts
&zephyr_udc0 {
	compatible = "zephyr,native-linux-udc";
	host-udc = "dummy_udc.0";
};
```

## Tests

`tests/udc_native_linux` runs anywhere `native_sim` builds, with a fake raw-gadget
behind the host calls of the driver and the test in the role of the USB host:

```sh
west twister -p native_sim/native/64 -T /path/to/zephyr-native-sim-usb/tests
```

It covers the enumeration, control transfers with data in both directions and
stalled ones, bulk and interrupt transfers, an endpoint halt that the host
controller refuses at first, a bus reset, a full speed USB host, a lost
connection, and disabling and enabling the device. The fake counts every call
that raw-gadget would have refused.

## Testing without root

For the real raw-gadget, `scripts/vm_test.sh <zephyr.exe>` boots the kernel of
the host in a KVM virtual machine, loads `dummy_hcd` and `raw_gadget` there, and
runs the application against the USB host stack of the guest. It needs `qemu-system-x86_64`, a static
`busybox`, a C compiler, the kernel headers of the running kernel and a readable
`/boot/vmlinuz-$(uname -r)`.

The default guest test, `scripts/vm_test_echo.sh`, expects the CDC ACM sample:
it checks the enumeration, echoes data, then switches the power of the port off
and on and echoes again. `VM_TEST_DUMMY_HCD=is_high_speed=0` makes the USB host
of the guest a full speed one.

## Limitations

- Linux controller drivers answer `SET_ADDRESS`, `GET_STATUS` and the endpoint
  halt requests themselves. The driver makes up `SET_ADDRESS` and
  `CLEAR_FEATURE(ENDPOINT_HALT)` for the stack, which is told that a halt is
  cleared before the USB host has cleared it.
- The status of a control transfer with a data OUT stage is sent before the
  stack has seen the data, which can therefore not be refused.
- No remote wakeup and no start of frame events.
- The driver takes the real-time signal `SIGRTMIN + 1` to interrupt its host
  threads.
- Tested with `dummy_hcd` only, on Linux 7.3: CDC ACM and mass storage, on a high
  speed and on a full speed USB host. `dummy_hcd` has no isochronous endpoints.

## License

Apache-2.0
