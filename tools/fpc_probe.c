/*
 * Interactive diagnostic probe for the FPC1021 fingerprint sensor.
 *
 * A small REPL over the wire protocol: send an arbitrary opcode, read a
 * reply, run captures back to back, annotate the timeline, reset the bus.
 * Every transfer is traced (see ../src/fpc_trace.h), so a session produces
 * a JSONL file that can be diffed against another session's.
 *
 * It exists for one question in particular: after 2-3 captures the sensor
 * stops answering reads until a full USB reset (the "wedge" -- see
 * ../libfprint-driver/README.md). `loop N` reproduces it in seconds
 * instead of the minutes a libfprint rebuild + fprintd-enroll cycle takes,
 * and once wedged, `cmd <opcode>` asks whether *anything* still answers.
 *
 * Usage: sudo ./fpc_probe [-t trace.jsonl | -n] [-q|-vv] [command ...]
 * Reads commands from stdin, or runs a single command given as arguments:
 *   sudo ./fpc_probe loop 10
 *   echo "loop 10" | sudo ./fpc_probe
 *
 * A caution on hunting opcodes: probe single opcodes you have a hypothesis
 * about, rather than sweeping the space. FPC chips carry calibration and
 * OTP registers, and this sensor is soldered inside a Type Cover -- an
 * unknown opcode could write persistent state.
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fpc_device.h"
#include "fpc_trace.h"

#define PROBE_LINE_MAX 512
#define PROBE_MAX_ARGS 8
#define DEFAULT_READ_TIMEOUT_MS 3000
#define DEFAULT_LOOP_DELAY_MS 200

/* Consecutive capture timeouts taken to mean the sensor has wedged. Matches
 * FPC_MAX_CONSECUTIVE_TIMEOUTS in the libfprint driver. */
#define WEDGE_TIMEOUT_THRESHOLD 6

static libusb_device_handle *dev;

static void print_help(void) {
    puts(
        "commands:\n"
        "  id                      get chip id (0x0001) and decode it\n"
        "  reset                   soft reset (0x0008) and read its reply\n"
        "  cmd <hex> [timeout_ms]  write an opcode, read one reply\n"
        "  send <hex>              write an opcode, read nothing\n"
        "  recv [len] [timeout_ms] read one packet, write nothing\n"
        "  cap [file]              one capture; optionally write the raw image\n"
        "  loop <n> [delay_ms]     n captures back to back -- the wedge reproducer\n"
        "  usbreset                real USB bus reset (the known wedge cure)\n"
        "  sleep <ms>              pause\n"
        "  note <text>             mark the timeline (e.g. 'note finger down')\n"
        "  help, quit\n"
        "\n"
        "after a wedge, 'cmd 0001' asks whether anything still answers.");
}

static int parse_opcode(const char *tok, unsigned short *out) {
    char *end = NULL;
    unsigned long v = strtoul(tok, &end, 16);
    if (!end || *end != '\0' || v > 0xffff) {
        fprintf(stderr, "not a 16-bit hex opcode: %s\n", tok);
        return -1;
    }
    *out = (unsigned short)v;
    return 0;
}

static void cmd_id(void) {
    fpc_chip_info chip;
    if (fpc_get_chip_id(dev, &chip) == 0)
        printf("chip: %s (%dx%d)\n", chip.name, chip.width, chip.height);
    else
        printf("chip: unidentified\n");
}

static void cmd_capture(const char *path) {
    unsigned char *img = NULL;
    int len = 0;
    fpc_cap_result rc = fpc_try_capture(dev, DEFAULT_READ_TIMEOUT_MS, &img, &len);

    switch (rc) {
        case FPC_CAP_OK:        printf("capture ok: %d bytes\n", len); break;
        case FPC_CAP_NOT_READY: puts("not ready (no finger?)"); return;
        case FPC_CAP_TIMEOUT:   puts("timeout waiting for the capture reply"); return;
        case FPC_CAP_ERROR:     puts("capture error"); return;
    }

    if (path) {
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(img, 1, (size_t)len, f);
            fclose(f);
            printf("wrote %s\n", path);
        } else {
            fprintf(stderr, "could not write %s\n", path);
        }
    }
    free(img);
}

/*
 * The wedge reproducer. Runs n captures back to back, counting consecutive
 * timeouts, and reports which capture the sensor stopped answering on --
 * the number the whole investigation turns on.
 */
static void cmd_loop(int n, int delay_ms) {
    int ok = 0, not_ready = 0, timeouts = 0, errors = 0;
    int consecutive_timeouts = 0;
    int wedged_at = -1;
    int i;

    fpc_trace_event("loop_begin", "%d captures, %dms apart", n, delay_ms);

    for (i = 1; i <= n; i++) {
        unsigned char *img = NULL;
        int len = 0;
        double t0 = fpc_trace_now_ms();

        fpc_trace_event("capture_begin", "#%d", i);
        fpc_cap_result rc = fpc_try_capture(dev, DEFAULT_READ_TIMEOUT_MS, &img, &len);
        double elapsed = fpc_trace_now_ms() - t0;

        switch (rc) {
            case FPC_CAP_OK:
                ok++;
                consecutive_timeouts = 0;
                fpc_trace_event("capture_end", "#%d ok, %d bytes, %.0fms", i, len, elapsed);
                free(img);
                break;
            case FPC_CAP_NOT_READY:
                not_ready++;
                consecutive_timeouts = 0;
                fpc_trace_event("capture_end", "#%d not_ready, %.0fms", i, elapsed);
                break;
            case FPC_CAP_TIMEOUT:
                timeouts++;
                consecutive_timeouts++;
                fpc_trace_event("capture_end", "#%d timeout (%d in a row), %.0fms",
                                i, consecutive_timeouts, elapsed);
                break;
            case FPC_CAP_ERROR:
                errors++;
                consecutive_timeouts = 0;
                fpc_trace_event("capture_end", "#%d error, %.0fms", i, elapsed);
                break;
        }

        if (wedged_at < 0 && consecutive_timeouts >= WEDGE_TIMEOUT_THRESHOLD) {
            wedged_at = i - consecutive_timeouts + 1;
            fpc_trace_event("wedge", "%d consecutive timeouts; first was capture #%d",
                            consecutive_timeouts, wedged_at);
            printf("\n*** wedged: stopped answering at capture #%d "
                   "(%d captures succeeded first)\n", wedged_at, ok);
            puts("*** try 'cmd 0001' to see if anything still answers, "
                 "then 'usbreset' to recover\n");
            break;
        }

        if (delay_ms > 0) usleep((useconds_t)delay_ms * 1000);
    }

    fpc_trace_event("loop_end", "ok=%d not_ready=%d timeouts=%d errors=%d",
                    ok, not_ready, timeouts, errors);
    printf("loop done: ok=%d not_ready=%d timeouts=%d errors=%d\n",
           ok, not_ready, timeouts, errors);
    if (wedged_at < 0 && timeouts == 0)
        puts("no wedge in this run");
}

static void run_command(int argc, char **argv) {
    const char *c = argv[0];

    if (strcmp(c, "help") == 0 || strcmp(c, "?") == 0) {
        print_help();
    } else if (strcmp(c, "id") == 0) {
        cmd_id();
    } else if (strcmp(c, "reset") == 0) {
        unsigned char buf[FPC_MAX_PACKET];
        int got = 0;
        if (fpc_trace_write_cmd(dev, "reset", CMD_RESET, 2000) == 0)
            fpc_trace_read_reply(dev, "reset.reply", buf, sizeof(buf), &got,
                                 DEFAULT_READ_TIMEOUT_MS);
    } else if (strcmp(c, "cmd") == 0 && argc >= 2) {
        unsigned short op;
        if (parse_opcode(argv[1], &op) != 0) return;
        int timeout = (argc >= 3) ? atoi(argv[2]) : DEFAULT_READ_TIMEOUT_MS;
        unsigned char buf[FPC_MAX_PACKET];
        int got = 0;
        if (fpc_trace_write_cmd(dev, "probe", op, 2000) == 0)
            fpc_trace_read_reply(dev, "probe.reply", buf, sizeof(buf), &got, timeout);
    } else if (strcmp(c, "send") == 0 && argc >= 2) {
        unsigned short op;
        if (parse_opcode(argv[1], &op) != 0) return;
        fpc_trace_write_cmd(dev, "probe", op, 2000);
    } else if (strcmp(c, "recv") == 0) {
        int len = (argc >= 2) ? atoi(argv[1]) : FPC_MAX_PACKET;
        int timeout = (argc >= 3) ? atoi(argv[2]) : DEFAULT_READ_TIMEOUT_MS;
        if (len <= 0 || len > FPC_MAX_PACKET) len = FPC_MAX_PACKET;
        unsigned char buf[FPC_MAX_PACKET];
        int got = 0;
        fpc_trace_read_reply(dev, "probe.reply", buf, sizeof(buf) < (size_t)len ?
                             (int)sizeof(buf) : len, &got, timeout);
    } else if (strcmp(c, "cap") == 0) {
        cmd_capture(argc >= 2 ? argv[1] : NULL);
    } else if (strcmp(c, "loop") == 0 && argc >= 2) {
        int n = atoi(argv[1]);
        int delay = (argc >= 3) ? atoi(argv[2]) : DEFAULT_LOOP_DELAY_MS;
        if (n > 0) cmd_loop(n, delay);
    } else if (strcmp(c, "usbreset") == 0) {
        fpc_usb_reset(dev);
    } else if (strcmp(c, "sleep") == 0 && argc >= 2) {
        int ms = atoi(argv[1]);
        if (ms > 0) usleep((useconds_t)ms * 1000);
    } else if (strcmp(c, "note") == 0) {
        char msg[PROBE_LINE_MAX] = "";
        int i;
        for (i = 1; i < argc; i++) {
            if (i > 1) strncat(msg, " ", sizeof(msg) - strlen(msg) - 1);
            strncat(msg, argv[i], sizeof(msg) - strlen(msg) - 1);
        }
        fpc_trace_event("note", "%s", msg);
    } else {
        fprintf(stderr, "unknown command: %s (try 'help')\n", c);
    }
}

static int tokenize(char *line, char **argv, int max) {
    int argc = 0;
    char *tok = strtok(line, " \t\r\n");
    while (tok && argc < max) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t\r\n");
    }
    return argc;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [-t trace.jsonl | -n] [-q|-vv] [command ...]\n"
        "  -t PATH   JSONL trace path ('-' auto-names by timestamp, the default)\n"
        "  -n        no JSONL trace file\n"
        "  -q        no per-transfer output on stderr\n"
        "  -vv       include image-stream packets on stderr\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *trace_path = "-";
    fpc_trace_level level = FPC_TRACE_NORMAL;
    int i, first_cmd = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) trace_path = argv[++i];
        else if (strcmp(argv[i], "-n") == 0) trace_path = NULL;
        else if (strcmp(argv[i], "-q") == 0) level = FPC_TRACE_QUIET;
        else if (strcmp(argv[i], "-vv") == 0) level = FPC_TRACE_FULL;
        else if (strcmp(argv[i], "-h") == 0) { usage(argv[0]); return 0; }
        else { first_cmd = i; break; }
    }

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (fpc_trace_open(trace_path, level) != 0) return 1;

    libusb_context *ctx = NULL;
    if (fpc_open(&ctx, &dev) != 0) {
        fpc_trace_close();
        return 1;
    }

    fpc_chip_info chip;
    if (fpc_get_chip_id(dev, &chip) == 0)
        printf("sensor: %s (%dx%d)\n", chip.name, chip.width, chip.height);
    else
        puts("sensor: chip id not readable (already wedged?)");
    if (fpc_trace_path()) printf("trace: %s\n", fpc_trace_path());

    if (first_cmd) {
        run_command(argc - first_cmd, argv + first_cmd);
    } else {
        int interactive = isatty(STDIN_FILENO);
        if (interactive) puts("type 'help' for commands, 'quit' to exit");

        char line[PROBE_LINE_MAX];
        for (;;) {
            if (interactive) { fputs("fpc> ", stdout); fflush(stdout); }
            if (!fgets(line, sizeof(line), stdin)) break;

            char *args[PROBE_MAX_ARGS];
            int n = tokenize(line, args, PROBE_MAX_ARGS);
            if (n == 0) continue;
            if (strcmp(args[0], "quit") == 0 || strcmp(args[0], "exit") == 0) break;
            run_command(n, args);
        }
    }

    fpc_close(ctx, dev);
    if (fpc_trace_path()) printf("trace written to %s\n", fpc_trace_path());
    fpc_trace_close();
    return 0;
}
