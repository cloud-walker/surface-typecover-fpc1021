#include "fpc_device.h"
#include "fpc_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FPC_WRITE_TIMEOUT_MS 2000
#define FPC_READ_TIMEOUT_MS 3000

/* PROTOCOL.md, "Timing": pauses the Windows driver observes around a capture. */
#define FPC_POST_RESET_US 15000
#define FPC_POST_CAPTURE_US 10000

int fpc_open(libusb_context **ctx, libusb_device_handle **handle) {
    *ctx = NULL;
    *handle = NULL;

    if (libusb_init(ctx) < 0) {
        fprintf(stderr, "libusb_init failed\n");
        return -1;
    }

    *handle = libusb_open_device_with_vid_pid(*ctx, FPC_VID, FPC_PID);
    if (!*handle) {
        fprintf(stderr, "could not open device %04x:%04x "
                        "(are you root? is the Type Cover attached?)\n", FPC_VID, FPC_PID);
        libusb_exit(*ctx);
        *ctx = NULL;
        return -1;
    }

    if (libusb_kernel_driver_active(*handle, FPC_IFACE) == 1)
        libusb_detach_kernel_driver(*handle, FPC_IFACE);

    if (libusb_claim_interface(*handle, FPC_IFACE) < 0) {
        fprintf(stderr, "could not claim interface %d\n", FPC_IFACE);
        libusb_close(*handle);
        libusb_exit(*ctx);
        *handle = NULL;
        *ctx = NULL;
        return -1;
    }
    return 0;
}

void fpc_close(libusb_context *ctx, libusb_device_handle *handle) {
    if (handle) {
        libusb_release_interface(handle, FPC_IFACE);
        libusb_close(handle);
    }
    if (ctx) libusb_exit(ctx);
}

int fpc_usb_reset(libusb_device_handle *handle) {
    int rc = libusb_reset_device(handle);
    fpc_trace_event("usb_reset", "libusb_reset_device -> %s",
                    rc == 0 ? "ok" : libusb_error_name(rc));
    if (rc != 0) return rc;

    rc = libusb_claim_interface(handle, FPC_IFACE);
    if (rc != 0)
        fpc_trace_event("usb_reset", "re-claim failed: %s", libusb_error_name(rc));
    return rc;
}

int fpc_get_chip_id(libusb_device_handle *handle, fpc_chip_info *out) {
    unsigned char resp[FPC_MAX_PACKET];
    int transferred = 0;

    if (fpc_trace_write_cmd(handle, "chip_id", CMD_GET_CHIP_ID, FPC_WRITE_TIMEOUT_MS) != 0)
        return -1;
    if (fpc_trace_read_reply(handle, "chip_id.reply", resp, sizeof(resp),
                             &transferred, FPC_READ_TIMEOUT_MS) != 0 || transferred < 6)
        return -1;

    return fpc_identify_chip((unsigned short)(resp[4] | (resp[5] << 8)), out);
}

fpc_cap_result fpc_try_capture(libusb_device_handle *handle,
                               unsigned int read_timeout_ms,
                               unsigned char **out_buf, int *out_len) {
    unsigned char pkt[FPC_MAX_PACKET];
    int transferred = 0;

    if (fpc_trace_write_cmd(handle, "reset", CMD_RESET, FPC_WRITE_TIMEOUT_MS) != 0)
        return FPC_CAP_ERROR;
    usleep(FPC_POST_RESET_US);
    /* The reset command's own reply (status 0x1008) must be consumed: reads
     * and writes are paired 1:1, so leaving it queued desynchronises everything
     * that follows. */
    fpc_trace_read_reply(handle, "reset.reply", pkt, sizeof(pkt),
                         &transferred, FPC_READ_TIMEOUT_MS);

    if (fpc_trace_write_cmd(handle, "capture", CMD_CAPTURE, FPC_WRITE_TIMEOUT_MS) != 0)
        return FPC_CAP_ERROR;
    usleep(FPC_POST_CAPTURE_US);

    int rc = fpc_trace_read_reply(handle, "capture.hdr", pkt, sizeof(pkt),
                                  &transferred, read_timeout_ms);
    if (rc == LIBUSB_ERROR_TIMEOUT) return FPC_CAP_TIMEOUT;
    if (rc != 0 || transferred < 4) return FPC_CAP_ERROR;

    unsigned short status    = (unsigned short)(pkt[0] | (pkt[1] << 8));
    unsigned short substatus = (unsigned short)(pkt[2] | (pkt[3] << 8));
    if (status != FPC_OPCODE_TO_STATUS(CMD_CAPTURE) || substatus == FPC_SUBSTATUS_NOT_READY)
        return FPC_CAP_NOT_READY;
    if (substatus != FPC_SUBSTATUS_OK) return FPC_CAP_ERROR;
    if (transferred < 6) return FPC_CAP_ERROR;

    int length = pkt[4] | (pkt[5] << 8);
    unsigned char *buf = malloc((size_t)length);
    if (!buf) return FPC_CAP_ERROR;

    int have = transferred - 6;
    if (have > length) have = length;
    if (have > 0) memcpy(buf, pkt + 6, (size_t)have);

    while (have < length) {
        int remaining = length - have;
        int reqlen = remaining + 2;
        if (reqlen > FPC_MAX_PACKET) reqlen = FPC_MAX_PACKET;
        if (reqlen < 3) break;

        if (fpc_trace_read_reply(handle, "stream", pkt, reqlen,
                                 &transferred, FPC_READ_TIMEOUT_MS) != 0 || transferred <= 0) {
            fpc_trace_event("stream_fail", "stream read failed at %d/%d bytes", have, length);
            free(buf);
            return FPC_CAP_ERROR;
        }
        /* First 2 bytes of every continuation packet are a marker, not payload. */
        int usable = transferred - 2;
        if (usable <= 0) continue;
        if (usable > length - have) usable = length - have;
        memcpy(buf + have, pkt + 2, (size_t)usable);
        have += usable;
    }

    if (have != length) {
        fpc_trace_event("stream_fail", "incomplete capture: %d of %d bytes", have, length);
        free(buf);
        return FPC_CAP_ERROR;
    }

    *out_buf = buf;
    *out_len = length;
    return FPC_CAP_OK;
}
