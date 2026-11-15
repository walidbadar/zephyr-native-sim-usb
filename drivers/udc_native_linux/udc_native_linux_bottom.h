/*
 * SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef UDC_NATIVE_LINUX_BOTTOM_H
#define UDC_NATIVE_LINUX_BOTTOM_H

#include <stdbool.h>
#include <stdint.h>

/* Endpoints of a host controller, other than the control one */
#define UDC_NATIVE_LINUX_EPS_MAX 30

/* Transfer handle of the control endpoint, the others are host endpoints */
#define UDC_NATIVE_LINUX_EP0 UDC_NATIVE_LINUX_EPS_MAX

/* Number of a host endpoint that takes any */
#define UDC_NATIVE_LINUX_EP_NUM_ANY 0xffU

/* Numbered as raw-gadget does up to UDC_NATIVE_LINUX_EVT_DISCONNECT */
enum udc_native_linux_event {
	UDC_NATIVE_LINUX_EVT_NONE,
	UDC_NATIVE_LINUX_EVT_CONNECT,
	UDC_NATIVE_LINUX_EVT_SETUP,
	UDC_NATIVE_LINUX_EVT_SUSPEND,
	UDC_NATIVE_LINUX_EVT_RESUME,
	UDC_NATIVE_LINUX_EVT_RESET,
	UDC_NATIVE_LINUX_EVT_DISCONNECT,
	/* The host controller is gone, no event follows */
	UDC_NATIVE_LINUX_EVT_FAILED,
};

enum udc_native_linux_speed {
	/* The USB host and the host controller have not agreed on one */
	UDC_NATIVE_LINUX_SPEED_UNKNOWN,
	UDC_NATIVE_LINUX_SPEED_FULL,
	UDC_NATIVE_LINUX_SPEED_HIGH,
};

struct udc_native_linux_ep {
	uint8_t num;
	uint16_t mps;
	bool in;
	bool out;
	bool iso;
	bool bulk;
	bool interrupt;
};

/* Errors are negative nsi_errno.h values */

/*
 * Bind the host controller <udc> of /sys/class/udc, the first one found when
 * NULL, and connect it to its USB host. <irq> is raised when an event or the
 * result of a transfer is there, checked every <poll_us> of simulated time;
 * both are shared by all instances. Returns a handle, or NULL with the error
 * in <err>.
 */
void *udc_native_linux_open(const char *udc, bool high_speed, uint32_t irq, uint32_t poll_us,
			    int *err);

/* Disconnect from the USB host and release the host controller */
void udc_native_linux_close(void *handle);

/* Close all open host controllers */
void udc_native_linux_close_all(void);

/* Speed of the connection to the USB host, known once the host sends requests */
enum udc_native_linux_speed udc_native_linux_speed(void *handle);

/* Fill <eps>, of UDC_NATIVE_LINUX_EPS_MAX entries, and return how many there are */
int udc_native_linux_eps(void *handle, struct udc_native_linux_ep *eps);

/* Next event in order of arrival, with the request in <setup> for a SETUP one */
enum udc_native_linux_event udc_native_linux_event_get(void *handle, uint8_t setup[8]);

/* Returns the host endpoint that the endpoint is enabled on */
int udc_native_linux_ep_enable(void *handle, uint8_t addr, uint8_t attributes, uint16_t mps,
			       uint8_t interval);

/* The three below fail while a transfer of the endpoint is in progress */
int udc_native_linux_ep_disable(void *handle, int ep);
int udc_native_linux_ep_clear_halt(void *handle, int ep);
int udc_native_linux_ep0_stall(void *handle);

/* Tell the host controller that the USB host has configured the device */
int udc_native_linux_configure(void *handle);

/*
 * Hand a transfer of <len> bytes to the host thread of the endpoint. <data> is
 * copied for an IN one. On the control endpoint, an OUT transfer of no bytes
 * acknowledges a request without data stage.
 */
int udc_native_linux_xfer_start(void *handle, int ep, bool in, const uint8_t *data, uint32_t len,
				bool zlp);

/*
 * Have the host thread of the endpoint halt it, which an IN endpoint only takes
 * once the USB host has collected the data sent last. Done like a transfer.
 */
int udc_native_linux_halt_start(void *handle, int ep);

/*
 * True once per transfer when it is done, with the number of bytes or the
 * error in <result>. Up to <size> received bytes are copied to <data>.
 */
bool udc_native_linux_xfer_done(void *handle, int ep, uint8_t *data, uint32_t size, int *result);

/* Abort the transfer or the halt of the endpoint, if any. Its result is dropped. */
void udc_native_linux_xfer_cancel(void *handle, int ep);

#endif /* UDC_NATIVE_LINUX_BOTTOM_H */
