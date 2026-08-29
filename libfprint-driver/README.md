# libfprint driver

`fpc1021.c` is a working `FpImageDevice` driver for libfprint, implementing
the protocol documented in [`../PROTOCOL.md`](../PROTOCOL.md).

## Status

**Working and validated against real hardware** (2026-08-29): built against
a fresh checkout of [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint),
correctly detects the device, identifies the FPC1021 chip via Get Chip ID,
waits for a finger, and captures a full clean 160x160 image — confirmed via
libfprint's own `examples/img-capture` tool, which wrote a real, recognizable
fingerprint to `finger.pgm` and even ran libfprint's minutiae detection on it
successfully.

Not yet done:
- Only tested with a single finger placement per run; the "await finger"
  retry-loop timing (`FPC_RETRY_DELAY_MS`/`FPC_CAPTURE_COOLDOWN_MS` in the
  driver) is a reasonable guess, not tuned against extended real-world use.
- Not tested for enroll/verify workflows end-to-end (via `fprintd`), only
  raw image capture.
- Not yet submitted upstream.

## Applying this to a libfprint checkout

```sh
git clone https://gitlab.freedesktop.org/libfprint/libfprint.git
cp fpc1021.c libfprint/libfprint/drivers/
cd libfprint
git apply ../meson.build.patch   # registers the driver in both meson.build files
meson setup build -Ddoc=false
ninja -C build
```

Then, with the Surface Type Cover attached:

```sh
sudo env LD_LIBRARY_PATH=./build/libfprint ./build/examples/img-capture
```

(root is needed only because there's no udev rule yet granting user access
to this specific USB device; a proper udev rule is one of the remaining
upstreaming steps.)
