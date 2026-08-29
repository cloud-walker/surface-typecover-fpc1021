#include "fpc_trace.h"
#include "fpc_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

/* Raw bytes shown on the stderr line before truncating; the JSONL keeps all of them. */
#define TRACE_STDERR_HEX_BYTES 16

static FILE *trace_fp;
static fpc_trace_level trace_level = FPC_TRACE_NORMAL;
static double trace_t0_ms;
static char trace_file_path[512];

static double monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

double fpc_trace_now_ms(void) { return monotonic_ms() - trace_t0_ms; }

const char *fpc_trace_path(void) { return trace_fp ? trace_file_path : NULL; }

/* Image-stream packets are the bulk of a trace by volume and the least
 * interesting per-packet, so they stay off stderr unless asked for. */
static int tag_is_stream(const char *tag) {
    return tag && strncmp(tag, "stream", 6) == 0;
}

static void hex_encode(const unsigned char *buf, int len, char *out, int out_sz) {
    static const char digits[] = "0123456789abcdef";
    int i, o = 0;
    for (i = 0; i < len && o + 2 < out_sz; i++) {
        out[o++] = digits[buf[i] >> 4];
        out[o++] = digits[buf[i] & 0x0f];
    }
    out[o] = '\0';
}

static void hex_encode_spaced(const unsigned char *buf, int len, int max_bytes,
                              char *out, int out_sz) {
    static const char digits[] = "0123456789abcdef";
    int i, o = 0;
    int shown = len < max_bytes ? len : max_bytes;
    for (i = 0; i < shown && o + 4 < out_sz; i++) {
        if (i) out[o++] = ' ';
        out[o++] = digits[buf[i] >> 4];
        out[o++] = digits[buf[i] & 0x0f];
    }
    if (shown < len && o + 4 < out_sz) { out[o++] = ' '; out[o++] = '.'; out[o++] = '.'; }
    out[o] = '\0';
}

static void json_write_escaped(FILE *f, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
}

int fpc_trace_open(const char *jsonl_path, fpc_trace_level level) {
    trace_level = level;
    trace_t0_ms = monotonic_ms();
    trace_fp = NULL;
    trace_file_path[0] = '\0';

    if (!jsonl_path) return 0;

    if (strcmp(jsonl_path, "-") == 0) {
        time_t now = time(NULL);
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        strftime(trace_file_path, sizeof(trace_file_path),
                 "fpc-trace-%Y%m%d-%H%M%S.jsonl", &tm_buf);
    } else {
        snprintf(trace_file_path, sizeof(trace_file_path), "%s", jsonl_path);
    }

    trace_fp = fopen(trace_file_path, "w");
    if (!trace_fp) {
        fprintf(stderr, "could not open trace file %s\n", trace_file_path);
        trace_file_path[0] = '\0';
        return -1;
    }
    /* Line-buffered: a wedge that ends in a kill -9 still leaves a usable trace. */
    setvbuf(trace_fp, NULL, _IOLBF, 0);
    return 0;
}

void fpc_trace_close(void) {
    if (trace_fp) {
        fclose(trace_fp);
        trace_fp = NULL;
    }
}

static void trace_transfer(const char *dir, const char *tag, int rc,
                           int requested, int transferred,
                           const unsigned char *buf,
                           double t_ms, double dur_ms) {
    int is_in = (dir[0] == 'i');
    int failed = (rc != 0);
    unsigned short status = 0, substatus = 0, word = 0, acked_opcode = 0;
    int have_reply_header = (is_in && !failed && transferred >= 4);
    int have_word = (is_in && !failed && transferred >= 6);
    /* Bytes 4-5 mean different things per command: the payload length on a
     * capture reply, the chip-ID word on a Get Chip ID reply. Label them by
     * the opcode the reply acknowledges rather than guessing from the size. */
    const char *word_name = "word";

    if (have_reply_header) {
        status       = (unsigned short)(buf[0] | (buf[1] << 8));
        substatus    = (unsigned short)(buf[2] | (buf[3] << 8));
        acked_opcode = FPC_STATUS_TO_OPCODE(status);
    }
    if (have_word) {
        word = (unsigned short)(buf[4] | (buf[5] << 8));
        if (acked_opcode == CMD_CAPTURE)          word_name = "length";
        else if (acked_opcode == CMD_GET_CHIP_ID) word_name = "chip_id";
    }

    if (trace_fp) {
        char hex[2 * FPC_MAX_PACKET + 8];
        hex_encode(buf, transferred > 0 ? transferred : 0, hex, sizeof(hex));

        fprintf(trace_fp, "{\"t\":%.3f,\"dur\":%.3f,\"dir\":\"%s\",\"tag\":\"", t_ms, dur_ms, dir);
        json_write_escaped(trace_fp, tag ? tag : "");
        fprintf(trace_fp, "\",\"rc\":%d", rc);
        if (failed) fprintf(trace_fp, ",\"rc_name\":\"%s\"", libusb_error_name(rc));
        fprintf(trace_fp, ",\"req\":%d,\"len\":%d,\"hex\":\"%s\"", requested, transferred, hex);
        if (have_reply_header) {
            fprintf(trace_fp, ",\"status\":%u,\"acks\":\"%s\",\"substatus\":%u",
                    status, fpc_opcode_name(acked_opcode), substatus);
        }
        if (have_word) fprintf(trace_fp, ",\"%s\":%u", word_name, word);
        fprintf(trace_fp, "}\n");
    }

    if (trace_level == FPC_TRACE_QUIET) return;
    if (trace_level == FPC_TRACE_NORMAL && tag_is_stream(tag) && !failed) return;

    char pretty[4 * TRACE_STDERR_HEX_BYTES + 8];
    if (failed) {
        snprintf(pretty, sizeof(pretty), "%s", libusb_error_name(rc));
    } else {
        hex_encode_spaced(buf, transferred, TRACE_STDERR_HEX_BYTES, pretty, sizeof(pretty));
    }

    /* Composed into one buffer and written once: stdout and stderr interleave
     * on the operator's terminal, and a line assembled by several fprintf
     * calls gets cut in half by any stdout write that lands between them. */
    char line[768];
    int n = snprintf(line, sizeof(line), "%10.3f %s %-13s %3dB  %-50s %8.2fms",
                     t_ms, is_in ? "<" : ">", tag ? tag : "", failed ? 0 : transferred,
                     pretty, dur_ms);

    if (have_reply_header && n > 0 && n < (int)sizeof(line)) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "  status=0x%04x(%s) sub=%u",
                      status, fpc_opcode_name(acked_opcode), substatus);
        if (have_word && n > 0 && n < (int)sizeof(line))
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %s=0x%04x", word_name, word);
    }
    if (n < 0) return;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
    line[n] = '\0';

    fprintf(stderr, "%s\n", line);
}

int fpc_trace_write_cmd(libusb_device_handle *h, const char *tag,
                        unsigned short opcode, unsigned int timeout_ms) {
    unsigned char cmd[2] = {(unsigned char)(opcode & 0xff), (unsigned char)(opcode >> 8)};
    int transferred = 0;
    double t = fpc_trace_now_ms();
    double start = monotonic_ms();

    int rc = libusb_bulk_transfer(h, FPC_EP_OUT, cmd, sizeof(cmd), &transferred, timeout_ms);

    trace_transfer("out", tag, rc, (int)sizeof(cmd), transferred, cmd,
                   t, monotonic_ms() - start);
    return rc;
}

int fpc_trace_read_reply(libusb_device_handle *h, const char *tag,
                         unsigned char *buf, int buflen, int *transferred,
                         unsigned int timeout_ms) {
    int got = 0;
    double t = fpc_trace_now_ms();
    double start = monotonic_ms();

    int rc = libusb_bulk_transfer(h, FPC_EP_IN, buf, buflen, &got, timeout_ms);

    trace_transfer("in", tag, rc, buflen, got, buf, t, monotonic_ms() - start);
    if (transferred) *transferred = got;
    return rc;
}

void fpc_trace_event(const char *kind, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    double t = fpc_trace_now_ms();

    if (trace_fp) {
        fprintf(trace_fp, "{\"t\":%.3f,\"ev\":\"", t);
        json_write_escaped(trace_fp, kind);
        fprintf(trace_fp, "\",\"msg\":\"");
        json_write_escaped(trace_fp, msg);
        fprintf(trace_fp, "\"}\n");
    }
    if (trace_level != FPC_TRACE_QUIET)
        fprintf(stderr, "%10.3f * %-13s %s\n", t, kind, msg);
}
