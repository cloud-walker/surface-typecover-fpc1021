# libfprint driver

`fpc1021.c` is a working `FpImageDevice` driver for libfprint, implementing
the protocol documented in [`../PROTOCOL.md`](../PROTOCOL.md).

## Status

**Working, and enrollment completes end to end** (2026-08-29): built against
a fresh checkout of [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint),
correctly detects the device, identifies the FPC1021 chip via Get Chip ID,
waits for a finger, and captures a full clean 160x160 image — confirmed via
libfprint's own `examples/img-capture` tool, which wrote a real, recognizable
fingerprint to `finger.pgm` and even ran libfprint's minutiae detection on it
successfully.

**Fixed (2026-08-29): the wedge was a reply-queue desynchronisation, and
enrollment now completes.**

The symptom: after roughly 2-3 successful captures the sensor appeared to
stop answering the capture-reply read entirely, until the whole USB device
was reset — which made libfprint think the device had disconnected and
aborted whatever `fprintd-enroll` session was in progress, so a multi-stage
enrollment could never finish.

The cause, found by tracing a real enrollment at the kernel URB level with
`tools/usbmon-watch.sh`: **the sensor emits a second, unrequested image
after most captures.** Because this protocol pairs reads and writes 1:1,
that unread image puts every later reply behind 413 stale packets (one
header plus 412 stream packets). Reset gets answered by pixel data, Get Chip
ID by stream packets, and once the backlog runs out the device goes quiet
and every read times out. Nothing is actually stuck; the host is simply
reading one conversation behind.

The fix is a `CAPTURE_DRAIN` state that empties the IN endpoint before each
capture, reading with a 50ms timeout until it comes back empty. Measured on
a completed enrollment:

```
fpc1021: drained 413 stale packet(s) before capture     (x6)
capture commands sent (0700):  8
capture headers received:     14
```

Six extra headers, six drains, exact accounting. `Enroll result:
enroll-completed` — five stages plus two retry-scans, print saved.

**The drain has to cover open too.** The first `fprintd-verify` after a
completed enrollment failed with `unrecognised FPC chip ID 0x6400` — and
0x6400 is 25600, the image-length field of a capture header being decoded as
a chip ID. The extra image survives a close/open cycle, so the reply to
Get Chip ID at open time was a stale header. `OPEN_DRAIN` now runs before it.

That failure also showed a gap worth closing generally: the driver believed
a reply that did not acknowledge the command it had sent. Replies carry
`status = 0x1000 | opcode` precisely so that can be checked, and the chip-ID
path now rejects a mismatch with "desynchronised reply" instead of decoding
whatever bytes arrived.

**Still open: where the extra image comes from.** It did not reproduce over
plain libusb in 21 captures, including four paced to match libfprint's own
~7.5ms/packet drain, and a bare read after a capture with the finger held
down always timed out. Whatever triggers it, the driver no longer needs to
know: draining makes an unread image harmless instead of fatal. The
never-ported opcode `0x0005` remains the natural suspect for a "stop
capture" command that would prevent the frame rather than discard it.

Two earlier observations, kept because they cost time to establish:
`CLEAR_FEATURE(ENDPOINT_HALT)` on both endpoints does not restore touch
detection, and `libusb_reset_device()` returned `ok` without clearing the
stuck state while a physical unplug/replug cleared it instantly. The USB
reset recovery path is retained as a last resort but should no longer
trigger in normal use.

## Matching

`fprintd-verify` returned `verify-no-match` four times in a row against a
freshly completed enrollment, on captures that look excellent — sharp dark
ridges on light valleys, correct polarity for NBIS. So the problem was not
acquisition.

`tools/fpc_minutiae.c` counts what libfprint actually extracts from a raw
capture, which turns that into a measurement:

```
image: 160x160, 0.00 px/mm
minutiae detected: 11
```

Two candidate defects behind those numbers. **Both were tried and reverted**
— see below — but they are recorded because the reasoning still holds and
the second is probably still worth fixing on its own:

- **No `FPI_IMAGE_PARTIAL`.** On a 160x160 patch the image edge is not the
  finger's edge, so every ridge running out of frame reads as a ridge
  ending. The flag makes NBIS drop those perimeter artefacts; `elan.c` and
  `elanspi.c` both set it for the same reason.
- **`ppmm` left at 0.0.** NBIS sizes the neighbourhood it scores each
  minutia's reliability over as `RADIUS_MM * ppmm`, so zero collapses it to
  no pixels at all. The FPC capacitive family is specified at 508 dpi.

**Setting both made enrollment worse, not better.** Captures still succeeded
(`CAPTURE_NUM_STATES completed successfully`) but no enroll stage ever
passed — libfprint accepted none of the images, and the enrollment sat there
until it was interrupted. Starting from 11 minutiae, `FPI_IMAGE_PARTIAL`
evidently removes enough of them to fall under whatever libfprint needs to
accept a stage. The flag is right in principle and wrong at this minutia
count.

Reverted to the state where enrollment completes. Worth retrying separately:
`ppmm` alone, since it only affects reliability scoring and was never the
suspect for stage rejection, and the two were changed together — which made
it impossible to tell which one did the damage.

The real lever is likely upstream of all this: 11 minutiae is thin, bozorth3
needs overlap between two sets, and nothing currently stops a poor frame
entering the template. That is the image-quality gating below.

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
