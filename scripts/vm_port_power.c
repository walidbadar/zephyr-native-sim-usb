/*
 * SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Switch the power of a port of a USB hub, which a device on it sees as its
 * cable being pulled or plugged. Runs in the guest of vm_test.sh.
 *
 * usage: vm_port_power <hub device node> <port> <0|1>
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <linux/usb/ch9.h>
#include <linux/usbdevice_fs.h>

/* Hub class feature selector, USB 2.0 specification table 11-17 */
#define PORT_POWER 8

int main(int argc, char **argv)
{
	struct usbdevfs_ctrltransfer ctrl = {
		.bRequestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
		.wValue = PORT_POWER,
		.timeout = 1000,
	};
	int fd;

	if (argc != 4) {
		fprintf(stderr, "usage: %s <hub device node> <port> <0|1>\n", argv[0]);
		return 2;
	}

	ctrl.wIndex = (__u16)atoi(argv[2]);
	ctrl.bRequest = atoi(argv[3]) != 0 ? USB_REQ_SET_FEATURE : USB_REQ_CLEAR_FEATURE;

	fd = open(argv[1], O_RDWR);
	if (fd < 0 || ioctl(fd, USBDEVFS_CONTROL, &ctrl) < 0) {
		perror(argv[1]);
		return 1;
	}

	return 0;
}
