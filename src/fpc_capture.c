/*
 * Standalone proof-of-concept capture tool for the FPC1021 fingerprint
 * sensor found in the Microsoft Surface Type Cover with Fingerprint ID.
 *
 * See ../PROTOCOL.md for the full protocol writeup this implements.
 *
 * Usage: sudo ./fpc_capture [output.bin]
 * Place a finger on the sensor; it will retry until a frame is captured
 * (or it gives up) and write the raw WxH 8bpp grayscale image to the
 * given path (default: capture.bin).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define FPC_VID 0x045e
#define FPC_PID 0x09c2
#define FPC_IFACE 1
#define EP_OUT 0x04
#define EP_IN 0x83

#define CMD_GET_CHIP_ID 0x0001
#define CMD_CAPTURE 0x0007
#define CMD_RESET 0x0008

#define MAX_CAPTURE_ATTEMPTS 15
#define ATTEMPT_RETRY_DELAY_US 200000

typedef struct {
    unsigned short width;
    unsigned short height;
    const char *name;
} fpc_chip_info;

static int fpc_write_cmd(libusb_device_handle *h, unsigned short opcode) {
    unsigned char cmd[2] = {(unsigned char)(opcode & 0xff), (unsigned char)(opcode >> 8)};
    int transferred = 0;
    return libusb_bulk_transfer(h, EP_OUT, cmd, sizeof(cmd), &transferred, 2000);
}

static int fpc_read_reply(libusb_device_handle *h, unsigned char *buf, int buflen, int *transferred) {
    return libusb_bulk_transfer(h, EP_IN, buf, buflen, transferred, 3000);
}

/* Looks up chip identity/resolution from the masked Get-Chip-ID word. See PROTOCOL.md. */
static int fpc_identify_chip(unsigned short chip_id, fpc_chip_info *out) {
    unsigned short masked = chip_id & 0xfff0;
    if (masked == 0x0200) { *out = (fpc_chip_info){192, 192, "FPC1020"}; return 0; }
    if (masked == 0x0210) { *out = (fpc_chip_info){160, 160, "FPC1021"}; return 0; }
    if (masked == 0x1400) { *out = (fpc_chip_info){192, 56, "FPC1140"}; return 0; }
    if (masked == 0x1500) { *out = (fpc_chip_info){208, 80, "FPC1150"}; return 0; }
    if ((chip_id & 0xff0f) == 0x0101) { *out = (fpc_chip_info){88, 112, "FPC1022"}; return 0; }
    return -1;
}

static int fpc_get_chip_id(libusb_device_handle *h, fpc_chip_info *out) {
    unsigned char resp[64];
    int transferred = 0;

    if (fpc_write_cmd(h, CMD_GET_CHIP_ID) != 0) return -1;
    if (fpc_read_reply(h, resp, sizeof(resp), &transferred) != 0 || transferred < 6) return -1;

    unsigned short chip_id = resp[4] | (resp[5] << 8);
    return fpc_identify_chip(chip_id, out);
}

/*
 * Attempts one capture. On success, fills *out_buf (caller must free) with
 * `length` bytes and returns 0. Returns nonzero if the sensor reports
 * "not ready" (e.g. no finger) so the caller can retry.
 */
static int fpc_try_capture(libusb_device_handle *h, unsigned char **out_buf, int *out_len) {
    unsigned char pkt[64];
    int transferred;

    if (fpc_write_cmd(h, CMD_RESET) != 0) return -1;
    usleep(15000);
    /* consume the reset command's own reply (status 0x1008) before continuing */
    fpc_read_reply(h, pkt, sizeof(pkt), &transferred);

    if (fpc_write_cmd(h, CMD_CAPTURE) != 0) return -1;
    usleep(10000);

    if (fpc_read_reply(h, pkt, sizeof(pkt), &transferred) != 0 || transferred < 4) return -1;

    unsigned short status = pkt[0] | (pkt[1] << 8);
    unsigned short substatus = pkt[2] | (pkt[3] << 8);
    if (status != 0x1007 || substatus == 5) return 1; /* not ready, retryable */
    if (substatus != 0) return -1;                     /* hard reject */
    if (transferred < 6) return -1;

    int length = pkt[4] | (pkt[5] << 8);
    unsigned char *buf = malloc(length);
    if (!buf) return -1;

    int have = transferred - 6;
    if (have > length) have = length;
    if (have > 0) memcpy(buf, pkt + 6, have);

    while (have < length) {
        int remaining = length - have;
        int reqlen = remaining + 2;
        if (reqlen > 64) reqlen = 64;
        if (reqlen < 3) break;

        if (fpc_read_reply(h, pkt, reqlen, &transferred) != 0 || transferred <= 0) {
            fprintf(stderr, "stream read failed at %d/%d bytes\n", have, length);
            free(buf);
            return -1;
        }
        int usable = transferred - 2; /* first 2 bytes of every continuation packet are a marker, not payload */
        if (usable <= 0) continue;
        if (usable > length - have) usable = length - have;
        memcpy(buf + have, pkt + 2, usable);
        have += usable;
    }

    if (have != length) {
        fprintf(stderr, "incomplete capture: got %d of %d bytes\n", have, length);
        free(buf);
        return -1;
    }

    *out_buf = buf;
    *out_len = length;
    return 0;
}

int main(int argc, char **argv) {
    const char *out_path = (argc > 1) ? argv[1] : "capture.bin";

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) < 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }

    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, FPC_VID, FPC_PID);
    if (!h) { fprintf(stderr, "could not open device %04x:%04x (are you root? is the Type Cover attached?)\n", FPC_VID, FPC_PID); return 1; }

    if (libusb_kernel_driver_active(h, FPC_IFACE) == 1) libusb_detach_kernel_driver(h, FPC_IFACE);
    if (libusb_claim_interface(h, FPC_IFACE) < 0) { fprintf(stderr, "could not claim interface %d\n", FPC_IFACE); return 1; }

    fpc_chip_info chip;
    if (fpc_get_chip_id(h, &chip) != 0) {
        fprintf(stderr, "could not identify sensor chip\n");
        goto cleanup;
    }
    printf("sensor: %s (%dx%d)\n", chip.name, chip.width, chip.height);
    printf("place a finger on the sensor...\n");

    unsigned char *image = NULL;
    int image_len = 0;
    int attempt;
    for (attempt = 0; attempt < MAX_CAPTURE_ATTEMPTS; attempt++) {
        int rc = fpc_try_capture(h, &image, &image_len);
        if (rc == 0) break;
        if (rc < 0) { fprintf(stderr, "capture error, aborting\n"); goto cleanup; }
        usleep(ATTEMPT_RETRY_DELAY_US);
    }

    if (!image) {
        fprintf(stderr, "gave up after %d attempts (no finger detected?)\n", MAX_CAPTURE_ATTEMPTS);
        goto cleanup;
    }

    FILE *f = fopen(out_path, "wb");
    if (f) {
        fwrite(image, 1, image_len, f);
        fclose(f);
        printf("captured %dx%d image -> %s\n", chip.width, chip.height, out_path);
    } else {
        fprintf(stderr, "could not write %s\n", out_path);
    }
    free(image);

cleanup:
    libusb_release_interface(h, FPC_IFACE);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
