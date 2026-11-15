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
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/raw_gadget.h>
#include <nsi_errno.h>
#include <nsi_hw_scheduler.h>
#include <nsi_hws_models_if.h>
#include <nsi_tracing.h>
#include <nsi_utils.h>

#include "irq_ctrl.h"
#include "udc_native_linux_bottom.h"

_Static_assert(UDC_NATIVE_LINUX_EPS_MAX == USB_RAW_EPS_NUM_MAX,
	       "UDC_NATIVE_LINUX_EPS_MAX differs from the endpoints that raw-gadget reports");

#define UDC_NATIVE_LINUX_GADGET "/dev/raw-gadget"
#define UDC_NATIVE_LINUX_SYSFS  "/sys/class/udc"

/* Longest path of a file of a host controller in udc_native_linux_host_sysfs() */
#define UDC_NATIVE_LINUX_PATH_MAX 192

#ifdef UDC_NATIVE_LINUX_TEST_HOOKS
/* Provided by the test in place of the host calls */
const char *udc_native_linux_host_sysfs(void);
int udc_native_linux_host_open(const char *path);
int udc_native_linux_host_ioctl(int fd, unsigned long request, unsigned long arg);
int udc_native_linux_host_close(int fd);
#else
NSI_INLINE const char *udc_native_linux_host_sysfs(void)
{
	return UDC_NATIVE_LINUX_SYSFS;
}

NSI_INLINE int udc_native_linux_host_open(const char *path)
{
	return open(path, O_RDWR | O_CLOEXEC);
}

NSI_INLINE int udc_native_linux_host_ioctl(int fd, unsigned long request, unsigned long arg)
{
	return ioctl(fd, request, arg);
}

NSI_INLINE int udc_native_linux_host_close(int fd)
{
	return close(fd);
}
#endif
#define UDC_NATIVE_LINUX_NAME "USB_UDC_NAME="

/*
 * Interrupts the ioctl() that a host thread is blocked in. A real-time signal,
 * as the application may use the standard ones.
 */
#define UDC_NATIVE_LINUX_SIGNAL (SIGRTMIN + 1)

#define UDC_NATIVE_LINUX_EVENTS 16U

struct udc_native_linux_dev;

struct udc_native_linux_xfer {
	struct udc_native_linux_dev *dev;
	pthread_t thread;
	/* Header and data of the transfer, as the ioctl() takes them */
	struct usb_raw_ep_io *io;
	size_t io_size;
	unsigned long request;
	unsigned long arg;
	int result;
	bool started;
	bool alive;
	/* Waits for its thread */
	bool queued;
	/* Its thread is in the ioctl(), or waits to repeat it */
	bool running;
	bool cancel;
	/* Not yet reported by udc_native_linux_xfer_done() */
	bool done;
};

struct udc_native_linux_evt {
	enum udc_native_linux_event type;
	uint8_t setup[8];
};

struct udc_native_linux_dev {
	struct udc_native_linux_dev *next;
	/* Name of the host controller in udc_native_linux_host_sysfs() */
	char name[UDC_NAME_LENGTH_MAX];
	int fd;
	/* Guards what follows. A host thread does not hold it in an ioctl(). */
	pthread_mutex_t lock;
	/* Signalled on every change that a thread may wait for */
	pthread_cond_t cond;
	pthread_t event_thread;
	bool event_alive;
	bool stop;
	struct udc_native_linux_evt events[UDC_NATIVE_LINUX_EVENTS];
	unsigned int event_head;
	unsigned int event_count;
	struct udc_native_linux_xfer xfers[UDC_NATIVE_LINUX_EPS_MAX + 1];
	/* Set by the host threads when there is something to fetch */
	atomic_bool notify;
};

/* Open controllers, only used from the simulation thread */
static struct udc_native_linux_dev *udc_native_linux_devs;
static uint64_t udc_native_linux_poll_time = NSI_NEVER;
/* Shared by all instances, set from the Zephyr side */
static uint32_t udc_native_linux_irq;
static uint32_t udc_native_linux_poll_us;

static void udc_native_linux_signal(int sig)
{
	(void)sig;
}

static void udc_native_linux_unblock_signal(void)
{
	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, UDC_NATIVE_LINUX_SIGNAL);
	pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}

/* Wait for a change, for <us> microseconds at most. Called with the lock held. */
static void udc_native_linux_wait(struct udc_native_linux_dev *dev, long us)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += us * 1000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_nsec -= 1000000000L;
		ts.tv_sec++;
	}

	pthread_cond_timedwait(&dev->cond, &dev->lock, &ts);
}

/* Runs the transfers of one endpoint, as the ioctl() blocks until the host has served it */
static void *udc_native_linux_xfer_thread(void *arg)
{
	struct udc_native_linux_xfer *xfer = arg;
	struct udc_native_linux_dev *dev = xfer->dev;
	bool again;
	int ret;

	udc_native_linux_unblock_signal();

	pthread_mutex_lock(&dev->lock);

	while (!dev->stop) {
		if (!xfer->queued) {
			pthread_cond_wait(&dev->cond, &dev->lock);
			continue;
		}

		xfer->queued = false;
		xfer->running = true;

		do {
			pthread_mutex_unlock(&dev->lock);

			ret = udc_native_linux_host_ioctl(dev->fd, xfer->request, xfer->arg);
			if (ret < 0) {
				ret = -errno;
			}

			pthread_mutex_lock(&dev->lock);

			/*
			 * An IN endpoint cannot be halted before the USB host
			 * has taken the data sent last.
			 */
			again = ret == -EAGAIN && xfer->request == USB_RAW_IOCTL_EP_SET_HALT &&
				!xfer->cancel && !dev->stop;
			if (again) {
				udc_native_linux_wait(dev, 100L);
			}
		} while (again);

		if (ret < 0) {
			ret = -nsi_errno_to_mid(-ret);
		}

		xfer->result = ret;
		xfer->running = false;
		xfer->done = true;
		atomic_store(&dev->notify, true);
		pthread_cond_broadcast(&dev->cond);
	}

	xfer->alive = false;
	pthread_cond_broadcast(&dev->cond);
	pthread_mutex_unlock(&dev->lock);

	return NULL;
}

/* Linux has the events after USB_RAW_EVENT_CONTROL since 6.7, and its headers their names */
_Static_assert((int)UDC_NATIVE_LINUX_EVT_CONNECT == (int)USB_RAW_EVENT_CONNECT &&
		       (int)UDC_NATIVE_LINUX_EVT_SETUP == (int)USB_RAW_EVENT_CONTROL,
	       "enum udc_native_linux_event does not follow enum usb_raw_event_type");

static enum udc_native_linux_event udc_native_linux_event_type(uint32_t type)
{
	if (type < UDC_NATIVE_LINUX_EVT_CONNECT || type > UDC_NATIVE_LINUX_EVT_DISCONNECT) {
		return UDC_NATIVE_LINUX_EVT_NONE;
	}

	return (enum udc_native_linux_event)type;
}

/* Queues the bus events and the control requests in the order the host controller reports them */
static void *udc_native_linux_event_thread(void *arg)
{
	struct udc_native_linux_dev *dev = arg;
	union {
		struct usb_raw_event event;
		uint8_t raw[sizeof(struct usb_raw_event) + sizeof(struct usb_ctrlrequest)];
	} fetch;
	enum udc_native_linux_event type = UDC_NATIVE_LINUX_EVT_NONE;
	struct udc_native_linux_evt *evt;

	udc_native_linux_unblock_signal();

	pthread_mutex_lock(&dev->lock);

	while (!dev->stop && type != UDC_NATIVE_LINUX_EVT_FAILED) {
		if (dev->event_count == UDC_NATIVE_LINUX_EVENTS) {
			pthread_cond_wait(&dev->cond, &dev->lock);
			continue;
		}

		pthread_mutex_unlock(&dev->lock);

		memset(&fetch, 0, sizeof(fetch));
		fetch.event.length = sizeof(struct usb_ctrlrequest);

		if (udc_native_linux_host_ioctl(dev->fd, USB_RAW_IOCTL_EVENT_FETCH,
						(unsigned long)&fetch) == 0) {
			type = udc_native_linux_event_type(fetch.event.type);
		} else if (errno == EINTR) {
			type = UDC_NATIVE_LINUX_EVT_NONE;
		} else {
			type = UDC_NATIVE_LINUX_EVT_FAILED;
		}

		pthread_mutex_lock(&dev->lock);

		if (type == UDC_NATIVE_LINUX_EVT_NONE) {
			continue;
		}

		evt = &dev->events[(dev->event_head + dev->event_count) % UDC_NATIVE_LINUX_EVENTS];
		evt->type = type;
		memcpy(evt->setup, &fetch.raw[sizeof(struct usb_raw_event)], sizeof(evt->setup));
		dev->event_count++;
		atomic_store(&dev->notify, true);
	}

	dev->event_alive = false;
	pthread_cond_broadcast(&dev->cond);
	pthread_mutex_unlock(&dev->lock);

	return NULL;
}

/* Raise the interrupt when a host thread has something for the Zephyr side */
static void udc_native_linux_poll(void)
{
	struct udc_native_linux_dev *dev;

	for (dev = udc_native_linux_devs; dev != NULL; dev = dev->next) {
		if (atomic_exchange(&dev->notify, false)) {
			hw_irq_ctrl_set_irq(udc_native_linux_irq);
		}
	}

	if (udc_native_linux_devs != NULL) {
		udc_native_linux_poll_time = nsi_hws_get_time() + udc_native_linux_poll_us;
	} else {
		udc_native_linux_poll_time = NSI_NEVER;
	}
}

NSI_HW_EVENT(udc_native_linux_poll_time, udc_native_linux_poll, 500);

static int udc_native_linux_copy_name(uint8_t *dst, size_t size, const char *src)
{
	size_t len = strlen(src);

	if (len == 0U || len >= size) {
		return -nsi_errno_to_mid(ENODEV);
	}

	memcpy(dst, src, len + 1U);

	return 0;
}

/* Names that raw-gadget binds a controller by: the controller's and its gadget's */
static int udc_native_linux_names(const char *udc, struct usb_raw_init *init)
{
	char line[sizeof(UDC_NATIVE_LINUX_NAME) + UDC_NAME_LENGTH_MAX];
	char path[UDC_NATIVE_LINUX_PATH_MAX];
	struct dirent *entry;
	FILE *file;
	DIR *dir;
	int ret = -nsi_errno_to_mid(ENODEV);

	if (udc != NULL) {
		ret = udc_native_linux_copy_name(init->device_name, sizeof(init->device_name), udc);
	} else {
		dir = opendir(udc_native_linux_host_sysfs());
		if (dir == NULL) {
			return ret;
		}

		for (entry = readdir(dir); entry != NULL && ret != 0; entry = readdir(dir)) {
			if (entry->d_name[0] != '.') {
				ret = udc_native_linux_copy_name(init->device_name,
								 sizeof(init->device_name),
								 entry->d_name);
			}
		}

		closedir(dir);
	}

	if (ret != 0) {
		return ret;
	}

	ret = snprintf(path, sizeof(path), "%s/%s/uevent", udc_native_linux_host_sysfs(),
		       (const char *)init->device_name);
	if (ret < 0 || (size_t)ret >= sizeof(path)) {
		return -nsi_errno_to_mid(ENODEV);
	}

	file = fopen(path, "r");
	if (file == NULL) {
		return -nsi_errno_to_mid(ENODEV);
	}

	ret = -nsi_errno_to_mid(ENODEV);
	while (ret != 0 && fgets(line, sizeof(line), file) != NULL) {
		if (strncmp(line, UDC_NATIVE_LINUX_NAME, strlen(UDC_NATIVE_LINUX_NAME)) == 0) {
			line[strcspn(line, "\n")] = '\0';
			ret = udc_native_linux_copy_name(init->driver_name,
							 sizeof(init->driver_name),
							 &line[strlen(UDC_NATIVE_LINUX_NAME)]);
		}
	}

	fclose(file);

	return ret;
}

void *udc_native_linux_open(const char *udc, bool high_speed, uint32_t irq, uint32_t poll_us,
			    int *err)
{
	struct sigaction action = {.sa_handler = udc_native_linux_signal};
	struct usb_raw_init init = {.speed = high_speed ? USB_SPEED_HIGH : USB_SPEED_FULL};
	struct udc_native_linux_dev *dev;
	int ret;

	udc_native_linux_irq = irq;
	udc_native_linux_poll_us = poll_us;

	*err = udc_native_linux_names(udc, &init);
	if (*err != 0) {
		nsi_print_warning("udc-native-linux: no host controller %s in %s\n",
				  udc != NULL ? udc : "", udc_native_linux_host_sysfs());
		return NULL;
	}

	dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		*err = -nsi_errno_to_mid(ENOMEM);
		return NULL;
	}

	memcpy(dev->name, init.device_name, sizeof(dev->name));

	dev->fd = udc_native_linux_host_open(UDC_NATIVE_LINUX_GADGET);
	if (dev->fd < 0) {
		*err = -nsi_errno_to_mid(errno);
		nsi_print_warning("udc-native-linux: failed to open " UDC_NATIVE_LINUX_GADGET
				  ": %s\n",
				  strerror(errno));
		free(dev);
		return NULL;
	}

	/* Without SA_RESTART, for the ioctl() to return when its thread gets the signal */
	sigemptyset(&action.sa_mask);
	sigaction(UDC_NATIVE_LINUX_SIGNAL, &action, NULL);

	pthread_mutex_init(&dev->lock, NULL);
	pthread_cond_init(&dev->cond, NULL);

	/* The gadget is bound, and the controller connected to the USB host, from here on */
	if (udc_native_linux_host_ioctl(dev->fd, USB_RAW_IOCTL_INIT, (unsigned long)&init) < 0 ||
	    udc_native_linux_host_ioctl(dev->fd, USB_RAW_IOCTL_RUN, 0UL) < 0) {
		*err = -nsi_errno_to_mid(errno);
		nsi_print_warning("udc-native-linux: failed to bind %s: %s\n",
				  (const char *)init.device_name, strerror(errno));
		goto fail;
	}

	dev->event_alive = true;
	ret = pthread_create(&dev->event_thread, NULL, udc_native_linux_event_thread, dev);
	if (ret != 0) {
		*err = -nsi_errno_to_mid(ret);
		nsi_print_warning("udc-native-linux: failed to start the event thread: %s\n",
				  strerror(ret));
		goto fail;
	}

	dev->next = udc_native_linux_devs;
	udc_native_linux_devs = dev;

	if (udc_native_linux_poll_time == NSI_NEVER) {
		udc_native_linux_poll_time = nsi_hws_get_time() + udc_native_linux_poll_us;
		nsi_hws_find_next_event();
	}

	return dev;

fail:
	pthread_cond_destroy(&dev->cond);
	pthread_mutex_destroy(&dev->lock);
	udc_native_linux_host_close(dev->fd);
	free(dev);

	return NULL;
}

/* Wait for <thread>, which may be blocked in an ioctl(), to clear <alive> */
static void udc_native_linux_join(struct udc_native_linux_dev *dev, pthread_t thread,
				  const bool *alive)
{
	pthread_mutex_lock(&dev->lock);

	while (*alive) {
		pthread_kill(thread, UDC_NATIVE_LINUX_SIGNAL);
		udc_native_linux_wait(dev, 1000L);
	}

	pthread_mutex_unlock(&dev->lock);
	pthread_join(thread, NULL);
}

void udc_native_linux_close(void *handle)
{
	struct udc_native_linux_dev *dev = handle;
	struct udc_native_linux_dev **link;
	struct udc_native_linux_xfer *xfer;

	for (link = &udc_native_linux_devs; *link != NULL; link = &(*link)->next) {
		if (*link == dev) {
			*link = dev->next;
			break;
		}
	}

	pthread_mutex_lock(&dev->lock);
	dev->stop = true;
	pthread_cond_broadcast(&dev->cond);
	pthread_mutex_unlock(&dev->lock);

	udc_native_linux_join(dev, dev->event_thread, &dev->event_alive);

	for (int i = 0; i <= UDC_NATIVE_LINUX_EPS_MAX; i++) {
		xfer = &dev->xfers[i];

		if (xfer->started) {
			udc_native_linux_join(dev, xfer->thread, &xfer->alive);
		}

		free(xfer->io);
	}

	/* Unbinds the gadget, which disconnects the controller from the USB host */
	udc_native_linux_host_close(dev->fd);
	pthread_cond_destroy(&dev->cond);
	pthread_mutex_destroy(&dev->lock);
	free(dev);
}

void udc_native_linux_close_all(void)
{
	while (udc_native_linux_devs != NULL) {
		udc_native_linux_close(udc_native_linux_devs);
	}
}

enum udc_native_linux_speed udc_native_linux_speed(void *handle)
{
	struct udc_native_linux_dev *dev = handle;
	enum udc_native_linux_speed speed = UDC_NATIVE_LINUX_SPEED_UNKNOWN;
	char path[UDC_NATIVE_LINUX_PATH_MAX];
	char text[sizeof("super-speed-plus\n")] = "";
	const int saved_errno = errno;
	FILE *file = NULL;
	int len;

	len = snprintf(path, sizeof(path), "%s/%s/current_speed", udc_native_linux_host_sysfs(),
		       dev->name);
	if (len > 0 && (size_t)len < sizeof(path)) {
		file = fopen(path, "r");
	}

	if (file != NULL) {
		if (fgets(text, sizeof(text), file) == NULL) {
			text[0] = '\0';
		}

		fclose(file);
	}

	if (strncmp(text, "high-speed", strlen("high-speed")) == 0) {
		speed = UDC_NATIVE_LINUX_SPEED_HIGH;
	} else if (strncmp(text, "full-speed", strlen("full-speed")) == 0 ||
		   strncmp(text, "low-speed", strlen("low-speed")) == 0) {
		speed = UDC_NATIVE_LINUX_SPEED_FULL;
	}

	/* See udc_native_linux_ioctl() */
	errno = saved_errno;

	return speed;
}

int udc_native_linux_eps(void *handle, struct udc_native_linux_ep *eps)
{
	struct udc_native_linux_dev *dev = handle;
	struct usb_raw_eps_info info;
	const struct usb_raw_ep_info *ep;
	int num;

	memset(&info, 0, sizeof(info));

	num = udc_native_linux_host_ioctl(dev->fd, USB_RAW_IOCTL_EPS_INFO, (unsigned long)&info);
	if (num < 0) {
		return -nsi_errno_to_mid(errno);
	}

	for (int i = 0; i < num; i++) {
		ep = &info.eps[i];

		eps[i].num = ep->addr == USB_RAW_EP_ADDR_ANY ? UDC_NATIVE_LINUX_EP_NUM_ANY
							     : (uint8_t)ep->addr;
		eps[i].mps = ep->limits.maxpacket_limit;
		eps[i].in = ep->caps.dir_in != 0U;
		eps[i].out = ep->caps.dir_out != 0U;
		eps[i].iso = ep->caps.type_iso != 0U;
		eps[i].bulk = ep->caps.type_bulk != 0U;
		eps[i].interrupt = ep->caps.type_int != 0U;
	}

	return num;
}

enum udc_native_linux_event udc_native_linux_event_get(void *handle, uint8_t setup[8])
{
	struct udc_native_linux_dev *dev = handle;
	enum udc_native_linux_event type = UDC_NATIVE_LINUX_EVT_NONE;
	const struct udc_native_linux_evt *evt;

	pthread_mutex_lock(&dev->lock);

	if (dev->event_count > 0U) {
		evt = &dev->events[dev->event_head];
		type = evt->type;
		memcpy(setup, evt->setup, sizeof(evt->setup));

		dev->event_head = (dev->event_head + 1U) % UDC_NATIVE_LINUX_EVENTS;
		dev->event_count--;
		pthread_cond_broadcast(&dev->cond);
	}

	pthread_mutex_unlock(&dev->lock);

	return type;
}

int udc_native_linux_ep_enable(void *handle, uint8_t addr, uint8_t attributes, uint16_t mps,
			       uint8_t interval)
{
	struct udc_native_linux_dev *dev = handle;
	struct usb_endpoint_descriptor desc = {
		.bLength = USB_DT_ENDPOINT_SIZE,
		.bDescriptorType = USB_DT_ENDPOINT,
		.bEndpointAddress = addr,
		.bmAttributes = attributes,
		.wMaxPacketSize = __cpu_to_le16(mps),
		.bInterval = interval,
	};
	int ep;

	ep = udc_native_linux_host_ioctl(dev->fd, USB_RAW_IOCTL_EP_ENABLE, (unsigned long)&desc);

	return ep < 0 ? -nsi_errno_to_mid(errno) : ep;
}

/*
 * For the requests that the Zephyr side may see fail without failing itself:
 * the errno of the host is the one of the calling Zephyr thread, where the USB
 * device stack takes a value left behind as the result of a request.
 */
static int udc_native_linux_ioctl(void *handle, unsigned long request, unsigned long arg)
{
	struct udc_native_linux_dev *dev = handle;
	const int saved_errno = errno;
	int ret = 0;

	if (udc_native_linux_host_ioctl(dev->fd, request, arg) < 0) {
		ret = -nsi_errno_to_mid(errno);
	}

	errno = saved_errno;

	return ret;
}

int udc_native_linux_ep_disable(void *handle, int ep)
{
	return udc_native_linux_ioctl(handle, USB_RAW_IOCTL_EP_DISABLE, (unsigned long)ep);
}

int udc_native_linux_ep_clear_halt(void *handle, int ep)
{
	return udc_native_linux_ioctl(handle, USB_RAW_IOCTL_EP_CLEAR_HALT, (unsigned long)ep);
}

int udc_native_linux_ep0_stall(void *handle)
{
	return udc_native_linux_ioctl(handle, USB_RAW_IOCTL_EP0_STALL, 0UL);
}

int udc_native_linux_configure(void *handle)
{
	return udc_native_linux_ioctl(handle, USB_RAW_IOCTL_CONFIGURE, 0UL);
}

/* Make the host thread of the endpoint ready for a request. Called with the lock held. */
static int udc_native_linux_xfer_prepare(struct udc_native_linux_dev *dev,
					 struct udc_native_linux_xfer *xfer, size_t io_size)
{
	struct usb_raw_ep_io *io;
	int ret;

	if (xfer->queued || xfer->running || xfer->done) {
		return -nsi_errno_to_mid(EBUSY);
	}

	if (xfer->io_size < io_size) {
		io = realloc(xfer->io, io_size);
		if (io == NULL) {
			return -nsi_errno_to_mid(ENOMEM);
		}

		xfer->io = io;
		xfer->io_size = io_size;
	}

	if (!xfer->started) {
		xfer->dev = dev;
		xfer->alive = true;

		ret = pthread_create(&xfer->thread, NULL, udc_native_linux_xfer_thread, xfer);
		if (ret != 0) {
			xfer->alive = false;
			return -nsi_errno_to_mid(ret);
		}

		xfer->started = true;
	}

	return 0;
}

int udc_native_linux_xfer_start(void *handle, int ep, bool in, const uint8_t *data, uint32_t len,
				bool zlp)
{
	struct udc_native_linux_dev *dev = handle;
	struct udc_native_linux_xfer *xfer = &dev->xfers[ep];
	int ret;

	pthread_mutex_lock(&dev->lock);

	ret = udc_native_linux_xfer_prepare(dev, xfer, sizeof(struct usb_raw_ep_io) + len);
	if (ret == 0) {
		xfer->io->ep = ep == UDC_NATIVE_LINUX_EP0 ? 0U : (uint16_t)ep;
		xfer->io->flags = zlp ? USB_RAW_IO_FLAGS_ZERO : 0U;
		xfer->io->length = len;
		xfer->arg = (unsigned long)xfer->io;

		if (in) {
			memcpy(xfer->io->data, data, len);
			xfer->request = ep == UDC_NATIVE_LINUX_EP0 ? USB_RAW_IOCTL_EP0_WRITE
								   : USB_RAW_IOCTL_EP_WRITE;
		} else {
			xfer->request = ep == UDC_NATIVE_LINUX_EP0 ? USB_RAW_IOCTL_EP0_READ
								   : USB_RAW_IOCTL_EP_READ;
		}

		xfer->queued = true;
		pthread_cond_broadcast(&dev->cond);
	}

	pthread_mutex_unlock(&dev->lock);

	return ret;
}

int udc_native_linux_halt_start(void *handle, int ep)
{
	struct udc_native_linux_dev *dev = handle;
	struct udc_native_linux_xfer *xfer = &dev->xfers[ep];
	int ret;

	pthread_mutex_lock(&dev->lock);

	ret = udc_native_linux_xfer_prepare(dev, xfer, 0U);
	if (ret == 0) {
		xfer->request = USB_RAW_IOCTL_EP_SET_HALT;
		xfer->arg = (unsigned long)ep;
		xfer->queued = true;
		pthread_cond_broadcast(&dev->cond);
	}

	pthread_mutex_unlock(&dev->lock);

	return ret;
}

bool udc_native_linux_xfer_done(void *handle, int ep, uint8_t *data, uint32_t size, int *result)
{
	struct udc_native_linux_dev *dev = handle;
	struct udc_native_linux_xfer *xfer = &dev->xfers[ep];
	bool done;

	pthread_mutex_lock(&dev->lock);

	done = xfer->done;
	if (done) {
		xfer->done = false;
		*result = xfer->result;

		if (data != NULL && xfer->result > 0) {
			memcpy(data, xfer->io->data, NSI_MIN((uint32_t)xfer->result, size));
		}
	}

	pthread_mutex_unlock(&dev->lock);

	return done;
}

void udc_native_linux_xfer_cancel(void *handle, int ep)
{
	struct udc_native_linux_dev *dev = handle;
	struct udc_native_linux_xfer *xfer = &dev->xfers[ep];

	pthread_mutex_lock(&dev->lock);

	xfer->queued = false;
	xfer->cancel = true;
	pthread_cond_broadcast(&dev->cond);

	while (xfer->running) {
		pthread_kill(xfer->thread, UDC_NATIVE_LINUX_SIGNAL);
		udc_native_linux_wait(dev, 1000L);
	}

	xfer->cancel = false;
	xfer->done = false;

	pthread_mutex_unlock(&dev->lock);
}
