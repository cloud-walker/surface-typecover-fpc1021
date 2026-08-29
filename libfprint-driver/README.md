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

**Update (2026-08-29, later that day), on the reset that is supposed to
cure it:** a session with `tools/fpc_probe` found the sensor already in the
stuck state — the soft reset opcode `0x0008` replying normally with `sub=0`
in ~0.25ms, while every capture reply timed out, across an hour and every
attempt. `libusb_reset_device()` returned `ok` and **did not clear it**.
Physically unplugging and replugging the Type Cover did, immediately: the
very next capture succeeded on the first finger press, both through the
original prototype and through the probe.

`g_usb_device_reset()` is the same `USBDEVFS_RESET` ioctl, so the claim
above that it fixes the wedge needs re-testing. If it doesn't, the driver's
auto-recovery pays the whole cost of a reset — libfprint sees a
disconnect and aborts the enrollment in progress — without getting the
benefit, which would explain why a 5-stage enroll never completes.

Not yet established: whether that stuck state is the same wedge described
above. It was found already stuck, not observed entering, and the wedge
proper requires successful captures first. Reproducing it from a
known-good device is the next measurement.

**Root cause found (2026-08-29, via `tools/usbmon-watch.sh` during a real
`fprintd-enroll`): the wedge is a reply-queue desynchronisation, not a
hardware lockup.**

The URB-level trace of an enrollment that wedged after two stages:

```
 5997.453  IN  len=64   07100000 0064...   capture #1 header, 414 reads, 26434 bytes  OK
 9685.474  IN  len=64   07100000 0064...   capture #2 header, 414 reads, 26434 bytes  OK
12771.935  OUT len=2    0800               reset
12772.433  IN  len=64   07100000 0064...   <-- a capture HEADER answers the reset
12773.416  IN  len=64   07118a8f 8e93...   <-- and behind it, image stream data
```

Only two Capture commands (`0700`) were ever sent, at 3468 and 9085, and both
images were read out in full. Yet a third capture header arrives at 12772,
as the reply to a Reset. The sensor produced an image nobody asked for, and
nobody drained it.

From there the 1:1 pairing the protocol requires is broken: every read
returns the reply to some earlier command, with ~412 stale stream packets
queued ahead of it. The driver sees `0800` answered by `0711...` pixel data,
`0100` answered by stream packets, and a Capture answered by a Reset reply.
Once the backlog runs out the device goes quiet and every read is cancelled
at the 3000ms timeout (`status=-2`, `-ENOENT`, i.e. libusb unlinking it) —
which is the state that looks like a wedge.

This explains every observation that confused the investigation:

- **Why plain libusb never reproduces it**: `fpc_capture` and `fpc_probe`
  always drain a full image before sending anything else, and their
  back-to-back gap is ~200ms. 17 captures, no wedge.
- **Why the soft reset appears to answer while wedged**: it is not
  answering. Those `08100000` replies are stale queued data.
- **Why it dies at a stage boundary**: libfprint runs minutiae detection
  between enrollment stages, leaving seconds of gap with the finger still
  on the sensor.
- **Why a USB reset does not cure it** while a physical replug does: the
  ioctl does not flush whatever the device has queued; a power cycle does.

Working hypothesis for the unasked-for image: with a finger still present,
the sensor re-arms and captures another frame on its own if enough time
passes. Our loop left only 200ms between captures and never saw it; the
enrollment left ~3s. **This makes the never-ported opcode `0x0005` the prime
suspect for a "stop/flush capture" command** — the Windows driver sends it,
a single capture does not need it, and this is exactly the gap it would fill.

Next measurements, in order:
1. `cap`, `sleep 3000` with the finger held down, then `recv` — if data comes
   back, the sensor free-runs and the mechanism is confirmed.
2. `cmd 0005` after a capture, then `recv` — does it stop the sensor
   producing frames?
3. Whether draining until empty on activate is enough to make enrollment
   complete, even without `0x0005`.

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
