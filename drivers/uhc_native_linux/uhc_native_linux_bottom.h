/*
 * SPDX-FileCopyrightText: 2026 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * usbfs access for the native Linux USB host controller. It is built in the
 * runner context, with the headers of the host, and takes and returns plain
 * C types only. Errors are negative errno values of the embedded C library.
 */

#ifndef UHC_NATIVE_LINUX_BOTTOM_H_
#define UHC_NATIVE_LINUX_BOTTOM_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum uhc_native_linux_speed {
	UHC_NATIVE_LINUX_SPEED_LOW,
	UHC_NATIVE_LINUX_SPEED_FULL,
	UHC_NATIVE_LINUX_SPEED_HIGH,
};

struct uhc_native_linux_host;

/**
 * Open a USB device of the host.
 *
 * @param id "vendor:product" in hexadecimal, or the usbfs node of the device
 * @param speed Speed of the device
 * @param err Negative errno value when NULL is returned
 */
struct uhc_native_linux_host *uhc_native_linux_open(const char *id,
						    enum uhc_native_linux_speed *speed, int *err);

/** Give the interfaces back to their kernel drivers and close the device. */
void uhc_native_linux_close(struct uhc_native_linux_host *host);

/** Close the devices that are still open. */
void uhc_native_linux_close_all(void);

/**
 * Do a control transfer and wait for it.
 *
 * @param setup Setup packet, its wLength gives the size of data
 * @return Number of data bytes transferred, or a negative errno value
 */
int uhc_native_linux_control(struct uhc_native_linux_host *host, const uint8_t *setup,
			     uint8_t *data, unsigned int timeout_ms);

/** Select a configuration. Nothing is done for the one that is active. */
int uhc_native_linux_set_config(struct uhc_native_linux_host *host, uint8_t config);

/** Start a bulk or interrupt transfer, reported by uhc_native_linux_reap() with ctx. */
int uhc_native_linux_submit(struct uhc_native_linux_host *host, void *ctx, uint8_t ep,
			    bool interrupt, uint8_t *data, unsigned int len);

/** Cancel a transfer. It is still reported by uhc_native_linux_reap(). */
int uhc_native_linux_discard(struct uhc_native_linux_host *host, void *ctx);

/**
 * Get a finished transfer.
 *
 * @param status 0 or the negative errno value the transfer ended with
 * @param len Number of bytes transferred
 * @return 1 for a transfer, 0 when none has finished, or a negative errno value
 */
int uhc_native_linux_reap(struct uhc_native_linux_host *host, void **ctx, int *status,
			  unsigned int *len);

#ifdef __cplusplus
}
#endif

#endif /* UHC_NATIVE_LINUX_BOTTOM_H_ */
