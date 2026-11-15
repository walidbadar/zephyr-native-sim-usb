/*
 * SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usb/ch9.h>
#include <linux/usbdevice_fs.h>
#include <nsi_errno.h>

#include "uhc_native_linux_bottom.h"

#define UHC_NATIVE_LINUX_SYSFS "/sys/bus/usb/devices"
#define UHC_NATIVE_LINUX_USBFS "/dev/bus/usb"
#define UHC_NATIVE_LINUX_PATH_MAX 320

struct uhc_native_linux_urb {
	struct uhc_native_linux_urb *next;
	void *ctx;
	struct usbdevfs_urb *urb;
};

struct uhc_native_linux_host {
	struct uhc_native_linux_host *next;
	int fd;
	/* Active configuration */
	uint8_t config;
	/* Bit n is set for interface n taken from its kernel driver */
	uint32_t claimed;
	struct uhc_native_linux_urb *urbs;
	/* Device and configuration descriptors as usbfs returns them */
	size_t desc_len;
	uint8_t desc[4096];
};

static struct uhc_native_linux_host *hosts;

static int uhc_native_linux_errno(void)
{
	return -nsi_errno_to_mid(errno);
}

static int uhc_native_linux_sysfs_read(const char *device, const char *file, int base)
{
	char path[UHC_NATIVE_LINUX_PATH_MAX];
	char text[16] = {0};
	int fd;

	snprintf(path, sizeof(path), UHC_NATIVE_LINUX_SYSFS "/%s/%s", device, file);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return -1;
	}

	if (read(fd, text, sizeof(text) - 1) <= 0) {
		close(fd);
		return -1;
	}

	close(fd);

	return (int)strtol(text, NULL, base);
}

/* Find the usbfs node of the first device with the vendor and product of id */
static int uhc_native_linux_find(const char *id, char *path, size_t size)
{
	unsigned int vendor;
	unsigned int product;
	struct dirent *entry;
	DIR *dir;

	if (sscanf(id, "%x:%x", &vendor, &product) != 2) {
		return -nsi_errno_to_mid(EINVAL);
	}

	dir = opendir(UHC_NATIVE_LINUX_SYSFS);
	if (dir == NULL) {
		return uhc_native_linux_errno();
	}

	while ((entry = readdir(dir)) != NULL) {
		if (uhc_native_linux_sysfs_read(entry->d_name, "idVendor", 16) != (int)vendor ||
		    uhc_native_linux_sysfs_read(entry->d_name, "idProduct", 16) != (int)product) {
			continue;
		}

		snprintf(path, size, UHC_NATIVE_LINUX_USBFS "/%03d/%03d",
			 uhc_native_linux_sysfs_read(entry->d_name, "busnum", 10),
			 uhc_native_linux_sysfs_read(entry->d_name, "devnum", 10));
		closedir(dir);

		return 0;
	}

	closedir(dir);

	return -nsi_errno_to_mid(ENODEV);
}

struct uhc_native_linux_host *uhc_native_linux_open(const char *id,
						    enum uhc_native_linux_speed *speed, int *err)
{
	const uint8_t get_config[] = {USB_DIR_IN, USB_REQ_GET_CONFIGURATION, 0, 0, 0, 0, 1, 0};
	char path[UHC_NATIVE_LINUX_PATH_MAX];
	struct uhc_native_linux_host *host;
	ssize_t len;

	if (id == NULL) {
		*err = -nsi_errno_to_mid(EINVAL);
		return NULL;
	}

	if (id[0] == '/') {
		snprintf(path, sizeof(path), "%s", id);
	} else {
		*err = uhc_native_linux_find(id, path, sizeof(path));
		if (*err != 0) {
			return NULL;
		}
	}

	host = calloc(1, sizeof(*host));
	if (host == NULL) {
		*err = -nsi_errno_to_mid(ENOMEM);
		return NULL;
	}

	host->fd = open(path, O_RDWR | O_CLOEXEC);
	if (host->fd < 0) {
		*err = uhc_native_linux_errno();
		free(host);
		return NULL;
	}

	len = read(host->fd, host->desc, sizeof(host->desc));
	host->desc_len = len > 0 ? (size_t)len : 0;

	*err = uhc_native_linux_control(host, get_config, &host->config, 1000);
	if (*err < 0) {
		close(host->fd);
		free(host);
		return NULL;
	}

	switch (ioctl(host->fd, USBDEVFS_GET_SPEED, NULL)) {
	case USB_SPEED_LOW:
		*speed = UHC_NATIVE_LINUX_SPEED_LOW;
		break;
	case USB_SPEED_FULL:
		*speed = UHC_NATIVE_LINUX_SPEED_FULL;
		break;
	default:
		*speed = UHC_NATIVE_LINUX_SPEED_HIGH;
		break;
	}

	host->next = hosts;
	hosts = host;
	*err = 0;

	return host;
}

/* Give the interfaces back to the kernel drivers they were taken from */
static void uhc_native_linux_release(struct uhc_native_linux_host *host)
{
	if (host->claimed == 0) {
		return;
	}

	for (unsigned int i = 0; i < 32; i++) {
		if ((host->claimed & (1U << i)) != 0) {
			(void)ioctl(host->fd, USBDEVFS_RELEASEINTERFACE, &i);
		}
	}

	/*
	 * A driver can span several interfaces and lose all of them with one,
	 * as cdc_acm does, and is then bound again through its first one only.
	 */
	for (int i = 0; i < 32; i++) {
		struct usbdevfs_ioctl connect = {
			.ifno = i,
			.ioctl_code = USBDEVFS_CONNECT,
		};

		(void)ioctl(host->fd, USBDEVFS_IOCTL, &connect);
	}

	host->claimed = 0;
}

void uhc_native_linux_close(struct uhc_native_linux_host *host)
{
	struct uhc_native_linux_host **link = &hosts;

	while (*link != NULL && *link != host) {
		link = &(*link)->next;
	}

	if (*link == NULL) {
		return;
	}

	*link = host->next;

	uhc_native_linux_release(host);
	/* Closing the device cancels its transfers */
	close(host->fd);

	while (host->urbs != NULL) {
		struct uhc_native_linux_urb *item = host->urbs;

		host->urbs = item->next;
		free(item->urb);
		free(item);
	}

	free(host);
}

void uhc_native_linux_close_all(void)
{
	while (hosts != NULL) {
		uhc_native_linux_close(hosts);
	}
}

/* Find the interface of an endpoint in the active configuration */
static int uhc_native_linux_interface(const struct uhc_native_linux_host *host, uint8_t ep)
{
	uint8_t config = 0;
	int interface = -1;
	size_t pos = USB_DT_DEVICE_SIZE;

	while (pos + 2 <= host->desc_len && host->desc[pos] >= 2 &&
	       pos + host->desc[pos] <= host->desc_len) {
		const uint8_t *desc = &host->desc[pos];

		if (desc[1] == USB_DT_CONFIG && desc[0] >= USB_DT_CONFIG_SIZE) {
			config = desc[5];
		} else if (desc[1] == USB_DT_INTERFACE && desc[0] >= USB_DT_INTERFACE_SIZE) {
			interface = desc[2];
		} else if (desc[1] == USB_DT_ENDPOINT && desc[0] >= USB_DT_ENDPOINT_SIZE &&
			   config == host->config && desc[2] == ep) {
			return interface;
		}

		pos += desc[0];
	}

	return -1;
}

/* Take an interface from its kernel driver, which usbfs asks for before it is used */
static int uhc_native_linux_claim(struct uhc_native_linux_host *host, int interface)
{
	struct usbdevfs_disconnect_claim claim = {
		.interface = (unsigned int)interface,
	};

	if (interface < 0 || interface >= 32) {
		return -nsi_errno_to_mid(ENOENT);
	}

	if ((host->claimed & (1U << interface)) != 0) {
		return 0;
	}

	if (ioctl(host->fd, USBDEVFS_DISCONNECT_CLAIM, &claim) < 0) {
		return uhc_native_linux_errno();
	}

	host->claimed |= 1U << interface;

	return 0;
}

int uhc_native_linux_control(struct uhc_native_linux_host *host, const uint8_t *setup,
			     uint8_t *data, unsigned int timeout_ms)
{
	struct usbdevfs_ctrltransfer transfer = {
		.bRequestType = setup[0],
		.bRequest = setup[1],
		.wValue = (uint16_t)(setup[2] | (setup[3] << 8)),
		.wIndex = (uint16_t)(setup[4] | (setup[5] << 8)),
		.wLength = (uint16_t)(setup[6] | (setup[7] << 8)),
		.timeout = timeout_ms,
		.data = data,
	};
	int ret = 0;

	switch (setup[0] & USB_RECIP_MASK) {
	case USB_RECIP_INTERFACE:
		ret = uhc_native_linux_claim(host, setup[4]);
		break;
	case USB_RECIP_ENDPOINT:
		if ((setup[4] & USB_ENDPOINT_NUMBER_MASK) != 0) {
			ret = uhc_native_linux_claim(host,
						     uhc_native_linux_interface(host, setup[4]));
		}
		break;
	default:
		break;
	}

	if (ret != 0) {
		return ret;
	}

	ret = ioctl(host->fd, USBDEVFS_CONTROL, &transfer);

	return ret < 0 ? uhc_native_linux_errno() : ret;
}

int uhc_native_linux_set_config(struct uhc_native_linux_host *host, uint8_t config)
{
	int value = config;

	if (config == host->config) {
		return 0;
	}

	uhc_native_linux_release(host);

	if (ioctl(host->fd, USBDEVFS_SETCONFIGURATION, &value) < 0) {
		return uhc_native_linux_errno();
	}

	host->config = config;

	return 0;
}

int uhc_native_linux_submit(struct uhc_native_linux_host *host, void *ctx, uint8_t ep,
			    bool interrupt, uint8_t *data, unsigned int len)
{
	struct uhc_native_linux_urb *item;
	struct usbdevfs_urb *urb;
	int ret;

	ret = uhc_native_linux_claim(host, uhc_native_linux_interface(host, ep));
	if (ret != 0) {
		return ret;
	}

	item = calloc(1, sizeof(*item));
	urb = calloc(1, sizeof(*urb));
	if (item == NULL || urb == NULL) {
		free(item);
		free(urb);
		return -nsi_errno_to_mid(ENOMEM);
	}

	urb->type = interrupt ? USBDEVFS_URB_TYPE_INTERRUPT : USBDEVFS_URB_TYPE_BULK;
	urb->endpoint = ep;
	urb->buffer = data;
	urb->buffer_length = (int)len;
	item->urb = urb;
	item->ctx = ctx;

	if (ioctl(host->fd, USBDEVFS_SUBMITURB, urb) < 0) {
		ret = uhc_native_linux_errno();
		free(item);
		free(urb);
		return ret;
	}

	item->next = host->urbs;
	host->urbs = item;

	return 0;
}

int uhc_native_linux_discard(struct uhc_native_linux_host *host, void *ctx)
{
	for (struct uhc_native_linux_urb *item = host->urbs; item != NULL; item = item->next) {
		if (item->ctx != ctx) {
			continue;
		}

		/* A transfer that has finished in the meantime is not an error */
		if (ioctl(host->fd, USBDEVFS_DISCARDURB, item->urb) < 0 && errno != EINVAL) {
			return uhc_native_linux_errno();
		}

		return 0;
	}

	return -nsi_errno_to_mid(ENOENT);
}

int uhc_native_linux_reap(struct uhc_native_linux_host *host, void **ctx, int *status,
			  unsigned int *len)
{
	struct uhc_native_linux_urb **link = &host->urbs;
	struct uhc_native_linux_urb *item;
	struct usbdevfs_urb *urb;

	if (ioctl(host->fd, USBDEVFS_REAPURBNDELAY, &urb) < 0) {
		return errno == EAGAIN ? 0 : uhc_native_linux_errno();
	}

	while (*link != NULL && (*link)->urb != urb) {
		link = &(*link)->next;
	}

	item = *link;
	if (item == NULL) {
		return -nsi_errno_to_mid(EIO);
	}

	*link = item->next;
	*ctx = item->ctx;
	*status = urb->status < 0 ? -nsi_errno_to_mid(-urb->status) : 0;
	*len = (unsigned int)urb->actual_length;
	free(urb);
	free(item);

	return 1;
}
