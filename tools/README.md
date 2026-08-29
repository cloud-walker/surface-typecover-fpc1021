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

`loop` counts consecutive capture timeouts and reports which capture the
sensor stopped answering on. Once it announces a wedge, the interesting
question is what — if anything — still responds:

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
