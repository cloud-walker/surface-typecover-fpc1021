/*
 * Device-level operations for the FPC1021 sensor: open/claim, chip
 * identification, and one capture attempt. Every USB transfer underneath
 * goes through the trace layer.
 *
 * Shared by the capture tool and the diagnostic probe so that both drive
 * the sensor through exactly the same sequence -- a divergence between the
 * two would make their traces incomparable, which is the one thing the
 * tracing is for.
 */

#ifndef FPC_DEVICE_H
#define FPC_DEVICE_H

#include <libusb-1.0/libusb.h>
#include "fpc_proto.h"

typedef enum {
    FPC_CAP_OK        =  0,
    FPC_CAP_NOT_READY =  1, /* sensor answered "not ready" -- no finger yet, retry */
    FPC_CAP_TIMEOUT   =  2, /* read timed out; normal while waiting, the wedge signature in bulk */
    FPC_CAP_ERROR     = -1
} fpc_cap_result;

/* Opens the sensor interface, detaching the kernel driver if needed. */
int  fpc_open(libusb_context **ctx, libusb_device_handle **handle);
void fpc_close(libusb_context *ctx, libusb_device_handle *handle);

/* A real USB bus reset, then re-claim: the only known way out of a wedge. */
int fpc_usb_reset(libusb_device_handle *handle);

int fpc_get_chip_id(libusb_device_handle *handle, fpc_chip_info *out);

/*
 * One capture attempt: reset, trigger, stream the image back. On FPC_CAP_OK
 * *out_buf holds *out_len bytes and belongs to the caller.
 * read_timeout_ms bounds the wait for the capture header, i.e. how long to
 * hold still waiting for a finger.
 */
fpc_cap_result fpc_try_capture(libusb_device_handle *handle,
                               unsigned int read_timeout_ms,
                               unsigned char **out_buf, int *out_len);

#endif /* FPC_DEVICE_H */
