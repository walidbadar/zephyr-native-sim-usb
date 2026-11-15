/*
 * SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FAKE_RAW_GADGET_BOTTOM_H
#define FAKE_RAW_GADGET_BOTTOM_H

#include <stdbool.h>
#include <stdint.h>

/* Host controllers of the fake, both of the driver FAKE_RAW_GADGET_DRIVER */
#define FAKE_RAW_GADGET_UDC       "fake_udc.1"
#define FAKE_RAW_GADGET_OTHER_UDC "fake_udc.0"
#define FAKE_RAW_GADGET_DRIVER    "fake_udc"

#define FAKE_RAW_GADGET_NAME_MAX 32

/* Result of a control transfer that the device has stalled */
#define FAKE_RAW_GADGET_STALLED (-1)

/* Numbered as raw-gadget does */
enum fake_raw_gadget_event {
	FAKE_RAW_GADGET_EVT_CONNECT = 1,
	FAKE_RAW_GADGET_EVT_CONTROL = 2,
	FAKE_RAW_GADGET_EVT_SUSPEND = 3,
	FAKE_RAW_GADGET_EVT_RESUME = 4,
	FAKE_RAW_GADGET_EVT_RESET = 5,
	FAKE_RAW_GADGET_EVT_DISCONNECT = 6,
};

struct fake_raw_gadget_stats {
	/* What the driver has bound */
	char udc[FAKE_RAW_GADGET_NAME_MAX];
	char driver[FAKE_RAW_GADGET_NAME_MAX];
	bool high_speed;
	unsigned int opens;
	unsigned int closes;
	unsigned int configures;
	unsigned int ep_enables;
	unsigned int ep_disables;
	unsigned int set_halts;
	unsigned int clear_halts;
	/* Calls that raw-gadget would have refused, with the last one */
	unsigned int errors;
	const char *last_error;
};

/*
 * None of these blocks: the device answers while simulated time goes on, so
 * the test polls the *_done() functions between sleeps.
 */

/* Queue an event. <setup> and the <out_len> bytes of a data OUT stage are for a CONTROL one. */
void fake_raw_gadget_event(enum fake_raw_gadget_event type, const uint8_t setup[8],
			   const uint8_t *out, uint32_t out_len);

/* True once the device has answered the last CONTROL event, with the data of an IN stage */
bool fake_raw_gadget_ctrl_done(int *result, uint8_t *in, uint32_t size);

/* The USB host sends <len> bytes to the OUT endpoint <ep> */
void fake_raw_gadget_ep_out(uint8_t ep, const uint8_t *data, uint32_t len);
bool fake_raw_gadget_ep_out_done(uint8_t ep);

/* The USB host takes one transfer of the IN endpoint <ep> */
void fake_raw_gadget_ep_in(uint8_t ep);
bool fake_raw_gadget_ep_in_done(uint8_t ep, uint8_t *data, uint32_t size, uint32_t *len);

bool fake_raw_gadget_ep_enabled(uint8_t ep);
bool fake_raw_gadget_ep_halted(uint8_t ep);
/* The device has a transfer of the endpoint in progress */
bool fake_raw_gadget_ep_queued(uint8_t ep);

/* What the host controller reports as the speed of the connection, e.g. "full-speed" */
void fake_raw_gadget_set_speed(const char *speed);

/* The next <count> attempts to halt an endpoint find data that the USB host has yet to take */
void fake_raw_gadget_set_halt_busy(unsigned int count);

void fake_raw_gadget_get_stats(struct fake_raw_gadget_stats *stats);

#endif /* FAKE_RAW_GADGET_BOTTOM_H */
