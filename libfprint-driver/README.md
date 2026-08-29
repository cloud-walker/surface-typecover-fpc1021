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

**Known bug, blocking full enrollment** (found 2026-08-29 testing via
`fprintd-enroll`, packaged as a patched Arch `libfprint` and run through
Omarchy's own `omarchy-setup-security-fingerprint`): after roughly 2-3
successful captures in a row, the sensor stops answering the
capture-reply read entirely (every read times out, even with a finger
present) until the *whole USB device* is reset — a plain unplug/replug of
the Type Cover, or `g_usb_device_reset()`, both fix it, but a lighter
`CLEAR_FEATURE(ENDPOINT_HALT)` on both endpoints does **not** restore
touch detection at all, so this isn't a simple USB pipe stall. The driver
auto-recovers via a real USB reset after `FPC_MAX_CONSECUTIVE_TIMEOUTS`
(6) timeouts in a row, but that reset makes libfprint think the device
was disconnected, which aborts whatever `fprintd-enroll`/verify session
was in progress — so a multi-stage enrollment (5 stages) currently cannot
reliably complete in one go. Root cause still unknown; the likely next
lead is an uninvestigated call in the original Windows driver's reset
routine (`FUN_1800072a4`) to a *different* object's vtable+0x60 that was
never traced — see project memory / a future session for details. This
is the main blocker before this driver is genuinely useful day-to-day.

Also not yet done:
- The "await finger" retry-loop timing (`FPC_RETRY_DELAY_MS`,
  `FPC_CAPTURE_COOLDOWN_MS`, the 3000ms read timeout in
  `CAPTURE_READ_HEADER`) reflects what got a full raw-image capture
  working, not necessarily what's needed for reliable back-to-back
  captures — see the bug above.
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
