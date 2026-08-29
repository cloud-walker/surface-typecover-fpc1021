/*
 * Standalone proof-of-concept capture tool for the FPC1021 fingerprint
 * sensor found in the Microsoft Surface Type Cover with Fingerprint ID.
 *
 * See ../PROTOCOL.md for the full protocol writeup this implements, and
 * fpc_probe.c for the interactive diagnostic tool built on the same layers.
 *
 * Usage: sudo ./fpc_capture [-v] [-t trace.jsonl] [output.bin]
 * Place a finger on the sensor; it will retry until a frame is captured
 * (or it gives up) and write the raw WxH 8bpp grayscale image to the
 * given path (default: capture.bin).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fpc_device.h"
#include "fpc_trace.h"

#define MAX_CAPTURE_ATTEMPTS 15
#define ATTEMPT_RETRY_DELAY_US 200000
#define CAPTURE_READ_TIMEOUT_MS 3000

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [-v] [-t trace.jsonl] [output.bin]\n"
        "  -v            trace every USB transfer to stderr\n"
        "  -t PATH       also write a JSONL trace ('-' auto-names by timestamp)\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *out_path = "capture.bin";
    const char *trace_path = NULL;
    fpc_trace_level level = FPC_TRACE_QUIET;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            level = FPC_TRACE_NORMAL;
        } else if (strcmp(argv[i], "-vv") == 0) {
            level = FPC_TRACE_FULL;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            trace_path = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            usage(argv[0]);
            return 1;
        } else {
            out_path = argv[i];
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (fpc_trace_open(trace_path, level) != 0) return 1;

    libusb_context *ctx = NULL;
    libusb_device_handle *h = NULL;
    int status = 1;

    if (fpc_open(&ctx, &h) != 0) {
        fpc_trace_close();
        return 1;
    }

    fpc_chip_info chip;
    if (fpc_get_chip_id(h, &chip) != 0) {
        fprintf(stderr, "could not identify sensor chip\n");
        goto cleanup;
    }
    printf("sensor: %s (%dx%d)\n", chip.name, chip.width, chip.height);
    printf("place a finger on the sensor...\n");
    fflush(stdout);

    unsigned char *image = NULL;
    int image_len = 0;
    int attempt;
    for (attempt = 0; attempt < MAX_CAPTURE_ATTEMPTS; attempt++) {
        fpc_cap_result rc = fpc_try_capture(h, CAPTURE_READ_TIMEOUT_MS, &image, &image_len);
        if (rc == FPC_CAP_OK) break;
        if (rc == FPC_CAP_ERROR) {
            fprintf(stderr, "capture error, aborting\n");
            goto cleanup;
        }
        usleep(ATTEMPT_RETRY_DELAY_US);
    }

    if (!image) {
        fprintf(stderr, "gave up after %d attempts (no finger detected?)\n", MAX_CAPTURE_ATTEMPTS);
        goto cleanup;
    }

    FILE *f = fopen(out_path, "wb");
    if (f) {
        fwrite(image, 1, (size_t)image_len, f);
        fclose(f);
        printf("captured %dx%d image -> %s\n", chip.width, chip.height, out_path);
        status = 0;
    } else {
        fprintf(stderr, "could not write %s\n", out_path);
    }
    free(image);

cleanup:
    fpc_close(ctx, h);
    if (fpc_trace_path()) fprintf(stderr, "trace written to %s\n", fpc_trace_path());
    fpc_trace_close();
    return status;
}
