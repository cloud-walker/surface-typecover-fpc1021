/*
 * USB transfer tracing for the FPC1021 diagnostic tools.
 *
 * Every bulk transfer goes through fpc_trace_write_cmd/fpc_trace_read_reply
 * instead of libusb_bulk_transfer directly, so that each one is recorded
 * with its timing, decoded status and raw bytes.
 *
 * Two sinks, from one source:
 *   - stderr: one human-readable line per transfer, for watching live.
 *   - JSONL file: one object per transfer, for diffing two runs against
 *     each other (the point of the whole thing: comparing the captures
 *     that work against the one where the sensor wedges).
 *
 * The `tag` argument names the *intent* of a transfer ("reset",
 * "capture.hdr", "stream") -- something the trace layer cannot infer from
 * the bytes, and the thing that makes a trace greppable.
 */

#ifndef FPC_TRACE_H
#define FPC_TRACE_H

#include <libusb-1.0/libusb.h>

typedef enum {
    FPC_TRACE_QUIET  = 0, /* JSONL only, nothing on stderr */
    FPC_TRACE_NORMAL = 1, /* one line per transfer; image-stream packets only if they fail */
    FPC_TRACE_FULL   = 2  /* one line per transfer, image-stream packets included */
} fpc_trace_level;

/*
 * Starts a trace. jsonl_path may be NULL for no file sink; pass "-" to get
 * an auto-named fpc-trace-<timestamp>.jsonl in the current directory.
 * Returns 0 on success (a file that cannot be opened is an error).
 */
int fpc_trace_open(const char *jsonl_path, fpc_trace_level level);
void fpc_trace_close(void);

/* Path of the JSONL sink, or NULL if there isn't one. */
const char *fpc_trace_path(void);

/* Milliseconds since fpc_trace_open(). */
double fpc_trace_now_ms(void);

/* Traced bulk transfers. Return libusb result codes, like the calls they wrap. */
int fpc_trace_write_cmd(libusb_device_handle *h, const char *tag,
                        unsigned short opcode, unsigned int timeout_ms);
int fpc_trace_read_reply(libusb_device_handle *h, const char *tag,
                         unsigned char *buf, int buflen, int *transferred,
                         unsigned int timeout_ms);

/*
 * Records a non-transfer event on the same timeline: a note typed by the
 * operator ("finger down"), a detected wedge, a USB reset. `kind` is a
 * short machine-facing category; the formatted message is for humans.
 */
void fpc_trace_event(const char *kind, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif /* FPC_TRACE_H */
