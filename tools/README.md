# Diagnostics

`fpc_probe` is an interactive probe over the FPC1021 wire protocol, built on
the same device and tracing layers as `../src/fpc_capture.c`. It exists to
answer questions about the sensor's *behaviour over time* — above all the
wedge described in [`../libfprint-driver/README.md`](../libfprint-driver/README.md):
after 2-3 successful captures the sensor stops answering reads until a full
USB reset.

```sh
make                       # builds build/fpc_capture and build/fpc_probe
sudo ./build/fpc_probe     # REPL
```

Root is needed only because there's no udev rule yet for this device.

## Why a probe and not more driver rebuilds

The wedge was found through `fprintd-enroll` against a patched Arch
`libfprint` package. That loop costs minutes per attempt and drags libfprint's
own state machine into every observation. `fpc_probe loop 10` reproduces the
same sequence of USB transfers in seconds, over plain `libusb`, which also
settles the first question worth settling: **does the wedge happen outside
libfprint at all?**

- If it does, it's the protocol or the hardware.
- If it doesn't, the bug is in the driver's state machine — a reply left
  unconsumed, say, which the 1:1 read/write pairing of this protocol punishes
  by desynchronising everything after it.

Those are different investigations, and the answer costs one command.

## Commands

| Command | Meaning |
|---|---|
| `id` | Get Chip ID (`0x0001`), decoded against the chip table |
| `reset` | soft reset (`0x0008`) and consume its reply |
| `cmd <hex> [timeout_ms]` | write an arbitrary opcode, read one reply |
| `send <hex>` / `recv [len] [timeout_ms]` | write or read in isolation, for when the 1:1 pairing has been broken deliberately |
| `cap [file]` | one capture attempt, optionally saved as a raw image |
| `loop <n> [delay_ms]` | `n` captures back to back — the wedge reproducer |
| `usbreset` | real USB bus reset: the only known cure for a wedge |
| `sleep <ms>` | pause |
| `note <text>` | annotate the timeline |

`note` is how physical actions get into the trace. Type `note finger down`
before touching the sensor and the timestamp lands next to the transfers it
explains — which is what makes a trace readable a day later.

## The wedge workflow

```sh
sudo ./build/fpc_probe -t wedge.jsonl
fpc> note finger down, holding
fpc> loop 10
```

`loop` counts consecutive capture timeouts, but a timeout is also the
ordinary "waiting for a finger" state: the capture read simply does not
return until a finger is on the sensor. So timeouts alone are not the wedge.
The wedge is a sensor that answered captures and then stopped, which is why
`loop` reports it only when at least one capture succeeded first; with none,
it says so plainly instead.

Get a single `cap` working before running a loop. And land the finger just
after starting a capture rather than resting it there beforehand — a run
where the finger was already down for the whole loop produced six timeouts
and zero captures, with the soft reset replying cleanly in 0.25ms before
every one of them.

Once it does announce a wedge, the interesting question is what — if
anything — still responds:

```
fpc> cmd 0001      # does Get Chip ID still answer while wedged?
fpc> cmd 0008      # does the sensor's own reset opcode?
fpc> usbreset      # recover
fpc> cmd 0001      # confirm it's back
```

A reply while wedged is the best lead available: its `substatus` says what
state the sensor believes it is in. Silence everywhere is itself a result —
it rules out a stuck command queue and points at the USB layer, where
`usbmon` can see what `libusb` hides.

Opcode `0x0005` is the obvious first hypothesis: the Windows driver sends it,
a single capture doesn't need it, and the wedge appears only across repeated
captures. `cmd 0005` after each capture is the cheap test.

**On hunting for unknown opcodes:** probe opcodes you have a hypothesis
about, one at a time. Don't sweep the space. FPC chips carry calibration and
OTP registers, and this sensor is soldered inside a Type Cover — an unknown
opcode could write persistent state. There is deliberately no sweep command.

## Watching what another program does: `usbmon-watch.sh`

`trace-watch.sh` renders our own libusb calls. It cannot see a capture driven
by libfprint or fprintd, which is where the wedge actually lives —
17 captures over plain libusb never reproduced it.

`usbmon-watch.sh` traces the sensor's bulk endpoints at the kernel URB level
instead, so it sees the traffic whoever generates it:

```sh
sudo modprobe usbmon
sudo tools/usbmon-watch.sh | tee usbmon-enroll.txt   # pane 2
fprintd-enroll                                        # pane 1
```

```
     0.000  OUT submit    len=2      status=-115  0100
     0.133  OUT complete  len=2      status=0
     0.221  IN  submit    len=64     status=-115
     0.361  IN  complete  len=6      status=0     01100000 1b02
```

It finds the device's bus and device number itself and filters to endpoints
4 (out) and 3 (in) — interface 0 is the keyboard, and its interrupt traffic
would bury everything.

The reason to reach for it: at the libusb API level, a device that NAKs
forever, one that stalls, and one that was never asked anything all surface
as the same failed read. At URB level they are three different pictures, and
three different bugs. `status=-115` is `-EINPROGRESS`, the normal marker on a
submit; a completion carries the real status.

## Offline matching bench: `fpc_bench.c`

Every matching question used to cost a `fprintd-delete`, a six-press
enrollment and several verifications — with a real finger — for one noisy
data point, and comparisons made that way vary conditions as much as the
thing under test. `fpc_bench` replays saved raw captures through the driver's
own pipeline instead: enlargement, scan resolution, blank-frame gate, then
libfprint's `fpi_print_add_from_image()` and the same bozorth3 entry point
`fpi_print_bz3_match()` calls. The scores are the ones fprintd would report.

```
$ fpc_bench -e 2 shot1.bin shot2.bin shot3.bin
enlarge 2x  ->  320x320      threshold 24      blank gate 40

capture           contrast minutiae
shot1.bin             77.7       11
shot2.bin            235.2       20
shot3.bin            206.8       23

scores (row = probe, column = gallery)
                     0     1     2
 0 shot1.bin         .     6     7
 1 shot2.bin         6     .    15
 2 shot3.bin         7    15     .

over 6 ungated pairs:  best 15   mean 9.3   at or above 24: 0 (0%)
```

Options: `-e` enlargement factor, `-t` threshold, `-g` blank-frame gate
(0 disables), `-s`/`-a` unsharp sigma and amount (`-a 0` disables sharpening),
`-m` bozorth3's computable minutia floor, `-S` a subject label, `-w`/`-h`
capture dimensions.

It found, in seconds, that the 3x enlargement chosen from live testing was
worse than 2x — see `../libfprint-driver/README.md`.

### Label the fingers: `-S`

Separation between genuine and impostor comparisons is the objective here, and
for a while it was computed by reading the score matrix by hand. That went
wrong once — a classification dropped the second finger's own genuine pairs,
which made the separation look inverted rather than merely tiny.

So the bench classifies pairs itself. `-S` labels every capture after it:

```sh
fpc_bench -e 2 -S left-index shot1.bin place1.bin -S right-index other1.bin other3.bin
```

Same label, genuine pair; different label, impostor pair. Filenames are never
parsed for this, because no convention could know that `shot*` and `place*` are
the same finger. Unlabelled captures are scored and printed but left out of the
separation statistics, since a capture belonging to no finger cannot be
classified either way.

With two or more labels the run ends with:

```
separation

            pairs    mean      sd
  genuine      32    18.9     9.0
  impostor     24    13.5     9.9

  d'  = 0.58   (usable biometrics sit above 3)
  AUC = 0.687   (0.5 is chance; d' assumes Gaussians, AUC does not)

  at the shipped threshold 24:  accepts 38% of genuine, 17% of impostor
  best operating point 16:      accepts 62% of genuine, 17% of impostor
```

`d'` is the distance between the two means in pooled standard deviations. It is
the number that decides whether any threshold is safe, and unlike a raw score it
compares across configurations. `AUC` is the probability that a random genuine
pair outscores a random impostor pair, ties splitting the credit — reported
alongside because d' assumes two Gaussians and these distributions are not. On a
weak configuration most scores are exactly zero, and that pile of ties shrinks
the standard deviations and inflates d'; AUC has no such assumption, and 0.5 is
chance. The operating point is the threshold maximising
genuine-accept minus false-accept — with distributions this close no threshold
is simply right, and what the best available trade *costs* is the honest report.

Gated frames are excluded from both classes, which is what the driver does to
them in reality. An earlier hand count evidently included them; it reported a d'
of 0.26 for the configuration that measures 0.58 here.

### Lowering the bozorth3 floor: `-m`

`bozorth3` refuses to compute a score if either side carries fewer than
`MIN_COMPUTABLE_BOZORTH_MINUTIAE` minutiae. NIST made that adjustable — the
`bozorth3(1E)` man page documents `-A minminutiae=#`, "That number is 10 by
default, and can be changed to any non-zero integer" — but libfprint's
re-vendoring script strips NIST's runtime globals and freezes it into a
`#define` (`nbis/update-from-nbis.sh:83-87`).

`../libfprint-driver/bozorth-floor.patch` restores the variable, defaulting to
NIST's 10 so nothing changes unless a caller asks:

```sh
cd /path/to/libfprint && git apply /path/to/repo/libfprint-driver/bozorth-floor.patch
ninja -C build
```

Then `-m N` sets it. **Without the patch the bench still builds** and everything
except `-m` behaves identically — `-m` reports what is missing and exits — so a
stock libfprint checkout remains usable.

### Minutia reliability: `-q`

`mindtct` scores every minutia for how much it trusts it, and libfprint computes
that score and then drops it — only x, y and theta reach the `xyt_struct` that
bozorth3 matches on (`fpi-print.c:138-152`). `-q N` drops minutiae under N%
reliability before the template is built, and the per-capture listing gains two
columns: how many minutiae the frame produced, how many survived the filter, and
the frame's median reliability.

Measured, it does not help — see `../libfprint-driver/README.md`. It is kept
because the answer was not obvious and the next person will want to ask.

### `-s 0` used to hand NBIS a frame of NaN

Worth knowing, because it produced a plausible-looking wrong answer rather than
a crash. Sigma 0 divides by zero in the Gaussian, every pixel becomes NaN, and
the frame yields zero minutiae — which reads exactly like "this configuration
finds nothing" instead of like a bug. Sharpening is now skipped when either
sigma or amount is zero. Use `-a 0` to turn it off.

### Building it

Nothing `fpi_*` is exported from the shared library, so this links against a
libfprint **build tree**'s static archives:

```sh
SRC=/path/to/libfprint          # source checkout
B=$SRC/build                    # its meson build directory
PKGS="glib-2.0 gobject-2.0 gio-2.0 gmodule-2.0 gusb json-glib-1.0 pixman-1 gudev-1.0 openssl"

cc -O2 -Wall -std=gnu11 tools/fpc_bench.c -o build/fpc_bench \
  -I"$SRC/libfprint" -I"$SRC" -I"$B" -I"$B/libfprint" \
  -I"$SRC/libfprint/nbis/include" -I"$SRC/libfprint/nbis/libfprint-include" \
  $(pkg-config --cflags $PKGS) \
  "$B/libfprint/libfprint-private.a" "$B/libfprint/libnbis.a" \
  -L"$B/libfprint" -lfprint-2 -Wl,-rpath,"$B/libfprint" \
  $(pkg-config --libs $PKGS) -lm
```

Collect samples with `sudo ./build/fpc_capture shot1.bin`, one per press.

## Collecting the ghost frame

The driver drains 413 packets before most captures — one capture header plus 412
stream packets, a complete unrequested 160x160 frame. Where it comes from is
still unexplained and the frame itself has never been looked at: it has only
been counted and dropped.

It does not reproduce over plain libusb (21 captures, including four paced to
libfprint's own drain rate), so `fpc_probe` cannot collect it. The place it
demonstrably happens is libfprint's own path, so the collection lives in the
driver, behind an environment variable:

```sh
sudo mkdir -p /etc/systemd/system/fprintd.service.d
sudo tee /etc/systemd/system/fprintd.service.d/ghost.conf <<'EOF'
[Service]
Environment=FPC1021_GHOST_DIR=/path/to/a/writable/dir
EOF
sudo systemctl daemon-reload && sudo systemctl restart fprintd
```

Then drive the device normally — `fprintd-enroll` is the reliable producer,
since the drain fires before each of its stages — and the frames land as
`ghost-<pid>-<n>.bin`, raw 160x160 grayscale, the same format `fpc_capture`
writes. `journalctl -u fprintd | grep ghost` confirms each write.

Unset, the variable costs one comparison per drained packet and changes nothing;
the frame is discarded exactly as before, it is just written down first.

**Blank frames in that directory are not sensor faults.** The requested frame is
written *before* the blank-frame gate, so a capture the driver rejected still
lands on disk — as an all-white raster, byte-identical between occurrences.
Reading the directory cold, one collection looked like four sensor failures and
was four ordinary rejections.

**`Finger present 1` / `0` in the journal is not a presence measurement.** The
FPC1021 has no finger-presence interrupt. Those lines are the driver's own
`fpi_image_device_report_finger_status()` calls — `TRUE` when a capture header
arrives, `FALSE` when the frame is delivered or rejected — so both transitions
land inside the same second regardless of how long the finger was actually
there. They cannot be used to measure hold duration, which matters because hold
duration is what decides whether the ghost frame is a print or a blank: across
three enrolments the ghost yield ran 2 of 6, 6 of 8 and 10 of 11, and the only
thing that varied was how promptly the finger came off. Fix it as a protocol —
the same deliberate count on every press — rather than trying to measure it.

Two questions it can answer. Whether the ghost is a real second view of the
finger at all — which would finally explain it — and, if it is, whether
averaging it with the requested frame helps. Two frames of one press cover the
same skin so averaging cannot add area, but it can average away sensor noise,
and noise amplified into minutiae that differ between two views of one finger is
the failure this driver's matching actually has.

## Trace format

Every transfer is recorded on both sinks: a human-readable line on stderr,
and a JSON object per line in the `.jsonl` file (`-t PATH`, or `-` to
auto-name by timestamp; `-n` for no file).

```
     7.560 > chip_id         2B  01 00                    0.13ms
     7.711 < chip_id.reply   6B  01 10 00 00 1b 02        0.23ms  status=0x1001(get_chip_id) sub=0 chip_id=0x021b
  3049.100 < capture.hdr     0B  LIBUSB_ERROR_TIMEOUT  3000.02ms
```

```json
{"t":7.711,"dur":0.220,"dir":"in","tag":"chip_id.reply","rc":0,"req":64,"len":6,
 "hex":"011000001b02","status":4097,"acks":"get_chip_id","substatus":0,"chip_id":539}
```

Fields: `t` ms since trace start, `dur` how long the transfer itself took,
`tag` the caller's name for the intent of the transfer, `rc` the libusb
result (with `rc_name` when it failed), `req`/`len` bytes requested and
transferred, `hex` the raw bytes, and the decoded reply header.

Two fields carry most of the diagnostic weight:

- **`dur`** separates "answered instantly" from "sat there for the full
  timeout". At the libusb API level both look like a failed read; they are
  not the same event.
- **`tag`** makes a trace greppable and two traces comparable. Bytes 4-5 of a
  reply mean the payload length after a capture and the chip-ID word after a
  Get Chip ID, so replies are decoded by the opcode they acknowledge, never
  by their size.

The file is line-buffered, so a trace survives killing a wedged process.

## Watching a run live

`trace-watch.sh` follows the newest trace as it is written, which is how to
watch a session from a second terminal while driving the probe in the first:

```sh
tools/trace-watch.sh          # pane 2: waits for a trace, then follows it
sudo ./build/fpc_probe -t -   # pane 1: '-' auto-names the trace by timestamp
```

```
     7.326  <  chip_id.reply 6B      sub=0 chip_id=539
   100      *  capture_begin  #1
  3130      <  capture.hdr   0B      LIBUSB_ERROR_TIMEOUT  [3000ms]
  9000      *  wedge          6 consecutive timeouts; first was capture #3
```

Transfers that took more than 100ms are called out in brackets; everything
else on this sensor answers in well under a millisecond, so a bracket is
always worth a look.

## Diffing two runs

The point of the JSONL sink. Compare the captures that worked against the one
that wedged:

```sh
jq -c 'select(.tag) | {tag, rc, len, substatus}' wedge.jsonl | uniq -c
```

Image-stream packets are the bulk of a trace by volume (~440 per capture) and
are kept off stderr unless `-vv` is passed — they are always in the JSONL.
