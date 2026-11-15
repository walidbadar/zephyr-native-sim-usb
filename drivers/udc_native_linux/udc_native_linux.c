/*
 * SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "udc_common.h"
#include "udc_native_linux_bottom.h"

#include <string.h>

#include <cmdline.h>
#include <nsi_errno.h>
#include <posix_native_task.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usb_ch9.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(udc_native_linux, CONFIG_UDC_DRIVER_LOG_LEVEL);

#define UDC_NL_IRQ_PRIORITY 2

/* Endpoint numbers 0 to 15 */
#define UDC_NL_NUM_EPS 16U

/* Index of "high-speed" in the maximum-speed property */
#define UDC_NL_SPEED_IDX_HS 2

/* Bit of an endpoint in udc_nl_data.halt_requests */
#define UDC_NL_HALT_BIT(ep) BIT(USB_EP_GET_IDX(ep) + (USB_EP_DIR_IS_IN(ep) ? UDC_NL_NUM_EPS : 0U))

enum udc_nl_ctrl {
	UDC_NL_CTRL_IDLE,
	/* Request of the USB host */
	UDC_NL_CTRL_HOST,
	/* Request made up for the stack, see udc_nl_local_request() */
	UDC_NL_CTRL_LOCAL,
};

struct udc_nl_config {
	struct udc_ep_config *ep_cfg_in;
	struct udc_ep_config *ep_cfg_out;
	/* NULL for the first host controller found */
	const char *host_udc;
	bool high_speed;
};

/* --udc: host controller of all instances, in place of the one of their devicetree node */
static char *udc_nl_cmdline_udc;

struct udc_nl_data {
	const struct device *dev;
	struct k_work work;
	/* Host controller, NULL while the device is disabled */
	void *host;
	/* Host endpoint of each enabled endpoint, negative when it has none */
	int8_t host_ep_in[UDC_NL_NUM_EPS];
	int8_t host_ep_out[UDC_NL_NUM_EPS];
	/* Control transfer that the stack is busy with */
	enum udc_nl_ctrl ctrl;
	/* Request of the USB host, which waits for its data or status while setup_pending */
	struct usb_setup_packet setup;
	bool setup_pending;
	/* Speed of the connection to the USB host */
	bool high_speed;
	/* The bus was reset or lost since the last request of the USB host */
	bool bus_reset;
	/* The stack has yet to handle the bus reset, and to get the request that followed it */
	bool reset_pending;
	bool setup_deferred;
	/* Requests to make up for the stack */
	bool addr_request;
	uint32_t halt_requests;
	/* Endpoints halted again while their halt was cleared, see udc_nl_ctrl_idle() */
	uint32_t halt_retries;
	/* Endpoints that the host controller has yet to halt */
	uint32_t halts_busy;
};

static int udc_nl_errno(int ret)
{
	return -nsi_errno_from_mid(-ret);
}

static int8_t *udc_nl_host_ep(const struct device *dev, const uint8_t ep)
{
	struct udc_nl_data *priv = udc_get_private(dev);

	return USB_EP_DIR_IS_IN(ep) ? &priv->host_ep_in[USB_EP_GET_IDX(ep)]
				    : &priv->host_ep_out[USB_EP_GET_IDX(ep)];
}

static void udc_nl_submit_work(const struct device *dev)
{
	struct udc_nl_data *priv = udc_get_private(dev);

	k_work_submit_to_queue(udc_get_work_q(), &priv->work);
}

/* Stop the transfer of the endpoint. Its buffer stays first in the queue. */
static void udc_nl_ep_abort(const struct device *dev, struct udc_ep_config *const cfg)
{
	struct udc_nl_data *priv = udc_get_private(dev);
	const int8_t host_ep = *udc_nl_host_ep(dev, cfg->addr);

	if (priv->host != NULL && host_ep >= 0 && udc_ep_is_busy(cfg)) {
		udc_native_linux_xfer_cancel(priv->host, host_ep);
	}

	udc_ep_set_busy(cfg, false);
}

/*
 * The stack is done with a control transfer. An endpoint that a class has
 * halted again when told that its halt was cleared stays halted for the stack
 * until the USB host has had a request served, as a recovery takes one.
 */
static void udc_nl_ctrl_idle(const struct device *dev)
{
	struct udc_nl_data *priv = udc_get_private(dev);

	if (priv->ctrl == UDC_NL_CTRL_HOST) {
		priv->halt_requests |= priv->halt_retries;
		priv->halt_retries = 0U;
	}

	priv->setup_pending = false;
	priv->ctrl = UDC_NL_CTRL_IDLE;
}

/* Give up the halt of the endpoint that the host controller has not done yet */
static void udc_nl_halt_abort(const struct device *dev, struct udc_ep_config *const cfg)
{
	struct udc_nl_data *priv = udc_get_private(dev);

	if ((priv->halts_busy & UDC_NL_HALT_BIT(cfg->addr)) != 0U) {
		udc_native_linux_xfer_cancel(priv->host, *udc_nl_host_ep(dev, cfg->addr));
		priv->halts_busy &= ~UDC_NL_HALT_BIT(cfg->addr);
	}
}

static void udc_nl_ep_done(const struct device *dev, struct udc_ep_config *const cfg, const int err)
{
	struct net_buf *buf = udc_buf_get(cfg);

	udc_ep_set_busy(cfg, false);
	udc_submit_ep_event(dev, buf, err);
}

/* Take the result of the transfer in progress, then start the next one in the queue */
static void udc_nl_ep_work(const struct device *dev, struct udc_ep_config *const cfg)
{
	struct udc_nl_data *priv = udc_get_private(dev);
	const int8_t host_ep = *udc_nl_host_ep(dev, cfg->addr);
	const bool in = USB_EP_DIR_IS_IN(cfg->addr);
	struct net_buf *buf;
	uint32_t len;
	int ret;

	if (host_ep < 0) {
		return;
	}

	if ((priv->halts_busy & UDC_NL_HALT_BIT(cfg->addr)) != 0U) {
		if (!udc_native_linux_xfer_done(priv->host, host_ep, NULL, 0U, &ret)) {
			return;
		}

		priv->halts_busy &= ~UDC_NL_HALT_BIT(cfg->addr);
		if (ret != 0) {
			LOG_ERR("Failed to halt ep 0x%02x (%d)", cfg->addr, udc_nl_errno(ret));
		}
	}

	buf = udc_buf_peek(cfg);

	if (buf != NULL && udc_ep_is_busy(cfg)) {
		if (!udc_native_linux_xfer_done(priv->host, host_ep, in ? NULL : net_buf_tail(buf),
						net_buf_tailroom(buf), &ret)) {
			return;
		}

		if (ret < 0) {
			LOG_DBG("ep 0x%02x transfer failed (%d)", cfg->addr, udc_nl_errno(ret));
			udc_nl_ep_done(dev, cfg, -ECONNABORTED);
		} else {
			if (in) {
				net_buf_pull(buf, MIN((size_t)ret, buf->len));
			} else {
				net_buf_add(buf, MIN((size_t)ret, net_buf_tailroom(buf)));
			}

			udc_nl_ep_done(dev, cfg, 0);
		}

		buf = udc_buf_peek(cfg);
	}

	while (buf != NULL && !cfg->stat.halted) {
		if (in) {
			ret = udc_native_linux_xfer_start(priv->host, host_ep, true, buf->data,
							  buf->len, udc_ep_buf_has_zlp(buf));
		} else {
			/* Host controllers may want whole packets, the surplus is dropped */
			len = ROUND_UP(net_buf_tailroom(buf), udc_mps_ep_size(cfg));
			ret = udc_native_linux_xfer_start(priv->host, host_ep, false, NULL, len,
							  false);
		}

		if (ret == 0) {
			udc_ep_set_busy(cfg, true);
			break;
		}

		LOG_ERR("Failed to start a transfer on ep 0x%02x (%d)", cfg->addr,
			udc_nl_errno(ret));
		udc_nl_ep_done(dev, cfg, udc_nl_errno(ret));
		buf = udc_buf_peek(cfg);
	}
}

static bool udc_nl_setup_is_set_configuration(const struct usb_setup_packet *const setup)
{
	return setup->bmRequestType == 0U && setup->bRequest == USB_SREQ_SET_CONFIGURATION &&
	       setup->wValue != 0U;
}

/* Data IN stage and status IN stage of the control transfer in progress */
static void udc_nl_ctrl_in(const struct device *dev)
{
	struct udc_ep_config *cfg = udc_get_ep_cfg(dev, USB_CONTROL_EP_IN);
	struct udc_nl_data *priv = udc_get_private(dev);
	struct net_buf *buf = udc_buf_peek(cfg);
	struct udc_buf_info *bi;
	int ret;

	if (buf == NULL) {
		return;
	}

	bi = udc_get_buf_info(buf);

	if (udc_ep_is_busy(cfg)) {
		if (!udc_native_linux_xfer_done(priv->host, UDC_NATIVE_LINUX_EP0, NULL, 0U, &ret)) {
			return;
		}

		udc_nl_ctrl_idle(dev);

		if (ret < 0) {
			LOG_DBG("Control IN stage failed (%d)", udc_nl_errno(ret));
			udc_nl_ep_done(dev, cfg, -ECONNABORTED);
			return;
		}

		net_buf_pull(buf, MIN((size_t)ret, buf->len));
		udc_nl_ep_done(dev, cfg, 0);

		return;
	}

	if (!priv->setup_pending) {
		/*
		 * Status of a made up request, or of a request with a data OUT
		 * stage, which the host controller has sent with the last data.
		 */
		udc_nl_ctrl_idle(dev);
		udc_nl_ep_done(dev, cfg, 0);

		return;
	}

	if (bi->data) {
		ret = udc_native_linux_xfer_start(priv->host, UDC_NATIVE_LINUX_EP0, true, buf->data,
						  buf->len, udc_ep_buf_has_zlp(buf));
	} else {
		if (udc_nl_setup_is_set_configuration(&priv->setup)) {
			(void)udc_native_linux_configure(priv->host);
		}

		/* Status of a request without data stage */
		ret = udc_native_linux_xfer_start(priv->host, UDC_NATIVE_LINUX_EP0, false, NULL, 0U,
						  false);
	}

	if (ret != 0) {
		LOG_ERR("Failed to start a control IN stage (%d)", udc_nl_errno(ret));
		udc_nl_ctrl_idle(dev);
		udc_nl_ep_done(dev, cfg, udc_nl_errno(ret));

		return;
	}

	udc_ep_set_busy(cfg, true);
}

/* Data OUT stage of the control transfer in progress */
static void udc_nl_ctrl_out(const struct device *dev)
{
	struct udc_ep_config *cfg = udc_get_ep_cfg(dev, USB_CONTROL_EP_OUT);
	struct udc_nl_data *priv = udc_get_private(dev);
	struct net_buf *buf = udc_buf_peek(cfg);
	uint32_t len;
	int ret;

	/* The buffer for the next request stays until udc_setup_received() */
	if (buf == NULL || udc_get_buf_info(buf)->setup) {
		return;
	}

	if (udc_ep_is_busy(cfg)) {
		if (!udc_native_linux_xfer_done(priv->host, UDC_NATIVE_LINUX_EP0, net_buf_tail(buf),
						net_buf_tailroom(buf), &ret)) {
			return;
		}

		/* The status stage follows without the stack, which cannot refuse the data */
		priv->setup_pending = false;

		if (ret < 0) {
			LOG_DBG("Control OUT stage failed (%d)", udc_nl_errno(ret));
			udc_nl_ctrl_idle(dev);
			udc_nl_ep_done(dev, cfg, -ECONNABORTED);
			return;
		}

		net_buf_add(buf, MIN((size_t)ret, net_buf_tailroom(buf)));
		udc_nl_ep_done(dev, cfg, 0);

		return;
	}

	if (!priv->setup_pending) {
		udc_nl_ep_done(dev, cfg, -ECONNABORTED);
		return;
	}

	len = MIN(sys_le16_to_cpu(priv->setup.wLength), net_buf_tailroom(buf));
	ret = udc_native_linux_xfer_start(priv->host, UDC_NATIVE_LINUX_EP0, false, NULL, len,
					  false);
	if (ret != 0) {
		LOG_ERR("Failed to start a control OUT stage (%d)", udc_nl_errno(ret));
		udc_nl_ctrl_idle(dev);
		udc_nl_ep_done(dev, cfg, udc_nl_errno(ret));

		return;
	}

	udc_ep_set_busy(cfg, true);
}

static void udc_nl_setup(const struct device *dev, const enum udc_nl_ctrl ctrl,
			 const struct usb_setup_packet *const setup)
{
	struct udc_nl_data *priv = udc_get_private(dev);

	/* The stages of the previous request, if any are left, are over */
	udc_native_linux_xfer_cancel(priv->host, UDC_NATIVE_LINUX_EP0);

	priv->ctrl = ctrl;
	udc_setup_received(dev, setup);
}

/*
 * Linux controller drivers answer SET_ADDRESS and CLEAR_FEATURE(ENDPOINT_HALT)
 * themselves. The stack gets them from here: the first to leave the default
 * state after a reset, the second for an endpoint that it has halted, as the
 * host controller holds the transfers of that endpoint back until the USB host
 * has cleared the halt.
 */
static void udc_nl_local_request(const struct device *dev)
{
	struct udc_nl_data *priv = udc_get_private(dev);
	const uint32_t halted = priv->halt_requests & ~priv->halts_busy;
	struct usb_setup_packet setup = {0};
	uint32_t bit;

	if (priv->addr_request) {
		priv->addr_request = false;
		setup.bRequest = USB_SREQ_SET_ADDRESS;
		setup.wValue = sys_cpu_to_le16(1U);
	} else if (halted != 0U) {
		bit = find_lsb_set(halted) - 1U;
		priv->halt_requests &= ~BIT(bit);
		setup.RequestType.recipient = USB_REQTYPE_RECIPIENT_ENDPOINT;
		setup.bRequest = USB_SREQ_CLEAR_FEATURE;
		setup.wValue = sys_cpu_to_le16(USB_SFS_ENDPOINT_HALT);
		setup.wIndex = sys_cpu_to_le16(
			bit < UDC_NL_NUM_EPS ? bit : USB_EP_DIR_IN | (bit - UDC_NL_NUM_EPS));
	} else {
		return;
	}

	udc_nl_setup(dev, UDC_NL_CTRL_LOCAL, &setup);
}

/* True when the state has changed: host controllers report a resume after a reset too */
static bool udc_nl_set_suspended(const struct device *dev, const bool suspended)
{
	if (udc_is_suspended(dev) == suspended) {
		return false;
	}

	udc_set_suspended(dev, suspended);

	return true;
}

/*
 * Host controllers report a bus reset in different ways, as a reset, as a
 * disconnect or not at all after the first one, and the speed is only settled
 * some time after it. The stack therefore gets the reset with the first
 * request that the USB host sends after any of them.
 */
static void udc_nl_bus_reset(const struct device *dev)
{
	const struct udc_nl_config *config = dev->config;
	struct udc_nl_data *priv = udc_get_private(dev);

	priv->high_speed = config->high_speed &&
			   udc_native_linux_speed(priv->host) != UDC_NATIVE_LINUX_SPEED_FULL;
	priv->halt_requests = 0U;
	priv->halt_retries = 0U;
	priv->reset_pending = true;
	udc_nl_set_suspended(dev, false);
	udc_submit_event(dev, UDC_EVT_RESET, 0);
}

/* One request at a time: the next event waits for the stack to be done with the last */
static void udc_nl_events(const struct device *dev)
{
	struct udc_nl_data *priv = udc_get_private(dev);
	struct usb_setup_packet setup;

	while (true) {
		udc_nl_local_request(dev);

		if (priv->ctrl != UDC_NL_CTRL_IDLE || priv->reset_pending) {
			return;
		}

		if (priv->setup_deferred) {
			priv->setup_deferred = false;
			priv->setup_pending = true;
			udc_nl_setup(dev, UDC_NL_CTRL_HOST, &priv->setup);
			continue;
		}

		switch (udc_native_linux_event_get(priv->host, (uint8_t *)&setup)) {
		case UDC_NATIVE_LINUX_EVT_NONE:
			return;
		case UDC_NATIVE_LINUX_EVT_CONNECT:
			__fallthrough;
		case UDC_NATIVE_LINUX_EVT_RESET:
			priv->bus_reset = true;
			break;
		case UDC_NATIVE_LINUX_EVT_SETUP:
			priv->setup = setup;

			if (priv->bus_reset) {
				priv->bus_reset = false;
				priv->setup_deferred = true;
				udc_nl_bus_reset(dev);
				break;
			}

			priv->setup_pending = true;
			udc_nl_setup(dev, UDC_NL_CTRL_HOST, &setup);
			break;
		case UDC_NATIVE_LINUX_EVT_SUSPEND:
			if (udc_nl_set_suspended(dev, true)) {
				udc_submit_event(dev, UDC_EVT_SUSPEND, 0);
			}
			break;
		case UDC_NATIVE_LINUX_EVT_RESUME:
			if (udc_nl_set_suspended(dev, false)) {
				udc_submit_event(dev, UDC_EVT_RESUME, 0);
			}
			break;
		case UDC_NATIVE_LINUX_EVT_DISCONNECT:
			/* As a controller without VBUS detection sees a pulled cable */
			priv->bus_reset = true;
			if (udc_nl_set_suspended(dev, true)) {
				udc_submit_event(dev, UDC_EVT_SUSPEND, 0);
			}
			break;
		case UDC_NATIVE_LINUX_EVT_FAILED:
			LOG_ERR("The host controller is gone");
			udc_submit_event(dev, UDC_EVT_ERROR, -EIO);
			return;
		default:
			break;
		}
	}
}

static void udc_nl_work(struct k_work *work)
{
	struct udc_nl_data *priv = CONTAINER_OF(work, struct udc_nl_data, work);
	const struct device *dev = priv->dev;
	const struct udc_nl_config *config = dev->config;

	udc_lock_internal(dev, K_FOREVER);

	if (priv->host != NULL) {
		udc_nl_ctrl_in(dev);
		udc_nl_ctrl_out(dev);

		for (uint8_t i = 1U; i < UDC_NL_NUM_EPS; i++) {
			udc_nl_ep_work(dev, &config->ep_cfg_in[i]);
			udc_nl_ep_work(dev, &config->ep_cfg_out[i]);
		}

		udc_nl_events(dev);
	}

	udc_unlock_internal(dev);
}

static int udc_nl_ep_enqueue(const struct device *dev, struct udc_ep_config *const cfg,
			     struct net_buf *buf)
{
	udc_buf_put(cfg, buf);
	udc_nl_submit_work(dev);

	return 0;
}

static int udc_nl_ep_dequeue(const struct device *dev, struct udc_ep_config *const cfg)
{
	udc_nl_ep_abort(dev, cfg);
	udc_ep_cancel_queued(dev, cfg);

	return 0;
}

static int udc_nl_ep_enable(const struct device *dev, struct udc_ep_config *const cfg)
{
	struct udc_nl_data *priv = udc_get_private(dev);
	int8_t *host_ep = udc_nl_host_ep(dev, cfg->addr);
	int ret;

	if (USB_EP_GET_IDX(cfg->addr) == 0U) {
		*host_ep = UDC_NATIVE_LINUX_EP0;
		return 0;
	}

	if (priv->host == NULL) {
		return -ENODEV;
	}

	ret = udc_native_linux_ep_enable(priv->host, cfg->addr, cfg->attributes, cfg->mps,
					 cfg->interval);
	if (ret < 0) {
		LOG_ERR("No host endpoint for ep 0x%02x (%d)", cfg->addr, udc_nl_errno(ret));
		return udc_nl_errno(ret);
	}

	*host_ep = (int8_t)ret;
	LOG_DBG("ep 0x%02x is on host endpoint %d", cfg->addr, ret);

	return 0;
}

static int udc_nl_ep_disable(const struct device *dev, struct udc_ep_config *const cfg)
{
	struct udc_nl_data *priv = udc_get_private(dev);
	int8_t *host_ep = udc_nl_host_ep(dev, cfg->addr);
	int ret;

	udc_nl_ep_abort(dev, cfg);

	if (USB_EP_GET_IDX(cfg->addr) != 0U) {
		if (priv->host != NULL && *host_ep >= 0) {
			udc_nl_halt_abort(dev, cfg);

			ret = udc_native_linux_ep_disable(priv->host, *host_ep);
			if (ret != 0) {
				LOG_WRN("Failed to disable ep 0x%02x (%d)", cfg->addr,
					udc_nl_errno(ret));
			}
		}

		priv->halt_requests &= ~UDC_NL_HALT_BIT(cfg->addr);
		priv->halt_retries &= ~UDC_NL_HALT_BIT(cfg->addr);
	}

	*host_ep = -1;

	return 0;
}

static int udc_nl_ep_set_halt(const struct device *dev, struct udc_ep_config *const cfg)
{
	struct udc_nl_data *priv = udc_get_private(dev);
	int ret;

	if (USB_EP_GET_IDX(cfg->addr) == 0U) {
		/* Too late once the host controller has sent the status */
		if (priv->setup_pending) {
			udc_nl_ep_abort(dev, udc_get_ep_cfg(dev, USB_CONTROL_EP_IN));
			udc_nl_ep_abort(dev, udc_get_ep_cfg(dev, USB_CONTROL_EP_OUT));

			ret = udc_native_linux_ep0_stall(priv->host);
			if (ret != 0) {
				LOG_WRN("Failed to stall the control endpoint (%d)",
					udc_nl_errno(ret));
			}
		}

		udc_nl_ctrl_idle(dev);
		udc_nl_submit_work(dev);

		return 0;
	}

	LOG_DBG("Set halt ep 0x%02x", cfg->addr);
	udc_nl_ep_abort(dev, cfg);
	udc_nl_halt_abort(dev, cfg);

	ret = udc_native_linux_halt_start(priv->host, *udc_nl_host_ep(dev, cfg->addr));
	if (ret != 0) {
		return udc_nl_errno(ret);
	}

	cfg->stat.halted = true;
	priv->halts_busy |= UDC_NL_HALT_BIT(cfg->addr);
	if (priv->ctrl == UDC_NL_CTRL_LOCAL) {
		priv->halt_retries |= UDC_NL_HALT_BIT(cfg->addr);
	} else {
		priv->halt_requests |= UDC_NL_HALT_BIT(cfg->addr);
	}

	udc_nl_submit_work(dev);

	return 0;
}

static int udc_nl_ep_clear_halt(const struct device *dev, struct udc_ep_config *const cfg)
{
	struct udc_nl_data *priv = udc_get_private(dev);
	int ret;

	if (USB_EP_GET_IDX(cfg->addr) == 0U) {
		return 0;
	}

	LOG_DBG("Clear halt ep 0x%02x", cfg->addr);
	priv->halt_requests &= ~UDC_NL_HALT_BIT(cfg->addr);
	priv->halt_retries &= ~UDC_NL_HALT_BIT(cfg->addr);

	/* The halt that a made up request is for is cleared by the USB host */
	if (priv->ctrl != UDC_NL_CTRL_LOCAL) {
		udc_nl_ep_abort(dev, cfg);
		udc_nl_halt_abort(dev, cfg);

		ret = udc_native_linux_ep_clear_halt(priv->host, *udc_nl_host_ep(dev, cfg->addr));
		if (ret != 0) {
			return udc_nl_errno(ret);
		}
	}

	udc_nl_submit_work(dev);

	return 0;
}

static int udc_nl_set_address(const struct device *dev, const uint8_t addr)
{
	struct udc_nl_data *priv = udc_get_private(dev);

	/* The stack is in the default state again, from where it takes an address */
	if (addr == 0U && priv->reset_pending) {
		priv->reset_pending = false;
		priv->addr_request = true;
		udc_nl_submit_work(dev);
	}

	return 0;
}

static int udc_nl_host_wakeup(const struct device *dev)
{
	ARG_UNUSED(dev);

	return -ENOTSUP;
}

static enum udc_bus_speed udc_nl_device_speed(const struct device *dev)
{
	struct udc_nl_data *priv = udc_get_private(dev);

	return priv->high_speed ? UDC_BUS_SPEED_HS : UDC_BUS_SPEED_FS;
}

static int udc_nl_open(const struct device *dev)
{
	const struct udc_nl_config *config = dev->config;
	struct udc_nl_data *priv = udc_get_private(dev);
	int err;

	priv->host = udc_native_linux_open(udc_nl_cmdline_udc != NULL ? udc_nl_cmdline_udc
								      : config->host_udc,
					   config->high_speed, CONFIG_UDC_NATIVE_LINUX_IRQ,
					   CONFIG_UDC_NATIVE_LINUX_POLL_PERIOD, &err);
	if (priv->host == NULL) {
		LOG_ERR("No host controller (%d)", udc_nl_errno(err));
		return udc_nl_errno(err);
	}

	return 0;
}

static int udc_nl_enable(const struct device *dev)
{
	const struct udc_nl_config *config = dev->config;
	struct udc_nl_data *priv = udc_get_private(dev);

	priv->ctrl = UDC_NL_CTRL_IDLE;
	priv->high_speed = config->high_speed;
	priv->bus_reset = false;
	priv->setup_pending = false;
	priv->setup_deferred = false;
	priv->reset_pending = false;
	priv->addr_request = false;
	priv->halt_requests = 0U;
	priv->halt_retries = 0U;
	priv->halts_busy = 0U;

	return udc_nl_open(dev);
}

static int udc_nl_disable(const struct device *dev)
{
	const struct udc_nl_config *config = dev->config;
	struct udc_nl_data *priv = udc_get_private(dev);

	for (uint8_t i = 0U; i < UDC_NL_NUM_EPS; i++) {
		udc_nl_ep_abort(dev, &config->ep_cfg_in[i]);
		udc_nl_ep_abort(dev, &config->ep_cfg_out[i]);

		if (i != 0U) {
			priv->host_ep_in[i] = -1;
			priv->host_ep_out[i] = -1;
		}
	}

	udc_native_linux_close(priv->host);
	priv->host = NULL;

	return 0;
}

static void udc_nl_ep_caps(struct udc_ep_config *const cfg, const struct udc_native_linux_ep *ep,
			   const uint16_t mps)
{
	cfg->caps.iso = ep->iso;
	cfg->caps.bulk = ep->bulk;
	cfg->caps.interrupt = ep->interrupt;
	cfg->caps.mps = MIN(ep->mps, mps);
}

static bool udc_nl_ep_is_free(const struct udc_ep_config *const cfg)
{
	return !cfg->caps.iso && !cfg->caps.bulk && !cfg->caps.interrupt;
}

/*
 * Give each host endpoint to one endpoint: the one of its number, or, when it
 * takes any number, the first one that has none. A host endpoint for both
 * directions serves one at a time, IN is preferred.
 */
static void udc_nl_eps_caps(const struct device *dev, const struct udc_native_linux_ep *eps,
			    const int num)
{
	const struct udc_nl_config *config = dev->config;
	const uint16_t mps = config->high_speed ? 1024U : 1023U;
	const struct udc_native_linux_ep *ep;
	struct udc_ep_config *cfg;

	for (uint8_t i = 1U; i < UDC_NL_NUM_EPS; i++) {
		config->ep_cfg_in[i].caps = (struct udc_ep_caps){.in = 1U};
		config->ep_cfg_out[i].caps = (struct udc_ep_caps){.out = 1U};
	}

	for (int pass = 0; pass < 2; pass++) {
		for (int e = 0; e < num; e++) {
			ep = &eps[e];

			if ((pass == 0) == (ep->num == UDC_NATIVE_LINUX_EP_NUM_ANY)) {
				continue;
			}

			for (uint8_t i = 1U; i < UDC_NL_NUM_EPS; i++) {
				if (pass == 0 && ep->num != i) {
					continue;
				}

				if (ep->in && udc_nl_ep_is_free(&config->ep_cfg_in[i])) {
					cfg = &config->ep_cfg_in[i];
				} else if (ep->out && udc_nl_ep_is_free(&config->ep_cfg_out[i])) {
					cfg = &config->ep_cfg_out[i];
				} else {
					continue;
				}

				udc_nl_ep_caps(cfg, ep, mps);
				break;
			}
		}
	}
}

static int udc_nl_init(const struct device *dev)
{
	struct udc_native_linux_ep eps[UDC_NATIVE_LINUX_EPS_MAX];
	struct udc_nl_data *priv = udc_get_private(dev);
	int ret;

	/*
	 * The endpoints of the host controller are known once it is bound, and
	 * the stack assigns them before it enables the device.
	 */
	ret = udc_nl_open(dev);
	if (ret != 0) {
		return ret;
	}

	ret = udc_native_linux_eps(priv->host, eps);
	udc_native_linux_close(priv->host);
	priv->host = NULL;
	if (ret < 0) {
		LOG_ERR("Failed to get the host endpoints (%d)", udc_nl_errno(ret));
		return udc_nl_errno(ret);
	}

	udc_nl_eps_caps(dev, eps, ret);

	if (udc_ep_enable_internal(dev, USB_CONTROL_EP_OUT, USB_EP_TYPE_CONTROL, 64, 0) != 0) {
		LOG_ERR("Failed to enable control endpoint");
		return -EIO;
	}

	if (udc_ep_enable_internal(dev, USB_CONTROL_EP_IN, USB_EP_TYPE_CONTROL, 64, 0) != 0) {
		LOG_ERR("Failed to enable control endpoint");
		return -EIO;
	}

	return 0;
}

static int udc_nl_shutdown(const struct device *dev)
{
	if (udc_ep_disable_internal(dev, USB_CONTROL_EP_OUT) != 0) {
		LOG_ERR("Failed to disable control endpoint");
		return -EIO;
	}

	if (udc_ep_disable_internal(dev, USB_CONTROL_EP_IN) != 0) {
		LOG_ERR("Failed to disable control endpoint");
		return -EIO;
	}

	return 0;
}

static void udc_nl_isr(const void *arg);

static int udc_nl_driver_preinit(const struct device *dev)
{
	const struct udc_nl_config *config = dev->config;
	struct udc_nl_data *priv = udc_get_private(dev);
	struct udc_data *data = dev->data;
	int err;

	priv->dev = dev;
	priv->high_speed = config->high_speed;
	k_work_init(&priv->work, udc_nl_work);

	data->caps.hs = config->high_speed;
	data->caps.mps0 = UDC_MPS0_64;
	/* The host controller sends the status of a request with a data stage */
	data->caps.out_ack = true;

	for (uint8_t i = 0U; i < UDC_NL_NUM_EPS; i++) {
		priv->host_ep_in[i] = -1;
		priv->host_ep_out[i] = -1;

		/* The other endpoints get their capabilities in udc_nl_init() */
		config->ep_cfg_out[i].caps.out = 1U;
		config->ep_cfg_in[i].caps.in = 1U;
		if (i == 0U) {
			config->ep_cfg_out[i].caps.control = 1U;
			config->ep_cfg_out[i].caps.mps = 64U;
			config->ep_cfg_in[i].caps.control = 1U;
			config->ep_cfg_in[i].caps.mps = 64U;
		}

		config->ep_cfg_out[i].addr = USB_EP_DIR_OUT | i;
		err = udc_register_ep(dev, &config->ep_cfg_out[i]);
		if (err != 0) {
			LOG_ERR("Failed to register endpoint");
			return err;
		}

		config->ep_cfg_in[i].addr = USB_EP_DIR_IN | i;
		err = udc_register_ep(dev, &config->ep_cfg_in[i]);
		if (err != 0) {
			LOG_ERR("Failed to register endpoint");
			return err;
		}
	}

	/* Shared by all instances */
	IRQ_CONNECT(CONFIG_UDC_NATIVE_LINUX_IRQ, UDC_NL_IRQ_PRIORITY, udc_nl_isr, NULL, 0);
	irq_enable(CONFIG_UDC_NATIVE_LINUX_IRQ);

	return 0;
}

static void udc_nl_lock(const struct device *dev)
{
	udc_lock_internal(dev, K_FOREVER);
}

static void udc_nl_unlock(const struct device *dev)
{
	udc_unlock_internal(dev);
}

static const struct udc_api udc_nl_api = {
	.lock = udc_nl_lock,
	.unlock = udc_nl_unlock,
	.device_speed = udc_nl_device_speed,
	.init = udc_nl_init,
	.enable = udc_nl_enable,
	.disable = udc_nl_disable,
	.shutdown = udc_nl_shutdown,
	.set_address = udc_nl_set_address,
	.host_wakeup = udc_nl_host_wakeup,
	.ep_enable = udc_nl_ep_enable,
	.ep_disable = udc_nl_ep_disable,
	.ep_set_halt = udc_nl_ep_set_halt,
	.ep_clear_halt = udc_nl_ep_clear_halt,
	.ep_enqueue = udc_nl_ep_enqueue,
	.ep_dequeue = udc_nl_ep_dequeue,
};

/* The node that the board has, and nodes that name their host controller */
#define UDC_NL_FOREACH_NODE(fn)                                                                    \
	DT_FOREACH_STATUS_OKAY(zephyr_native_posix_udc, fn)                                        \
	DT_FOREACH_STATUS_OKAY(zephyr_native_linux_udc, fn)

#define UDC_NL_NAME(node, name) _CONCAT(name, DT_DEP_ORD(node))

#define UDC_NL_DEVICE_DEFINE(node)                                                                 \
	static struct udc_ep_config UDC_NL_NAME(node, udc_nl_ep_cfg_out_)[UDC_NL_NUM_EPS];         \
	static struct udc_ep_config UDC_NL_NAME(node, udc_nl_ep_cfg_in_)[UDC_NL_NUM_EPS];          \
                                                                                                   \
	static const struct udc_nl_config UDC_NL_NAME(node, udc_nl_config_) = {                    \
		.ep_cfg_in = UDC_NL_NAME(node, udc_nl_ep_cfg_in_),                                 \
		.ep_cfg_out = UDC_NL_NAME(node, udc_nl_ep_cfg_out_),                               \
		.host_udc = DT_PROP_OR(node, host_udc, NULL),                                      \
		.high_speed = DT_ENUM_IDX_OR(node, maximum_speed, UDC_NL_SPEED_IDX_HS) >=          \
			      UDC_NL_SPEED_IDX_HS,                                                 \
	};                                                                                         \
                                                                                                   \
	static struct udc_nl_data UDC_NL_NAME(node, udc_nl_priv_);                                 \
                                                                                                   \
	static struct udc_data UDC_NL_NAME(node, udc_nl_data_) = {                                 \
		.mutex = Z_MUTEX_INITIALIZER(UDC_NL_NAME(node, udc_nl_data_).mutex),               \
		.priv = &UDC_NL_NAME(node, udc_nl_priv_),                                          \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_DEFINE(node, udc_nl_driver_preinit, NULL, &UDC_NL_NAME(node, udc_nl_data_),      \
			 &UDC_NL_NAME(node, udc_nl_config_), POST_KERNEL,                          \
			 CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &udc_nl_api);

UDC_NL_FOREACH_NODE(UDC_NL_DEVICE_DEFINE)

#define UDC_NL_SUBMIT_WORK(node) udc_nl_submit_work(DEVICE_DT_GET(node));

static void udc_nl_isr(const void *arg)
{
	ARG_UNUSED(arg);

	UDC_NL_FOREACH_NODE(UDC_NL_SUBMIT_WORK)
}

static void udc_nl_options(void)
{
	static struct args_struct_t options[] = {
		{
			.option = "udc",
			.name = "name",
			.type = 's',
			.dest = (void *)&udc_nl_cmdline_udc,
			.descript = "USB device controller of the host to use, by its name in "
				    "/sys/class/udc, in place of the one of the devicetree. "
				    "The first one found when neither is set.",
		},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(options);
}

NATIVE_TASK(udc_nl_options, PRE_BOOT_1, 10);
NATIVE_TASK(udc_native_linux_close_all, ON_EXIT, 99);
