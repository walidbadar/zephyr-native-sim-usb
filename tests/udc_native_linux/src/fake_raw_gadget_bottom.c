/*
 * SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Fake raw-gadget behind the host call hooks of the driver bottom
 * (UDC_NATIVE_LINUX_TEST_HOOKS), with the test in the role of the USB host.
 * It keeps to the rules of the real one: the calls block until the USB host
 * has served them, a blocked call returns when its thread gets a signal, and
 * what raw-gadget refuses is counted as an error.
 */

#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <linux/usb/ch9.h>
#include <linux/usb/raw_gadget.h>

#include "fake_raw_gadget_bottom.h"

#define FAKE_FD       100
#define FAKE_EPS      6
#define FAKE_EVENTS   16U
#define FAKE_DATA_MAX 2048U
#define FAKE_MPS      1024U
#define FAKE_PATH_MAX 96

struct fake_ep {
	/* What the host controller has */
	const char *name;
	uint32_t num;
	bool in;
	bool interrupt;
	/* What the device has made of it */
	bool enabled;
	bool halted;
	bool queued;
	uint8_t addr;
	/* Given by the test with the data of an OUT endpoint, or to take that of an IN one */
	sem_t sem;
	uint8_t data[FAKE_DATA_MAX];
	uint32_t len;
	bool done;
};

struct fake_event {
	uint32_t type;
	uint8_t setup[8];
};

/* One fixed bulk pair, and endpoints for any number that are interrupt capable as well */
static struct fake_ep fake_eps[FAKE_EPS] = {
	{.name = "ep1in-bulk", .num = 1U, .in = true},
	{.name = "ep1out-bulk", .num = 1U},
	{.name = "ep-ain", .num = USB_RAW_EP_ADDR_ANY, .in = true, .interrupt = true},
	{.name = "ep-bout", .num = USB_RAW_EP_ADDR_ANY, .interrupt = true},
	{.name = "ep-cin", .num = USB_RAW_EP_ADDR_ANY, .in = true, .interrupt = true},
	{.name = "ep-dout", .num = USB_RAW_EP_ADDR_ANY, .interrupt = true},
};

static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t fake_once = PTHREAD_ONCE_INIT;
static char fake_sysfs[FAKE_PATH_MAX];

static bool fake_open;
static bool fake_initialized;
static bool fake_running;

static struct fake_event fake_events[FAKE_EVENTS];
static unsigned int fake_event_head;
static unsigned int fake_event_count;
static sem_t fake_event_sem;

/* The control transfer in progress */
static struct usb_ctrlrequest fake_setup;
static bool fake_in_pending;
static bool fake_out_pending;
static uint8_t fake_ctrl_out[FAKE_DATA_MAX];
static uint32_t fake_ctrl_out_len;
static uint8_t fake_ctrl_in[FAKE_DATA_MAX];
static bool fake_ctrl_done;
static int fake_ctrl_result;

static unsigned int fake_halt_busy;
static struct fake_raw_gadget_stats fake_stats;

static void fake_write_file(const char *udc, const char *file, const char *text)
{
	char path[FAKE_PATH_MAX + 64];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s/%s", fake_sysfs, udc, file);
	f = fopen(path, "w");
	if (f != NULL) {
		fputs(text, f);
		fclose(f);
	}
}

static void fake_remove_sysfs(void)
{
	static const char *const udcs[] = {FAKE_RAW_GADGET_UDC, FAKE_RAW_GADGET_OTHER_UDC};
	static const char *const files[] = {"uevent", "current_speed"};
	char path[FAKE_PATH_MAX + 64];

	for (size_t u = 0; u < sizeof(udcs) / sizeof(udcs[0]); u++) {
		for (size_t f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
			snprintf(path, sizeof(path), "%s/%s/%s", fake_sysfs, udcs[u], files[f]);
			unlink(path);
		}

		snprintf(path, sizeof(path), "%s/%s", fake_sysfs, udcs[u]);
		rmdir(path);
	}

	rmdir(fake_sysfs);
}

/* A directory like /sys/class/udc, with what the driver reads there */
static void fake_init(void)
{
	static const char *const udcs[] = {FAKE_RAW_GADGET_UDC, FAKE_RAW_GADGET_OTHER_UDC};
	char path[FAKE_PATH_MAX + 64];

	sem_init(&fake_event_sem, 0, 0);
	for (int i = 0; i < FAKE_EPS; i++) {
		sem_init(&fake_eps[i].sem, 0, 0);
	}

	snprintf(fake_sysfs, sizeof(fake_sysfs), "/tmp/fake_raw_gadget.XXXXXX");
	if (mkdtemp(fake_sysfs) == NULL) {
		fake_sysfs[0] = '\0';
		return;
	}

	for (size_t u = 0; u < sizeof(udcs) / sizeof(udcs[0]); u++) {
		snprintf(path, sizeof(path), "%s/%s", fake_sysfs, udcs[u]);
		mkdir(path, 0700);
		fake_write_file(udcs[u], "uevent", "USB_UDC_NAME=" FAKE_RAW_GADGET_DRIVER "\n");
		fake_write_file(udcs[u], "current_speed", "UNKNOWN\n");
	}

	atexit(fake_remove_sysfs);
}

/* Called with the lock held */
static int fake_error(const char *what, int err)
{
	fake_stats.errors++;
	fake_stats.last_error = what;
	errno = err;

	return -1;
}

static struct fake_ep *fake_ep_by_addr(uint8_t addr)
{
	for (int i = 0; i < FAKE_EPS; i++) {
		if (fake_eps[i].enabled && fake_eps[i].addr == addr) {
			return &fake_eps[i];
		}
	}

	return NULL;
}

static void fake_queue_event(uint32_t type, const uint8_t setup[8])
{
	struct fake_event *event;

	if (fake_event_count == FAKE_EVENTS) {
		return;
	}

	event = &fake_events[(fake_event_head + fake_event_count) % FAKE_EVENTS];
	event->type = type;
	if (setup != NULL) {
		memcpy(event->setup, setup, sizeof(event->setup));
	}

	fake_event_count++;
	sem_post(&fake_event_sem);
}

const char *udc_native_linux_host_sysfs(void)
{
	pthread_once(&fake_once, fake_init);

	return fake_sysfs;
}

int udc_native_linux_host_open(const char *path)
{
	int ret = FAKE_FD;

	pthread_once(&fake_once, fake_init);
	pthread_mutex_lock(&fake_lock);

	if (strcmp(path, "/dev/raw-gadget") != 0) {
		errno = ENOENT;
		ret = -1;
	} else if (fake_open) {
		ret = fake_error("open while open", EBUSY);
	} else {
		fake_open = true;
		fake_stats.opens++;
	}

	pthread_mutex_unlock(&fake_lock);

	return ret;
}

int udc_native_linux_host_close(int fd)
{
	(void)fd;

	pthread_mutex_lock(&fake_lock);

	for (int i = 0; i < FAKE_EPS; i++) {
		if (fake_eps[i].queued) {
			fake_error("close with a transfer in progress", EBUSY);
		}

		fake_eps[i].enabled = false;
		fake_eps[i].halted = false;
		fake_eps[i].done = false;
	}

	fake_open = false;
	fake_initialized = false;
	fake_running = false;
	fake_in_pending = false;
	fake_out_pending = false;
	fake_stats.closes++;

	pthread_mutex_unlock(&fake_lock);

	return 0;
}

static int fake_ioctl_init(const struct usb_raw_init *init)
{
	if (fake_initialized) {
		return fake_error("INIT twice", EINVAL);
	}

	if (snprintf(fake_stats.udc, sizeof(fake_stats.udc), "%s",
		     (const char *)init->device_name) >= (int)sizeof(fake_stats.udc) ||
	    snprintf(fake_stats.driver, sizeof(fake_stats.driver), "%s",
		     (const char *)init->driver_name) >= (int)sizeof(fake_stats.driver)) {
		return fake_error("INIT with a long name", EINVAL);
	}

	fake_stats.high_speed = init->speed == USB_SPEED_HIGH;

	if (strcmp(fake_stats.driver, FAKE_RAW_GADGET_DRIVER) != 0 ||
	    (strcmp(fake_stats.udc, FAKE_RAW_GADGET_UDC) != 0 &&
	     strcmp(fake_stats.udc, FAKE_RAW_GADGET_OTHER_UDC) != 0)) {
		return fake_error("INIT with unknown names", ENODEV);
	}

	fake_initialized = true;

	return 0;
}

static int fake_ioctl_run(void)
{
	if (!fake_initialized || fake_running) {
		return fake_error("RUN in the wrong state", EINVAL);
	}

	fake_running = true;
	fake_event_head = 0U;
	fake_event_count = 0U;
	while (sem_trywait(&fake_event_sem) == 0) {
	}

	fake_queue_event(USB_RAW_EVENT_CONNECT, NULL);

	return 0;
}

static int fake_ioctl_eps_info(struct usb_raw_eps_info *info)
{
	if (!fake_running) {
		return fake_error("EPS_INFO while not running", EINVAL);
	}

	for (int i = 0; i < FAKE_EPS; i++) {
		struct usb_raw_ep_info *ep = &info->eps[i];

		snprintf((char *)ep->name, sizeof(ep->name), "%s", fake_eps[i].name);
		ep->addr = fake_eps[i].num;
		ep->caps.type_bulk = 1U;
		ep->caps.type_int = fake_eps[i].interrupt ? 1U : 0U;
		ep->caps.dir_in = fake_eps[i].in ? 1U : 0U;
		ep->caps.dir_out = fake_eps[i].in ? 0U : 1U;
		ep->limits.maxpacket_limit = FAKE_MPS;
	}

	return FAKE_EPS;
}

static int fake_ioctl_ep_enable(const struct usb_endpoint_descriptor *desc)
{
	const bool in = (desc->bEndpointAddress & USB_DIR_IN) != 0U;
	const uint32_t num = desc->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
	const uint8_t type = desc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK;

	if (!fake_running) {
		return fake_error("EP_ENABLE while not running", EINVAL);
	}

	for (int i = 0; i < FAKE_EPS; i++) {
		struct fake_ep *ep = &fake_eps[i];

		if ((ep->num != num && ep->num != USB_RAW_EP_ADDR_ANY) || ep->in != in ||
		    ep->enabled) {
			continue;
		}

		if (type != USB_ENDPOINT_XFER_BULK &&
		    (type != USB_ENDPOINT_XFER_INT || !ep->interrupt)) {
			continue;
		}

		ep->enabled = true;
		ep->halted = false;
		ep->done = false;
		ep->addr = desc->bEndpointAddress;
		while (sem_trywait(&ep->sem) == 0) {
		}

		fake_stats.ep_enables++;

		return i;
	}

	return fake_error("EP_ENABLE without a matching endpoint", EINVAL);
}

/* The endpoint of a request that raw-gadget refuses while a transfer is in progress */
static struct fake_ep *fake_idle_ep(unsigned long arg, const char *what)
{
	struct fake_ep *ep = arg < FAKE_EPS ? &fake_eps[arg] : NULL;

	if (!fake_running || ep == NULL || !ep->enabled) {
		fake_error(what, EINVAL);
		return NULL;
	}

	if (ep->queued) {
		fake_error(what, EINVAL);
		return NULL;
	}

	return ep;
}

static int fake_ioctl_ep_disable(unsigned long arg)
{
	struct fake_ep *ep = fake_idle_ep(arg, "EP_DISABLE refused");

	if (ep == NULL) {
		return -1;
	}

	ep->enabled = false;
	ep->halted = false;
	fake_stats.ep_disables++;

	return 0;
}

static int fake_ioctl_ep_halt(unsigned long arg, bool halt)
{
	struct fake_ep *ep =
		fake_idle_ep(arg, halt ? "EP_SET_HALT refused" : "EP_CLEAR_HALT refused");

	if (ep == NULL) {
		return -1;
	}

	if (halt) {
		fake_stats.set_halts++;
		if (fake_halt_busy > 0U) {
			/* Not an error: the device has to try again */
			fake_halt_busy--;
			errno = EAGAIN;
			return -1;
		}
	} else {
		fake_stats.clear_halts++;
	}

	ep->halted = halt;

	return 0;
}

static void fake_ctrl_finish(int result)
{
	fake_in_pending = false;
	fake_out_pending = false;
	fake_ctrl_result = result;
	fake_ctrl_done = true;
}

static int fake_ioctl_ep0(struct usb_raw_ep_io *io, bool in)
{
	uint32_t len;

	if (!fake_running) {
		return fake_error("EP0 I/O while not running", EINVAL);
	}

	if ((in && !fake_in_pending) || (!in && !fake_out_pending)) {
		return fake_error(in ? "EP0_WRITE without a data IN stage"
				     : "EP0_READ without a data OUT or status stage",
				  EBUSY);
	}

	if (in) {
		len = io->length < FAKE_DATA_MAX ? io->length : FAKE_DATA_MAX;
		memcpy(fake_ctrl_in, io->data, len);
	} else {
		len = io->length < fake_ctrl_out_len ? io->length : fake_ctrl_out_len;
		memcpy(io->data, fake_ctrl_out, len);
	}

	fake_ctrl_finish(in ? (int)len : 0);

	return (int)len;
}

static int fake_ioctl_ep0_stall(void)
{
	if (!fake_running || (!fake_in_pending && !fake_out_pending)) {
		return fake_error("EP0_STALL without a request", EBUSY);
	}

	fake_ctrl_finish(FAKE_RAW_GADGET_STALLED);

	return 0;
}

/* Called with the lock held, which it gives up while it waits for the test */
static int fake_ioctl_ep_io(struct usb_raw_ep_io *io, bool in)
{
	struct fake_ep *ep = io->ep < FAKE_EPS ? &fake_eps[io->ep] : NULL;
	uint32_t len;
	int ret;

	if (!fake_running || ep == NULL || !ep->enabled || ep->queued || ep->in != in) {
		return fake_error("endpoint I/O refused", EBUSY);
	}

	ep->queued = true;
	pthread_mutex_unlock(&fake_lock);

	ret = sem_wait(&ep->sem);

	pthread_mutex_lock(&fake_lock);
	ep->queued = false;

	if (ret != 0) {
		errno = EINTR;
		return -1;
	}

	if (in) {
		len = io->length < FAKE_DATA_MAX ? io->length : FAKE_DATA_MAX;
		memcpy(ep->data, io->data, len);
		ep->len = len;
	} else {
		len = io->length < ep->len ? io->length : ep->len;
		memcpy(io->data, ep->data, len);
	}

	ep->done = true;

	return (int)len;
}

static int fake_ioctl_event_fetch(struct usb_raw_event *event)
{
	const struct fake_event *queued;
	int ret;

	if (!fake_running) {
		return fake_error("EVENT_FETCH while not running", EINVAL);
	}

	pthread_mutex_unlock(&fake_lock);

	ret = sem_wait(&fake_event_sem);

	pthread_mutex_lock(&fake_lock);

	if (ret != 0 || fake_event_count == 0U) {
		errno = EINTR;
		return -1;
	}

	queued = &fake_events[fake_event_head];
	fake_event_head = (fake_event_head + 1U) % FAKE_EVENTS;
	fake_event_count--;

	event->type = queued->type;
	if (queued->type == USB_RAW_EVENT_CONTROL) {
		memcpy(&fake_setup, queued->setup, sizeof(fake_setup));
		memcpy(event->data, queued->setup, sizeof(queued->setup));
		event->length = sizeof(queued->setup);

		/* As raw-gadget does when the request arrives */
		if ((fake_setup.bRequestType & USB_DIR_IN) != 0U && fake_setup.wLength != 0U) {
			fake_in_pending = true;
		} else {
			fake_out_pending = true;
		}
	} else {
		event->length = 0U;
	}

	return 0;
}

int udc_native_linux_host_ioctl(int fd, unsigned long request, unsigned long arg)
{
	const int saved_errno = errno;
	int ret;

	if (fd != FAKE_FD) {
		errno = EBADF;
		return -1;
	}

	pthread_mutex_lock(&fake_lock);

	switch (request) {
	case USB_RAW_IOCTL_INIT:
		ret = fake_ioctl_init((const struct usb_raw_init *)arg);
		break;
	case USB_RAW_IOCTL_RUN:
		ret = fake_ioctl_run();
		break;
	case USB_RAW_IOCTL_EVENT_FETCH:
		ret = fake_ioctl_event_fetch((struct usb_raw_event *)arg);
		break;
	case USB_RAW_IOCTL_EPS_INFO:
		ret = fake_ioctl_eps_info((struct usb_raw_eps_info *)arg);
		break;
	case USB_RAW_IOCTL_EP_ENABLE:
		ret = fake_ioctl_ep_enable((const struct usb_endpoint_descriptor *)arg);
		break;
	case USB_RAW_IOCTL_EP_DISABLE:
		ret = fake_ioctl_ep_disable(arg);
		break;
	case USB_RAW_IOCTL_EP_SET_HALT:
		ret = fake_ioctl_ep_halt(arg, true);
		break;
	case USB_RAW_IOCTL_EP_CLEAR_HALT:
		ret = fake_ioctl_ep_halt(arg, false);
		break;
	case USB_RAW_IOCTL_EP0_WRITE:
		ret = fake_ioctl_ep0((struct usb_raw_ep_io *)arg, true);
		break;
	case USB_RAW_IOCTL_EP0_READ:
		ret = fake_ioctl_ep0((struct usb_raw_ep_io *)arg, false);
		break;
	case USB_RAW_IOCTL_EP0_STALL:
		ret = fake_ioctl_ep0_stall();
		break;
	case USB_RAW_IOCTL_EP_WRITE:
		ret = fake_ioctl_ep_io((struct usb_raw_ep_io *)arg, true);
		break;
	case USB_RAW_IOCTL_EP_READ:
		ret = fake_ioctl_ep_io((struct usb_raw_ep_io *)arg, false);
		break;
	case USB_RAW_IOCTL_CONFIGURE:
		fake_stats.configures++;
		ret = 0;
		break;
	default:
		ret = fake_error("unknown request", ENOTTY);
		break;
	}

	pthread_mutex_unlock(&fake_lock);

	/* A call that succeeds leaves errno alone, as the real one does */
	if (ret >= 0) {
		errno = saved_errno;
	}

	return ret;
}

void fake_raw_gadget_event(enum fake_raw_gadget_event type, const uint8_t setup[8],
			   const uint8_t *out, uint32_t out_len)
{
	pthread_mutex_lock(&fake_lock);

	if (type == FAKE_RAW_GADGET_EVT_CONTROL) {
		fake_ctrl_done = false;
		fake_ctrl_out_len = out_len < FAKE_DATA_MAX ? out_len : FAKE_DATA_MAX;
		if (out != NULL) {
			memcpy(fake_ctrl_out, out, fake_ctrl_out_len);
		}
	}

	fake_queue_event((uint32_t)type, setup);

	pthread_mutex_unlock(&fake_lock);
}

bool fake_raw_gadget_ctrl_done(int *result, uint8_t *in, uint32_t size)
{
	bool done;

	pthread_mutex_lock(&fake_lock);

	done = fake_ctrl_done;
	if (done) {
		fake_ctrl_done = false;
		*result = fake_ctrl_result;
		if (in != NULL && fake_ctrl_result > 0) {
			memcpy(in, fake_ctrl_in,
			       (uint32_t)fake_ctrl_result < size ? (uint32_t)fake_ctrl_result
								 : size);
		}
	}

	pthread_mutex_unlock(&fake_lock);

	return done;
}

void fake_raw_gadget_ep_out(uint8_t addr, const uint8_t *data, uint32_t len)
{
	struct fake_ep *ep;

	pthread_mutex_lock(&fake_lock);

	ep = fake_ep_by_addr(addr);
	if (ep != NULL) {
		ep->len = len < FAKE_DATA_MAX ? len : FAKE_DATA_MAX;
		memcpy(ep->data, data, ep->len);
		ep->done = false;
		sem_post(&ep->sem);
	}

	pthread_mutex_unlock(&fake_lock);
}

void fake_raw_gadget_ep_in(uint8_t addr)
{
	struct fake_ep *ep;

	pthread_mutex_lock(&fake_lock);

	ep = fake_ep_by_addr(addr);
	if (ep != NULL) {
		ep->done = false;
		sem_post(&ep->sem);
	}

	pthread_mutex_unlock(&fake_lock);
}

bool fake_raw_gadget_ep_out_done(uint8_t addr)
{
	struct fake_ep *ep;
	bool done = false;

	pthread_mutex_lock(&fake_lock);

	ep = fake_ep_by_addr(addr);
	if (ep != NULL && ep->done) {
		ep->done = false;
		done = true;
	}

	pthread_mutex_unlock(&fake_lock);

	return done;
}

bool fake_raw_gadget_ep_in_done(uint8_t addr, uint8_t *data, uint32_t size, uint32_t *len)
{
	struct fake_ep *ep;
	bool done = false;

	pthread_mutex_lock(&fake_lock);

	ep = fake_ep_by_addr(addr);
	if (ep != NULL && ep->done) {
		ep->done = false;
		done = true;
		*len = ep->len;
		memcpy(data, ep->data, ep->len < size ? ep->len : size);
	}

	pthread_mutex_unlock(&fake_lock);

	return done;
}

static bool fake_ep_flag(uint8_t addr, int flag)
{
	const struct fake_ep *ep;
	bool value = false;

	pthread_mutex_lock(&fake_lock);

	ep = fake_ep_by_addr(addr);
	if (ep != NULL) {
		value = flag == 0 ? true : flag == 1 ? ep->halted : ep->queued;
	}

	pthread_mutex_unlock(&fake_lock);

	return value;
}

bool fake_raw_gadget_ep_enabled(uint8_t addr)
{
	return fake_ep_flag(addr, 0);
}

bool fake_raw_gadget_ep_halted(uint8_t addr)
{
	return fake_ep_flag(addr, 1);
}

bool fake_raw_gadget_ep_queued(uint8_t addr)
{
	return fake_ep_flag(addr, 2);
}

void fake_raw_gadget_set_speed(const char *speed)
{
	char text[32];

	pthread_once(&fake_once, fake_init);
	snprintf(text, sizeof(text), "%s\n", speed);
	fake_write_file(FAKE_RAW_GADGET_UDC, "current_speed", text);
	fake_write_file(FAKE_RAW_GADGET_OTHER_UDC, "current_speed", text);
}

void fake_raw_gadget_set_halt_busy(unsigned int count)
{
	pthread_mutex_lock(&fake_lock);
	fake_halt_busy = count;
	pthread_mutex_unlock(&fake_lock);
}

void fake_raw_gadget_get_stats(struct fake_raw_gadget_stats *stats)
{
	pthread_mutex_lock(&fake_lock);
	*stats = fake_stats;
	pthread_mutex_unlock(&fake_lock);
}
