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

### Why verification returns no match: the bozorth3 floor

`fprintd-verify` fails with `score 0/24` on **every** comparison — 25 of 25
across five attempts, never a low score, always exactly zero. The reason is
in `nbis/include/bozorth.h`:

```c
#define MIN_COMPUTABLE_BOZORTH_MINUTIAE 10
```

`bz_match_score()` returns `ZERO_MATCH_SCORE` without attempting a
comparison if *either* side has fewer than 10 minutiae. The stored template
holds five prints with 7, 5, 8, 2 and 7 minutiae; a verification image
carries about 8. Every comparison is short-circuited before matching starts.

So `bz3_threshold` is irrelevant here — the score is never computed — and
this is not a question of poor overlap between prints. The sensor simply
does not clear the floor.

Measured minutiae per capture, ten samples (`tools/fpc_minutiae.c`):

```
11, 8, 8, 8, 8, 7, 0, 0, 0     one of ten reaches 10
```

Three placements yielded *no* minutiae at all. Three captures of one
unmoved finger gave 8, 8, 8 with positions stable to within a few pixels,
so extraction is repeatable and these are real features, not noise — there
are just very few of them. 160x160 at 508 dpi is 8x8mm, and ~8 minutiae is
about what that area of a finger holds.

Ruled out along the way: contrast processing (normalize, equalize,
auto-level all leave the count unchanged) and a resolution mismatch — the
ridge period measures 10px, i.e. 0.50mm at 508dpi, which is the normal human
value and confirms both the scale NBIS assumes and the `ppmm` set above.

Upscaling, as `elanspi.c` does with `fpi_image_resize (img, 2, 2)`, changes
counts wildly and inconsistently: one image went 7 -> 67 minutiae at 2x,
another 11 -> 2 at 3x. Interpolation cannot add information, so most of that
is likely spurious. **Counting minutiae is the wrong measurement** — false
minutiae inflate the count and hurt matching. What matters is whether two
captures of the same finger score against each other, and that needs an
offline matching harness (libfprint's `virtual-image` driver, not currently
built here) rather than an enroll/verify cycle against a real finger.

Where this leaves the driver: capture and enrollment work; verification
cannot until captures reliably clear 10 minutiae. Image-quality gating would
remove the zero-minutiae frames and the 2-minutia print now in the template,
which is necessary but may not be sufficient, since the best sample seen is
11.

### Enlargement vs match score, measured

| factor | delivered | best bozorth3 score |
|---|---|---|
| 1x | 160x160 | 0 — under MIN_COMPUTABLE_BOZORTH_MINUTIAE, never computed |
| **2x** | 320x320 | **15** |
| 3x | 480x480 | 10 |
| 4x | 640x640 | 0 |

Threshold is 24; `aes3k` settles for 9. At 3x the distribution over 25
comparisons was `0 x18, 3 x3, 6 x1, 9 x1, 11 x1, 12 x1` — three would clear
aes3k's bar, none clear ours.

**2x is the optimum**, and the numbers above supersede an earlier live
comparison that appeared to favour 3x. That comparison re-enrolled and
re-pressed the finger between factors, so it varied conditions as much as
factors; these come from `tools/fpc_bench.c` replaying the same ten saved
captures through the same pipeline, with only the factor changing.

What goes wrong past 2x is visible on three captures of a finger that was
never lifted between them:

| factor | minutiae | scores against each other |
|---|---|---|
| 1x | 8, 8, 8 | all 0 (under the floor) |
| **2x** | 11, 20, 23 | **6, 7, 15** |
| 3x | 4, 21, 41 | all 0 |
| 4x | 17, 7, 20 | all 0 |

At 1x the counts are stable but too low. At 2x they clear the floor and stay
consistent. Past that the count keeps climbing while consistency collapses —
4 against 41 minutiae from near-identical images — because interpolation is
amplifying sensor noise into minutiae that differ between two views of the
same finger. More minutiae that do not correspond is worse than fewer that
do, which is why counting minutiae was the wrong measurement all along.

Enlargement alone is spent as a lever at 15 — but the *interpolation* is not.

### Interpolation: Catmull-Rom instead of bilinear

`fpi_image_resize()` interpolates bilinearly, which softens ridge edges
exactly where NBIS looks for them. Swapping the filter at the same 2x, over
the same ten captures:

| filter | best | mean | pairs at or above 24 |
|---|---|---|---|
| bilinear (`fpi_image_resize`) | 15 | 4.0 | 0 |
| Point / nearest | 15 | 3.1 | 0 |
| Mitchell | 20 | 2.7 | 0 |
| Lanczos | 22 | 2.7 | 0 |
| **Catmull-Rom** | **25** | 2.8 | **2** |

Catmull-Rom is the only configuration tried that puts any pair at or above
the threshold — the first genuine matches this driver has produced. It is
also the principled choice: it is the interpolating cubic, so it passes
through the original samples rather than averaging them away, and its slight
overshoot at an edge sharpens a ridge boundary instead of rounding it off.

Larger targets do not help with it either — Catmull-Rom at 400px scores 9 and
at 480px scores 11, against 25 at 320px — which is the same 2x optimum found
above, now confirmed under a second filter.

The driver carries its own `fpc_resize_catrom()` rather than calling
`fpi_image_resize()`. Making the shared helper offer a sharper filter would
be the better fix, and is worth proposing upstream.

### Sharpening before enlargement

A user observation pointed at this one: `fprintd-verify` returned no-match
*instantly*, as though nothing were really being compared. It wasn't. The
enrolled template was healthy — 35, 51, 29, 65 and 24 minutiae, all five
clear of the floor — so the short-circuit had to be on the other side: the
verification image was landing under 10 minutiae, and bozorth3 was returning
zero without comparing anything.

That asymmetry has a cause. **Enrollment quietly selects good frames** — a
stage only passes if libfprint accepts the image, so poor ones are retried —
while a verification is scored on whatever arrives. The verification image is
the one that needs help.

An unsharp mask on the raw frame, before enlargement, provides it. Real
frames go from 8–11 minutiae to 13–148, so none fall under the floor any
more, and the share of captures that match another capture of the same finger
roughly triples:

| configuration | captures matching another | mean best score per capture |
|---|---|---|
| enlargement only | 2 of 11 | 7.0 |
| + unsharp (sigma 1.5, amount 2.5) | **6 of 11** | **17.4** |

The parameters come from a grid over both, and sit on a plateau rather than a
spike. Past sigma 2.0 the score collapses to 0–2 of 11, which is the
mechanism showing itself: blur too much before sharpening and there is
nothing left to sharpen.

The gate still runs on the *unsharpened* frame, where a blank frame is
unambiguous and where its threshold was calibrated.

Ten samples is a small dataset and the optimum is loosely determined; the
plateau is the reassuring part, not the peak.

### `fprintd-identify` appears to hang

It is not hanging on the driver. `fprintd-identify` keeps capturing until it
recognises a finger, so while matching fails it never stops — and libfprint's
thermal model then disables the device:

```
Updated temperature model ... FP_TEMPERATURE_HOT
Device reported an error during verify: Device disabled to prevent overheating.
verify_cb: result verify-disconnected
```

`DEFAULT_TEMP_HOT_SECONDS` is `3 * 60` in `fp-device-private.h`: three
minutes of continuous activity disables any device, driver-independent.
Cooling back to cold takes `DEFAULT_TEMP_COLD_SECONDS`, nine minutes —
or restart `fprintd`, since the model is per-process state.

While matching is unreliable, use `fprintd-verify`, which captures once and
reports, rather than `fprintd-identify`, which loops. The same session
confirmed the drain still doing its job under a long run:
`drained 413 stale packet(s) before capture`.

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
