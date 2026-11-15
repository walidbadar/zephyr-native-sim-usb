# zephyr-native-sim-usb

A Zephyr module with USB controller drivers for `native_sim` that use the USB of
the Linux host.

| Driver | The application becomes | Linux interface |
| --- | --- | --- |
| USB device controller | a USB device, on a cable or on the same machine | raw-gadget |
| USB host controller | the host of one USB device of the machine | usbfs |

Add the module to the build of an application:

```sh
west build -b native_sim/native/64 <application> -- \
    -DZEPHYR_EXTRA_MODULES=/path/to/zephyr-native-sim-usb
```

Requirements, usage, devicetree, tests and limitations are in the
[documentation](https://walidbadar.github.io/zephyr-native-sim-usb/).

## License

Apache-2.0
