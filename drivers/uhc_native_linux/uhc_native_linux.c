/*
 * SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zephyr_native_linux_uhc

#include "uhc_common.h"
#include "uhc_native_linux_bottom.h"

#include <cmdline.h>
#include <nsi_errno.h>
#include <posix_native_task.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usb_ch9.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(uhc_native_linux, CONFIG_UHC_DRIVER_LOG_LEVEL);

#define UHC_NL_CONTROL_TIMEOUT_MS 1000U
/* Finished bulk and interrupt transfers are looked for at this period */
#define UHC_NL_POLL_PERIOD        K_MSEC(1)

/* Values of the stage field of a transfer */
#define UHC_NL_STAGE_QUEUED    0U
#define UHC_NL_STAGE_SUBMITTED 1U

struct uhc_nl_config {
	const char *host_device;
};

struct uhc_nl_data {
	const struct device *dev;
	struct uhc_native_linux_host *host;
	struct k_work_delayable work;
};

static char *uhc_nl_cmdline_device;

static int uhc_nl_errno(int ret)
{
	return ret < 0 ? -nsi_errno_from_mid(-ret) : ret;
}

static int uhc_nl_control(struct uhc_nl_data *priv, struct uhc_transfer *const xfer)
{
	const struct usb_setup_packet *setup = (const void *)xfer->setup_pkt;
	bool to_host = usb_reqtype_is_to_host(setup);
	uint16_t length = sys_le16_to_cpu(setup->wLength);
	struct net_buf *buf = xfer->buf;
	uint8_t *data = NULL;
	int ret;

	if (setup->bmRequestType == 0U && setup->bRequest == USB_SREQ_SET_ADDRESS) {
		/* The device keeps the address that Linux gave it */
		return 0;
	}

	if (setup->bmRequestType == 0U && setup->bRequest == USB_SREQ_SET_CONFIGURATION) {
		return uhc_nl_errno(uhc_native_linux_set_config(priv->host,
								sys_le16_to_cpu(setup->wValue)));
	}

	if (length > 0U) {
		if (buf == NULL || length > (to_host ? net_buf_tailroom(buf) : buf->len)) {
			return -ENOMEM;
		}

		data = to_host ? net_buf_tail(buf) : buf->data;
	}

	ret = uhc_nl_errno(uhc_native_linux_control(priv->host, xfer->setup_pkt, data,
						    UHC_NL_CONTROL_TIMEOUT_MS));
	if (ret < 0) {
		return ret;
	}

	if (to_host && ret > 0) {
		net_buf_add(buf, ret);
	}

	return 0;
}

static int uhc_nl_submit(struct uhc_nl_data *priv, struct uhc_transfer *const xfer)
{
	struct net_buf *buf = xfer->buf;
	uint8_t *data = NULL;
	size_t len = 0;

	if (xfer->type != USB_EP_TYPE_BULK && xfer->type != USB_EP_TYPE_INTERRUPT) {
		return -ENOTSUP;
	}

	if (buf != NULL) {
		data = USB_EP_DIR_IS_IN(xfer->ep) ? net_buf_tail(buf) : buf->data;
		len = USB_EP_DIR_IS_IN(xfer->ep) ? net_buf_tailroom(buf) : buf->len;
	}

	return uhc_nl_errno(uhc_native_linux_submit(priv->host, xfer, xfer->ep,
						    xfer->type == USB_EP_TYPE_INTERRUPT, data,
						    len));
}

/* The device is gone: end its transfers and tell the stack */
static void uhc_nl_removed(const struct device *dev)
{
	struct uhc_nl_data *priv = uhc_get_private(dev);
	struct uhc_data *data = dev->data;
	struct uhc_transfer *xfer;
	struct uhc_transfer *tmp;

	uhc_native_linux_close(priv->host);
	priv->host = NULL;

	SYS_DLIST_FOR_EACH_CONTAINER_SAFE(&data->ctrl_xfers, xfer, tmp, node) {
		uhc_xfer_return(dev, xfer, -ENODEV);
	}

	uhc_submit_event(dev, UHC_EVT_DEV_REMOVED, 0);
}

static int uhc_nl_reap(const struct device *dev)
{
	struct uhc_nl_data *priv = uhc_get_private(dev);
	struct uhc_transfer *xfer;
	unsigned int len;
	void *ctx;
	int status;
	int ret;

	while ((ret = uhc_native_linux_reap(priv->host, &ctx, &status, &len)) > 0) {
		xfer = ctx;
		status = uhc_nl_errno(status);

		if (USB_EP_DIR_IS_IN(xfer->ep) && xfer->buf != NULL) {
			net_buf_add(xfer->buf, len);
		}

		/* This is how a cancelled transfer ends */
		uhc_xfer_return(dev, xfer, status == -ENOENT ? -ECONNRESET : status);
	}

	return uhc_nl_errno(ret);
}

static void uhc_nl_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct uhc_nl_data *priv = CONTAINER_OF(dwork, struct uhc_nl_data, work);
	const struct device *dev = priv->dev;
	struct uhc_data *data = dev->data;
	struct uhc_transfer *xfer;
	struct uhc_transfer *tmp;
	bool submitted = false;
	int ret;

	uhc_lock_internal(dev, K_FOREVER);

	if (priv->host == NULL) {
		goto unlock;
	}

	ret = uhc_nl_reap(dev);
	if (ret < 0) {
		LOG_WRN("Lost the device (%d)", ret);
		uhc_nl_removed(dev);
		goto unlock;
	}

	SYS_DLIST_FOR_EACH_CONTAINER_SAFE(&data->ctrl_xfers, xfer, tmp, node) {
		if (xfer->stage == UHC_NL_STAGE_SUBMITTED) {
			submitted = true;
			continue;
		}

		if (USB_EP_GET_IDX(xfer->ep) == 0U) {
			uhc_xfer_return(dev, xfer, uhc_nl_control(priv, xfer));
			continue;
		}

		ret = uhc_nl_submit(priv, xfer);
		if (ret != 0) {
			uhc_xfer_return(dev, xfer, ret);
			continue;
		}

		xfer->stage = UHC_NL_STAGE_SUBMITTED;
		submitted = true;
	}

	if (submitted) {
		k_work_reschedule(&priv->work, UHC_NL_POLL_PERIOD);
	}

unlock:
	uhc_unlock_internal(dev);
}

static int uhc_nl_enqueue(const struct device *dev, struct uhc_transfer *const xfer)
{
	struct uhc_nl_data *priv = uhc_get_private(dev);

	if (priv->host == NULL) {
		return -ENODEV;
	}

	xfer->stage = UHC_NL_STAGE_QUEUED;
	uhc_xfer_append(dev, xfer);
	k_work_reschedule(&priv->work, K_NO_WAIT);

	return 0;
}

static int uhc_nl_dequeue(const struct device *dev, struct uhc_transfer *const xfer)
{
	struct uhc_nl_data *priv = uhc_get_private(dev);

	if (xfer->queued == 0U) {
		return 0;
	}

	if (xfer->stage == UHC_NL_STAGE_SUBMITTED && priv->host != NULL) {
		/* The transfer is returned once usbfs reports it as cancelled */
		return uhc_nl_errno(uhc_native_linux_discard(priv->host, xfer));
	}

	uhc_xfer_return(dev, xfer, -ECONNRESET);

	return 0;
}

static int uhc_nl_bus_reset(const struct device *dev)
{
	/* The device stays as Linux has set it up */
	return uhc_submit_event(dev, UHC_EVT_RESETED, 0);
}

static int uhc_nl_sof_enable(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static int uhc_nl_bus_suspend(const struct device *dev)
{
	return uhc_submit_event(dev, UHC_EVT_SUSPENDED, 0);
}

static int uhc_nl_bus_resume(const struct device *dev)
{
	return uhc_submit_event(dev, UHC_EVT_RESUMED, 0);
}

static int uhc_nl_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static int uhc_nl_enable(const struct device *dev)
{
	const struct uhc_nl_config *config = dev->config;
	struct uhc_nl_data *priv = uhc_get_private(dev);
	const char *id = uhc_nl_cmdline_device != NULL ? uhc_nl_cmdline_device
						       : config->host_device;
	enum uhc_native_linux_speed speed;
	int err;

	priv->host = uhc_native_linux_open(id, &speed, &err);
	if (priv->host == NULL) {
		LOG_ERR("Failed to open the USB device %s (%d)", id != NULL ? id : "(none)",
			uhc_nl_errno(err));
		return uhc_nl_errno(err);
	}

	switch (speed) {
	case UHC_NATIVE_LINUX_SPEED_LOW:
		return uhc_submit_event(dev, UHC_EVT_DEV_CONNECTED_LS, 0);
	case UHC_NATIVE_LINUX_SPEED_FULL:
		return uhc_submit_event(dev, UHC_EVT_DEV_CONNECTED_FS, 0);
	default:
		return uhc_submit_event(dev, UHC_EVT_DEV_CONNECTED_HS, 0);
	}
}

static int uhc_nl_disable(const struct device *dev)
{
	struct uhc_nl_data *priv = uhc_get_private(dev);
	struct uhc_data *data = dev->data;
	struct uhc_transfer *xfer;
	struct uhc_transfer *tmp;

	if (priv->host == NULL) {
		return 0;
	}

	uhc_native_linux_close(priv->host);
	priv->host = NULL;

	SYS_DLIST_FOR_EACH_CONTAINER_SAFE(&data->ctrl_xfers, xfer, tmp, node) {
		uhc_xfer_return(dev, xfer, -ECONNRESET);
	}

	return 0;
}

static int uhc_nl_shutdown(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static int uhc_nl_lock(const struct device *dev)
{
	return uhc_lock_internal(dev, K_FOREVER);
}

static int uhc_nl_unlock(const struct device *dev)
{
	return uhc_unlock_internal(dev);
}

static int uhc_nl_driver_preinit(const struct device *dev)
{
	struct uhc_nl_data *priv = uhc_get_private(dev);
	struct uhc_data *data = dev->data;

	priv->dev = dev;
	k_mutex_init(&data->mutex);
	k_work_init_delayable(&priv->work, uhc_nl_work_handler);

	return 0;
}

static DEVICE_API(uhc, uhc_nl_api) = {
	.lock = uhc_nl_lock,
	.unlock = uhc_nl_unlock,
	.init = uhc_nl_init,
	.enable = uhc_nl_enable,
	.disable = uhc_nl_disable,
	.shutdown = uhc_nl_shutdown,

	.bus_reset = uhc_nl_bus_reset,
	.sof_enable = uhc_nl_sof_enable,
	.bus_suspend = uhc_nl_bus_suspend,
	.bus_resume = uhc_nl_bus_resume,

	.ep_enqueue = uhc_nl_enqueue,
	.ep_dequeue = uhc_nl_dequeue,
};

#define UHC_NL_DEVICE_DEFINE(n)                                                                    \
	static const struct uhc_nl_config uhc_nl_config_##n = {                                    \
		.host_device = DT_INST_PROP_OR(n, host_device, NULL),                              \
	};                                                                                         \
                                                                                                   \
	static struct uhc_nl_data uhc_nl_priv_##n;                                                 \
                                                                                                   \
	static struct uhc_data uhc_nl_data_##n = {                                                 \
		.priv = &uhc_nl_priv_##n,                                                          \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, uhc_nl_driver_preinit, NULL, &uhc_nl_data_##n,                    \
			      &uhc_nl_config_##n, POST_KERNEL,                                     \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &uhc_nl_api);

DT_INST_FOREACH_STATUS_OKAY(UHC_NL_DEVICE_DEFINE)

static void uhc_nl_options(void)
{
	static struct args_struct_t options[] = {
		{
			.option = "uhc-device",
			.name = "id",
			.type = 's',
			.dest = (void *)&uhc_nl_cmdline_device,
			.descript = "USB device of the host to use, as vendor:product in "
				    "hexadecimal or as its usbfs node, in place of the one of "
				    "the devicetree.",
		},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(options);
}

NATIVE_TASK(uhc_nl_options, PRE_BOOT_1, 10);
NATIVE_TASK(uhc_native_linux_close_all, ON_EXIT, 99);
