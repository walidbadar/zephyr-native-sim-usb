/*
 * SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Runs the USB device stack on the native Linux controller, with a fake
 * raw-gadget in place of the one of the host and the test as the USB host. The
 * device is the loopback function: what the host sends on an OUT endpoint, or
 * with a vendor request, is what it gets from the IN ones afterwards.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/ztest.h>

#include "fake_raw_gadget_bottom.h"

#define TIMEOUT_US 3000000U
#define TIMED_OUT  (-2)

/* Vendor requests of the loopback function */
#define LB_REQ_OUT 0x5b
#define LB_REQ_IN  0x5c

#define REQTYPE_STD_DEV_OUT    0x00
#define REQTYPE_STD_DEV_IN     0x80
#define REQTYPE_STD_IFACE_OUT  0x01
#define REQTYPE_STD_EP_OUT     0x02
#define REQTYPE_STD_EP_IN      0x82
#define REQTYPE_VENDOR_DEV_OUT 0x40
#define REQTYPE_VENDOR_DEV_IN  0xc0

USBD_DEVICE_DEFINE(test_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), 0x2fe3, 0xffff);
USBD_DESC_LANG_DEFINE(test_lang);
USBD_DESC_MANUFACTURER_DEFINE(test_mfr, "ZEPHYR");
USBD_DESC_PRODUCT_DEFINE(test_product, "UDC native Linux test");
USBD_DESC_CONFIG_DEFINE(test_fs_cfg_desc, "FS Configuration");
USBD_DESC_CONFIG_DEFINE(test_hs_cfg_desc, "HS Configuration");
USBD_CONFIGURATION_DEFINE(test_fs_config, 0, 100, &test_fs_cfg_desc);
USBD_CONFIGURATION_DEFINE(test_hs_config, 0, 100, &test_hs_cfg_desc);

/* Endpoints of the loopback function, as the configuration descriptor has them */
struct test_eps {
	uint8_t bulk_out;
	uint8_t bulk_in;
	uint8_t int_out;
	uint8_t int_in;
	uint16_t bulk_mps;
};

static struct test_eps eps;
static atomic_t suspends;
static atomic_t resets;
static unsigned int errors_before;

static void test_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	ARG_UNUSED(ctx);

	if (msg->type == USBD_MSG_SUSPEND) {
		atomic_inc(&suspends);
	} else if (msg->type == USBD_MSG_RESET) {
		atomic_inc(&resets);
	}
}

/* A control transfer of the USB host: the bytes of its data IN stage, or 0 */
static int host_ctrl(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint16_t length,
		     const uint8_t *out, uint8_t *in)
{
	const struct usb_setup_packet setup = {
		.bmRequestType = type,
		.bRequest = request,
		.wValue = sys_cpu_to_le16(value),
		.wIndex = sys_cpu_to_le16(index),
		.wLength = sys_cpu_to_le16(length),
	};
	int result = TIMED_OUT;

	fake_raw_gadget_event(FAKE_RAW_GADGET_EVT_CONTROL, (const uint8_t *)&setup, out,
			      out != NULL ? length : 0U);
	WAIT_FOR(fake_raw_gadget_ctrl_done(&result, in, length), TIMEOUT_US, k_sleep(K_MSEC(1)));

	return result;
}

static void host_get_eps(struct test_eps *found)
{
	uint8_t desc[255];
	const struct usb_ep_descriptor *ep;
	int len;

	memset(found, 0, sizeof(*found));

	len = host_ctrl(REQTYPE_STD_DEV_IN, USB_SREQ_GET_DESCRIPTOR, USB_DESC_CONFIGURATION << 8, 0,
			sizeof(desc), NULL, desc);
	zassert_true(len > 0, "no configuration descriptor (%d)", len);

	for (int i = 0; i + 1 < len && desc[i] != 0U; i += desc[i]) {
		if (desc[i + 1] != USB_DESC_ENDPOINT) {
			continue;
		}

		ep = (const struct usb_ep_descriptor *)&desc[i];

		if ((ep->bmAttributes & USB_EP_TRANSFER_TYPE_MASK) == USB_EP_TYPE_BULK) {
			found->bulk_mps = sys_le16_to_cpu(ep->wMaxPacketSize);
			if (USB_EP_DIR_IS_IN(ep->bEndpointAddress)) {
				found->bulk_in = ep->bEndpointAddress;
			} else {
				found->bulk_out = ep->bEndpointAddress;
			}
		} else if (USB_EP_DIR_IS_IN(ep->bEndpointAddress)) {
			found->int_in = ep->bEndpointAddress;
		} else {
			found->int_out = ep->bEndpointAddress;
		}
	}
}

/* The USB host resets the bus, configures the device and selects the loopback endpoints */
static void host_enumerate(void)
{
	uint8_t desc[18];

	fake_raw_gadget_event(FAKE_RAW_GADGET_EVT_RESET, NULL, NULL, 0U);

	zassert_equal(host_ctrl(REQTYPE_STD_DEV_IN, USB_SREQ_GET_DESCRIPTOR, USB_DESC_DEVICE << 8,
				0, sizeof(desc), NULL, desc),
		      sizeof(desc));
	zassert_equal(desc[0], sizeof(desc));
	zassert_equal(desc[1], USB_DESC_DEVICE);
	zassert_equal(sys_get_le16(&desc[8]), 0x2fe3);

	zassert_equal(
		host_ctrl(REQTYPE_STD_DEV_OUT, USB_SREQ_SET_CONFIGURATION, 1, 0, 0, NULL, NULL), 0);
	zassert_equal(host_ctrl(REQTYPE_STD_IFACE_OUT, USB_SREQ_SET_INTERFACE, 1, 0, 0, NULL, NULL),
		      0);

	host_get_eps(&eps);
	zassert_true(eps.bulk_out != 0U && eps.bulk_in != 0U && eps.int_out != 0U &&
		     eps.int_in != 0U);
}

/* What the host sends on <out> is what it gets on <in>, at the latest with the third transfer */
static void host_loopback(uint8_t out, uint8_t in, uint32_t size)
{
	uint8_t tx[512];
	uint8_t rx[1024];
	uint32_t len = 0U;
	bool same = false;

	for (uint32_t i = 0U; i < size; i++) {
		tx[i] = (uint8_t)(i * 7U + out);
	}

	zassert_true(WAIT_FOR(fake_raw_gadget_ep_queued(out), TIMEOUT_US, k_sleep(K_MSEC(1))));
	fake_raw_gadget_ep_out(out, tx, size);
	zassert_true(WAIT_FOR(fake_raw_gadget_ep_out_done(out), TIMEOUT_US, k_sleep(K_MSEC(1))));

	for (int i = 0; i < 3 && !same; i++) {
		fake_raw_gadget_ep_in(in);
		zassert_true(WAIT_FOR(fake_raw_gadget_ep_in_done(in, rx, sizeof(rx), &len),
				      TIMEOUT_US, k_sleep(K_MSEC(1))));
		same = len >= size && memcmp(rx, tx, size) == 0;
	}

	zassert_true(same, "ep 0x%02x does not return what ep 0x%02x got", in, out);
}

static void *suite_setup(void)
{
	zassert_ok(usbd_add_descriptor(&test_usbd, &test_lang));
	zassert_ok(usbd_add_descriptor(&test_usbd, &test_mfr));
	zassert_ok(usbd_add_descriptor(&test_usbd, &test_product));

	zassert_equal(usbd_caps_speed(&test_usbd), USBD_SPEED_HS);
	zassert_ok(usbd_add_configuration(&test_usbd, USBD_SPEED_HS, &test_hs_config));
	zassert_ok(usbd_register_all_classes(&test_usbd, USBD_SPEED_HS, 1, NULL));
	zassert_ok(usbd_add_configuration(&test_usbd, USBD_SPEED_FS, &test_fs_config));
	zassert_ok(usbd_register_all_classes(&test_usbd, USBD_SPEED_FS, 1, NULL));

	zassert_ok(usbd_msg_register_cb(&test_usbd, test_msg_cb));
	zassert_ok(usbd_init(&test_usbd));
	zassert_ok(usbd_enable(&test_usbd));

	return NULL;
}

static void before(void *fixture)
{
	struct fake_raw_gadget_stats stats;

	ARG_UNUSED(fixture);

	fake_raw_gadget_get_stats(&stats);
	errors_before = stats.errors;

	fake_raw_gadget_set_speed("high-speed");
	host_enumerate();
}

static void after(void *fixture)
{
	struct fake_raw_gadget_stats stats;

	ARG_UNUSED(fixture);

	fake_raw_gadget_get_stats(&stats);
	zassert_equal(stats.errors, errors_before, "raw-gadget would have refused: %s",
		      stats.last_error);
}

ZTEST(udc_native_linux, test_enumeration)
{
	struct fake_raw_gadget_stats stats;
	uint8_t config = 0U;

	fake_raw_gadget_get_stats(&stats);

	/* The controller of the host-udc property, not the first one found */
	zassert_str_equal(stats.udc, FAKE_RAW_GADGET_UDC);
	zassert_str_equal(stats.driver, FAKE_RAW_GADGET_DRIVER);
	zassert_true(stats.high_speed);
	zassert_true(stats.configures > 0U);

	zassert_equal(
		host_ctrl(REQTYPE_STD_DEV_IN, USB_SREQ_GET_CONFIGURATION, 0, 0, 1, NULL, &config),
		1);
	zassert_equal(config, 1U);

	zassert_equal(eps.bulk_mps, 512U);
	zassert_true(fake_raw_gadget_ep_enabled(eps.bulk_out));
	zassert_true(fake_raw_gadget_ep_enabled(eps.bulk_in));
	zassert_true(fake_raw_gadget_ep_enabled(eps.int_out));
	zassert_true(fake_raw_gadget_ep_enabled(eps.int_in));
}

ZTEST(udc_native_linux, test_control_data)
{
	uint8_t tx[64];
	uint8_t rx[64] = {0};

	for (size_t i = 0; i < sizeof(tx); i++) {
		tx[i] = (uint8_t)(0xa5U ^ i);
	}

	zassert_equal(host_ctrl(REQTYPE_VENDOR_DEV_OUT, LB_REQ_OUT, 0, 0, sizeof(tx), tx, NULL), 0);
	zassert_equal(host_ctrl(REQTYPE_VENDOR_DEV_IN, LB_REQ_IN, 0, 0, sizeof(rx), NULL, rx),
		      sizeof(rx));
	zassert_mem_equal(rx, tx, sizeof(tx));
}

ZTEST(udc_native_linux, test_control_stall)
{
	uint8_t rx[8];

	/* Not a request of the loopback function, with and without a data stage */
	zassert_equal(host_ctrl(REQTYPE_VENDOR_DEV_IN, 0x01, 0, 0, sizeof(rx), NULL, rx),
		      FAKE_RAW_GADGET_STALLED);
	zassert_equal(host_ctrl(REQTYPE_VENDOR_DEV_OUT, 0x01, 0, 0, 0, NULL, NULL),
		      FAKE_RAW_GADGET_STALLED);

	/* The control endpoint serves the next request */
	zassert_equal(host_ctrl(REQTYPE_VENDOR_DEV_IN, LB_REQ_IN, 0, 0, sizeof(rx), NULL, rx),
		      sizeof(rx));
}

ZTEST(udc_native_linux, test_bulk)
{
	host_loopback(eps.bulk_out, eps.bulk_in, 512U);
	host_loopback(eps.bulk_out, eps.bulk_in, 100U);
}

ZTEST(udc_native_linux, test_interrupt)
{
	host_loopback(eps.int_out, eps.int_in, 64U);
}

ZTEST(udc_native_linux, test_halt)
{
	struct fake_raw_gadget_stats start;
	struct fake_raw_gadget_stats stats;
	uint8_t status[2] = {0xff, 0xff};

	fake_raw_gadget_get_stats(&start);

	/* The device has a transfer waiting for the host, and data that the host has not taken */
	zassert_true(
		WAIT_FOR(fake_raw_gadget_ep_queued(eps.bulk_in), TIMEOUT_US, k_sleep(K_MSEC(1))));
	fake_raw_gadget_set_halt_busy(2U);

	zassert_equal(host_ctrl(REQTYPE_STD_EP_OUT, USB_SREQ_SET_FEATURE, USB_SFS_ENDPOINT_HALT,
				eps.bulk_in, 0, NULL, NULL),
		      0);
	zassert_true(
		WAIT_FOR(fake_raw_gadget_ep_halted(eps.bulk_in), TIMEOUT_US, k_sleep(K_MSEC(1))));

	/*
	 * A host controller answers the request that clears the halt itself. The
	 * driver has told the stack in its place, and the transfer waits again.
	 */
	zassert_true(WAIT_FOR(host_ctrl(REQTYPE_STD_EP_IN, USB_SREQ_GET_STATUS, 0, eps.bulk_in,
					sizeof(status), NULL, status) == sizeof(status) &&
				      sys_get_le16(status) == 0U,
			      TIMEOUT_US, k_sleep(K_MSEC(1))));
	zassert_true(
		WAIT_FOR(fake_raw_gadget_ep_queued(eps.bulk_in), TIMEOUT_US, k_sleep(K_MSEC(1))));

	fake_raw_gadget_get_stats(&stats);
	zassert_equal(stats.set_halts, start.set_halts + 3U);
	zassert_equal(stats.clear_halts, start.clear_halts);
	zassert_true(fake_raw_gadget_ep_halted(eps.bulk_in));

	/* A host controller that passes the request on has the halt cleared by the driver */
	zassert_equal(host_ctrl(REQTYPE_STD_EP_OUT, USB_SREQ_CLEAR_FEATURE, USB_SFS_ENDPOINT_HALT,
				eps.bulk_in, 0, NULL, NULL),
		      0);

	fake_raw_gadget_get_stats(&stats);
	zassert_equal(stats.clear_halts, start.clear_halts + 1U);
	zassert_false(fake_raw_gadget_ep_halted(eps.bulk_in));
	zassert_true(
		WAIT_FOR(fake_raw_gadget_ep_queued(eps.bulk_in), TIMEOUT_US, k_sleep(K_MSEC(1))));

	host_loopback(eps.bulk_out, eps.bulk_in, 256U);
}

ZTEST(udc_native_linux, test_bus_reset)
{
	struct fake_raw_gadget_stats start;
	struct fake_raw_gadget_stats stats;
	uint8_t config = 0xffU;

	fake_raw_gadget_get_stats(&start);

	fake_raw_gadget_event(FAKE_RAW_GADGET_EVT_RESET, NULL, NULL, 0U);

	/* The device is not configured after the reset, and its endpoints are gone */
	zassert_equal(
		host_ctrl(REQTYPE_STD_DEV_IN, USB_SREQ_GET_CONFIGURATION, 0, 0, 1, NULL, &config),
		1);
	zassert_equal(config, 0U);

	fake_raw_gadget_get_stats(&stats);
	zassert_equal(stats.ep_disables, start.ep_disables + 4U);
}

ZTEST(udc_native_linux, test_full_speed_host)
{
	/* The device is a high speed one, the host and the controller settle on full speed */
	fake_raw_gadget_set_speed("full-speed");
	host_enumerate();

	zassert_equal(eps.bulk_mps, 64U);
	host_loopback(eps.bulk_out, eps.bulk_in, 64U);
}

ZTEST(udc_native_linux, test_disconnect)
{
	const atomic_val_t suspends_before = atomic_get(&suspends);
	const atomic_val_t resets_before = atomic_get(&resets);

	fake_raw_gadget_event(FAKE_RAW_GADGET_EVT_DISCONNECT, NULL, NULL, 0U);
	zassert_true(
		WAIT_FOR(atomic_get(&suspends) > suspends_before, TIMEOUT_US, k_sleep(K_MSEC(1))));

	/* The first request after it comes with a bus reset */
	host_get_eps(&eps);
	zassert_true(atomic_get(&resets) > resets_before);
}

ZTEST(udc_native_linux, test_disable_enable)
{
	struct fake_raw_gadget_stats start;
	struct fake_raw_gadget_stats stats;

	fake_raw_gadget_get_stats(&start);

	zassert_ok(usbd_disable(&test_usbd));
	fake_raw_gadget_get_stats(&stats);
	zassert_equal(stats.closes, start.closes + 1U);

	zassert_ok(usbd_enable(&test_usbd));
	fake_raw_gadget_get_stats(&stats);
	zassert_equal(stats.opens, start.opens + 1U);

	host_enumerate();
	host_loopback(eps.bulk_out, eps.bulk_in, 512U);
}

ZTEST_SUITE(udc_native_linux, NULL, suite_setup, before, after, NULL);
